/*
 * nptest — the NETPOSIX gate program (U10, tools/netposix_test.sh).
 * A musl static-PIE built with tools/secos-cc. Every check prints
 * "NP PASS <name>" or "NP FAIL <name>: <why>"; a battery ends with
 * "NP DONE <battery> pass=N fail=M" and exits non-zero on any failure.
 * SPDX-License-Identifier: MIT
 *
 *   nptest net HOST      AF_INET against the host services the driver runs
 *                        (tools/netposix_drive.py): TCP echo, refused port,
 *                        non-blocking connect + poll + SO_ERROR, UDP echo,
 *                        ICMP port unreachable (ECONNREFUSED, IP_RECVERR),
 *                        socket options, timeouts, MSG_* flags, DNS through
 *                        musl's resolver, ping sockets (identifier isolation,
 *                        echo replies, ICMP net unreachable)
 *   nptest local DIR     AF_UNIX (stream, dgram, socketpair, abstract names,
 *                        SO_PEERCRED), epoll (also with every free fd),
 *                        eventfd, timerfd, setitimer/alarm
 *   nptest userver PATH  bind PATH (current umask), echo one connection
 *   nptest uconnect PATH connect to PATH, print "NP UCONNECT errno=N"
 *   nptest raw HOST      try SOCK_RAW ICMP: "NP RAW errno=N" (0 = allowed, an
 *                        echo to HOST answered through the raw socket)
 *   nptest server PORT N accept N TCP connections (accept4), echo each
 *   nptest nonet         socket(AF_INET) from a program without CAP_NET
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
/* <linux/errqueue.h> (Linux UAPI, not in the musl sysroot) */
struct sock_extended_err { uint32_t ee_errno; uint8_t ee_origin, ee_type, ee_code, ee_pad; uint32_t ee_info, ee_data; };
#define SO_EE_ORIGIN_ICMP 2
#define SO_EE_OFFENDER(ee) ((struct sockaddr *)((ee) + 1))

#define P_TCP_ECHO   7001
#define P_UDP_ECHO   7002
#define P_TCP_CLOSED 7009
#define P_UDP_CLOSED 7010

static int g_pass, g_fail;
static void result(const char *name, int ok, const char *why)
{
    if (ok) printf("NP PASS %s\n", name), g_pass++;
    else printf("NP FAIL %s: %s\n", name, why ? why : "?"), g_fail++;
    fflush(stdout);
}
#define CHECK(name, cond, ...) do { char _w[200]; snprintf(_w, sizeof _w, __VA_ARGS__); result(name, (cond), _w); } while (0)

static long now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
static struct sockaddr_in sin_of(const char *ip, int port)
{
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port); inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}
static int readn(int fd, void *b, int n)
{
    int got = 0;
    while (got < n) { int r = read(fd, (char *)b + got, n - got); if (r <= 0) return got ? got : r; got += r; }
    return got;
}
static int writen(int fd, const void *b, int n)
{
    int put = 0;
    while (put < n) { int r = write(fd, (const char *)b + put, n - put); if (r <= 0) return put ? put : r; put += r; }
    return put;
}
static unsigned short cksum(const void *p, int n)
{
    const unsigned char *b = p; unsigned s = 0;
    for (int i = 0; i + 1 < n; i += 2) s += (b[i] << 8) | b[i + 1];
    if (n & 1) s += b[n - 1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (unsigned short)~s;
}

/* ================================================================ AF_INET */
static void t_tcp(const char *host)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = sin_of(host, P_TCP_ECHO);
    int r = connect(fd, (struct sockaddr *)&a, sizeof a);
    CHECK("tcp connect (blocking)", fd >= 0 && r == 0, "fd=%d r=%d errno=%d", fd, r, errno);
    if (r) { close(fd); return; }
    CHECK("SOCK_CLOEXEC sets FD_CLOEXEC", fcntl(fd, F_GETFD) == FD_CLOEXEC, "getfd=%d", fcntl(fd, F_GETFD));
    struct sockaddr_in me, peer; socklen_t ml = sizeof me, pl = sizeof peer;
    r = getsockname(fd, (struct sockaddr *)&me, &ml) | getpeername(fd, (struct sockaddr *)&peer, &pl);
    CHECK("getsockname/getpeername", r == 0 && ml == sizeof me && peer.sin_port == htons(P_TCP_ECHO) &&
          peer.sin_addr.s_addr == a.sin_addr.s_addr && me.sin_port != 0 && me.sin_addr.s_addr != 0,
          "r=%d port=%d", r, ntohs(peer.sin_port));
    int one = 1;
    CHECK("TCP_NODELAY set/get", setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) == 0 &&
          ({ int v = 0; socklen_t l = sizeof v; getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, &l) == 0 && v == 1; }), "errno=%d", errno);
    static unsigned char out[65536], in[65536];
    for (int i = 0; i < (int)sizeof out; i++) out[i] = (unsigned char)(i * 7 + 3);
    pid_t w = fork();
    if (w == 0) { writen(fd, out, sizeof out); _exit(0); }
    int got = readn(fd, in, sizeof in);
    waitpid(w, 0, 0);
    CHECK("tcp 64 KiB echo round trip", got == (int)sizeof in && !memcmp(in, out, sizeof in), "got=%d", got);
    /* MSG_PEEK then MSG_WAITALL */
    send(fd, "peekdata", 8, 0);
    char pk[16] = {0}, rd[16] = {0};
    int p1 = recv(fd, pk, 8, MSG_PEEK | MSG_WAITALL);
    int p2 = recv(fd, rd, 8, MSG_WAITALL);
    CHECK("MSG_PEEK leaves data, MSG_WAITALL reads it all", p1 == 8 && p2 == 8 && !memcmp(pk, "peekdata", 8) && !memcmp(rd, "peekdata", 8),
          "p1=%d p2=%d", p1, p2);
    /* FIONREAD */
    send(fd, "xyz", 3, 0);
    int avail = 0; long t0 = now_ms();
    while (avail < 3 && now_ms() - t0 < 3000) { ioctl(fd, FIONREAD, &avail); usleep(2000); }
    char x[3]; recv(fd, x, 3, 0);
    CHECK("FIONREAD counts buffered bytes", avail == 3, "avail=%d", avail);
    /* MSG_DONTWAIT on an empty socket */
    r = recv(fd, x, 1, MSG_DONTWAIT);
    CHECK("MSG_DONTWAIT -> EAGAIN", r == -1 && errno == EAGAIN, "r=%d errno=%d", r, errno);
    /* SO_RCVTIMEO */
    struct timeval tv = { 0, 300000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    t0 = now_ms(); r = recv(fd, x, 1, 0); long dt = now_ms() - t0;
    CHECK("SO_RCVTIMEO 300 ms -> EAGAIN on time", r == -1 && errno == EAGAIN && dt >= 250 && dt < 2000, "r=%d errno=%d dt=%ld", r, errno, dt);
    t0 = now_ms(); r = read(fd, x, 1); dt = now_ms() - t0;
    CHECK("SO_RCVTIMEO applies to read() too", r == -1 && errno == EAGAIN && dt >= 250 && dt < 2000, "r=%d errno=%d dt=%ld", r, errno, dt);
    /* shutdown(SHUT_WR): the echo server sees EOF and closes, we read EOF */
    r = shutdown(fd, SHUT_WR);
    tv.tv_sec = 5; tv.tv_usec = 0; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int e = recv(fd, x, 1, 0);
    CHECK("shutdown(SHUT_WR): FIN sent, peer's close read as EOF", r == 0 && e == 0, "r=%d e=%d errno=%d", r, e, errno);
    r = send(fd, "z", 1, MSG_NOSIGNAL);
    CHECK("send after SHUT_WR -> EPIPE (MSG_NOSIGNAL: no SIGPIPE)", r == -1 && errno == EPIPE, "r=%d errno=%d", r, errno);
    close(fd);
}

static void t_nbconnect(const char *host)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    CHECK("SOCK_NONBLOCK sets O_NONBLOCK", fd >= 0 && (fcntl(fd, F_GETFL) & O_NONBLOCK), "fl=%x", fcntl(fd, F_GETFL));
    struct sockaddr_in a = sin_of(host, P_TCP_ECHO);
    int r = connect(fd, (struct sockaddr *)&a, sizeof a);
    CHECK("non-blocking connect -> EINPROGRESS", r == -1 && errno == EINPROGRESS, "r=%d errno=%d", r, errno);
    struct pollfd p = { fd, POLLOUT, 0 };
    r = poll(&p, 1, 5000);
    int err = -1; socklen_t l = sizeof err;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
    CHECK("poll POLLOUT + SO_ERROR == 0 once connected", r == 1 && (p.revents & POLLOUT) && err == 0, "r=%d rev=%x err=%d", r, p.revents, err);
    char b[5];
    send(fd, "hello", 5, 0);
    p.events = POLLIN; poll(&p, 1, 5000);
    r = recv(fd, b, 5, 0);
    CHECK("non-blocking socket echo", r == 5 && !memcmp(b, "hello", 5), "r=%d", r);
    close(fd);

    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    a = sin_of(host, P_TCP_CLOSED);
    r = connect(fd, (struct sockaddr *)&a, sizeof a);
    p.fd = fd; p.events = POLLOUT; p.revents = 0;
    int pr = poll(&p, 1, 5000);
    err = 0; l = sizeof err;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
    CHECK("refused port: non-blocking connect, poll, SO_ERROR = ECONNREFUSED",
          r == -1 && errno == EINPROGRESS && pr == 1 && err == ECONNREFUSED, "pr=%d rev=%x err=%d", pr, p.revents, err);
    close(fd);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    r = connect(fd, (struct sockaddr *)&a, sizeof a);
    CHECK("refused port: blocking connect -> ECONNREFUSED", r == -1 && errno == ECONNREFUSED, "r=%d errno=%d", r, errno);
    close(fd);
}

static void t_udp(const char *host)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = sin_of(host, P_UDP_ECHO), from; socklen_t fl = sizeof from;
    int one = 1;
    setsockopt(fd, IPPROTO_IP, IP_RECVTTL, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &one, sizeof one);
    struct timeval tv = { 3, 0 }; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int r = sendto(fd, "ping-udp", 8, 0, (struct sockaddr *)&a, sizeof a);
    char b[64] = {0};
    int pk = recvfrom(fd, b, sizeof b, MSG_PEEK, (struct sockaddr *)&from, &fl);
    char cb[256]; struct iovec iov = { b, sizeof b };
    struct msghdr mh = { .msg_name = &from, .msg_namelen = sizeof from, .msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = cb, .msg_controllen = sizeof cb };
    int n = recvmsg(fd, &mh, 0);
    int ttl = -1, ts = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TTL) ttl = *(int *)CMSG_DATA(c);
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMP) ts = ((struct timeval *)CMSG_DATA(c))->tv_sec > 0;
    }
    CHECK("udp sendto/recvfrom echo + MSG_PEEK", r == 8 && pk == 8 && n == 8 && !memcmp(b, "ping-udp", 8) &&
          from.sin_port == htons(P_UDP_ECHO), "r=%d pk=%d n=%d", r, pk, n);
    CHECK("recvmsg cmsgs: IP_TTL and SO_TIMESTAMP", ttl > 0 && ttl <= 255 && ts, "ttl=%d ts=%d", ttl, ts);
    /* truncation */
    sendto(fd, "0123456789", 10, 0, (struct sockaddr *)&a, sizeof a);
    mh.msg_controllen = 0; mh.msg_control = NULL; iov.iov_len = 4;
    n = recvmsg(fd, &mh, 0);
    CHECK("datagram truncation sets MSG_TRUNC", n == 4 && (mh.msg_flags & MSG_TRUNC), "n=%d flags=%x", n, mh.msg_flags);
    sendto(fd, "0123456789", 10, 0, (struct sockaddr *)&a, sizeof a);
    n = recv(fd, b, 4, MSG_TRUNC);
    CHECK("recv(MSG_TRUNC) returns the datagram length", n == 10, "n=%d", n);
    /* connected UDP */
    r = connect(fd, (struct sockaddr *)&a, sizeof a);
    send(fd, "conn", 4, 0);
    n = recv(fd, b, sizeof b, 0);
    CHECK("connected udp send/recv", r == 0 && n == 4 && !memcmp(b, "conn", 4), "r=%d n=%d", r, n);
    close(fd);

    /* ICMP port unreachable: connected socket -> ECONNREFUSED; IP_RECVERR queue */
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in z = sin_of(host, P_UDP_CLOSED);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    connect(fd, (struct sockaddr *)&z, sizeof z);
    setsockopt(fd, IPPROTO_IP, IP_RECVERR, &one, sizeof one);
    send(fd, "anyone?", 7, 0);
    r = recv(fd, b, sizeof b, 0);
    int e1 = errno;
    char eb[64]; struct sockaddr_in orig; struct iovec ei = { eb, sizeof eb };
    struct msghdr em = { .msg_name = &orig, .msg_namelen = sizeof orig, .msg_iov = &ei, .msg_iovlen = 1,
                         .msg_control = cb, .msg_controllen = sizeof cb };
    n = recvmsg(fd, &em, MSG_ERRQUEUE);
    struct sock_extended_err *ee = NULL;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&em); c; c = CMSG_NXTHDR(&em, c))
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_RECVERR) ee = (struct sock_extended_err *)CMSG_DATA(c);
    CHECK("ICMP port unreachable -> ECONNREFUSED on the connected socket", r == -1 && e1 == ECONNREFUSED, "r=%d errno=%d", r, e1);
    CHECK("IP_RECVERR: MSG_ERRQUEUE has the ICMP error (origin ICMP, 3/3, offender)",
          n >= 0 && ee && ee->ee_errno == ECONNREFUSED && ee->ee_origin == SO_EE_ORIGIN_ICMP && ee->ee_type == 3 &&
          ee->ee_code == 3 && ((struct sockaddr_in *)SO_EE_OFFENDER(ee))->sin_addr.s_addr != 0 &&
          (em.msg_flags & MSG_ERRQUEUE) && orig.sin_port == htons(P_UDP_CLOSED),
          "n=%d ee=%p errno=%d type=%d code=%d", n, (void *)ee, ee ? (int)ee->ee_errno : -1, ee ? ee->ee_type : -1, ee ? ee->ee_code : -1);
    n = recvmsg(fd, &em, MSG_ERRQUEUE);
    CHECK("error queue drained -> EAGAIN", n == -1 && errno == EAGAIN, "n=%d errno=%d", n, errno);
    close(fd);
}

static void t_sockopt(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0), v = 0; socklen_t l = sizeof v;
    CHECK("SO_TYPE / SO_DOMAIN / SO_PROTOCOL",
          !getsockopt(fd, SOL_SOCKET, SO_TYPE, &v, &l) && v == SOCK_STREAM &&
          !getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &v, &l) && v == AF_INET &&
          !getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &v, &l) && v == IPPROTO_TCP, "v=%d", v);
    int one = 1;
    CHECK("SO_REUSEADDR / SO_KEEPALIVE round trip",
          !setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) &&
          !getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &v, &l) && v == 1 &&
          !setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one) &&
          !getsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &v, &l) && v == 1, "errno=%d", errno);
    v = 0;
    CHECK("SO_RCVBUF / SO_SNDBUF readable", !getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, &l) && v > 0 &&
          !getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, &l) && v > 0, "v=%d", v);
    int r = setsockopt(fd, SOL_SOCKET, 0x7777, &one, sizeof one);
    CHECK("unknown option -> ENOPROTOOPT", r == -1 && errno == ENOPROTOOPT, "r=%d errno=%d", r, errno);
    struct linger lg = { 1, 0 };
    CHECK("SO_LINGER set/get", !setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg) &&
          ({ struct linger g; socklen_t gl = sizeof g; !getsockopt(fd, SOL_SOCKET, SO_LINGER, &g, &gl) && g.l_onoff == 1; }), "errno=%d", errno);
    close(fd);
    fd = socket(AF_INET6, SOCK_STREAM, 0);
    CHECK("AF_INET6 -> EAFNOSUPPORT (musl falls back to IPv4)", fd == -1 && errno == EAFNOSUPPORT, "fd=%d errno=%d", fd, errno);
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = sin_of("0.0.0.0", 0); socklen_t al = sizeof a;
    r = bind(fd, (struct sockaddr *)&a, sizeof a);
    getsockname(fd, (struct sockaddr *)&a, &al);
    CHECK("bind port 0 picks an ephemeral port", r == 0 && ntohs(a.sin_port) >= 1024, "r=%d port=%d", r, ntohs(a.sin_port));
    int fd2 = socket(AF_INET, SOCK_DGRAM, 0);
    r = bind(fd2, (struct sockaddr *)&a, sizeof a);
    CHECK("second bind to the same port -> EADDRINUSE", r == -1 && errno == EADDRINUSE, "r=%d errno=%d", r, errno);
    close(fd); close(fd2);
    if (getuid() != 0) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        a = sin_of("0.0.0.0", 80);
        r = bind(fd, (struct sockaddr *)&a, sizeof a);
        CHECK("port < 1024 needs root -> EACCES", r == -1 && errno == EACCES, "r=%d errno=%d", r, errno);
        close(fd);
    }
}

static void t_dns(void)
{
    struct addrinfo h = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    int r = getaddrinfo("np-test.secos", "80", &h, &res);
    char ip[32] = "-";
    if (!r && res) inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, sizeof ip);
    CHECK("getaddrinfo via musl's resolver (stub DNS on the host)", r == 0 && !strcmp(ip, "10.9.8.7"), "r=%d (%s) ip=%s", r, gai_strerror(r), ip);
    if (res) freeaddrinfo(res);
    res = NULL;
    r = getaddrinfo("big.secos", NULL, &h, &res);
    int n = 0; for (struct addrinfo *p = res; p; p = p->ai_next) n++;
    CHECK("truncated UDP answer -> TCP fallback (many A records)", r == 0 && n >= 20, "r=%d n=%d", r, n);
    if (res) freeaddrinfo(res);
    res = NULL;
    r = getaddrinfo("nosuch.secos", NULL, &h, &res);
    CHECK("NXDOMAIN -> EAI_NONAME", r == EAI_NONAME, "r=%d", r);
    res = NULL;
    r = getaddrinfo("hosts-entry.secos", NULL, &h, &res);
    ip[0] = 0;
    if (!r && res) inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, sizeof ip);
    CHECK("/etc/hosts entry resolved", r == 0 && !strcmp(ip, "10.1.2.3"), "r=%d ip=%s", r, ip);
    if (res) freeaddrinfo(res);
    struct servent *se = getservbyname("ssh", "tcp");
    struct protoent *pe = getprotobyname("icmp");
    CHECK("/etc/services and /etc/protocols", se && ntohs(se->s_port) == 22 && pe && pe->p_proto == 1,
          "se=%p pe=%p", (void *)se, (void *)pe);
}

/* ICMP ping sockets */
static int ping_once(int fd, const char *host, int seq, int *rid, int timeout_ms)
{
    struct sockaddr_in a = sin_of(host, 0);
    unsigned char pk[64]; memset(pk, 0, sizeof pk);
    pk[0] = ICMP_ECHO; pk[6] = (unsigned char)(seq >> 8); pk[7] = (unsigned char)seq;
    pk[4] = 0x12; pk[5] = 0x34;                    /* the kernel replaces it */
    memcpy(pk + 8, "secos-np", 8);
    if (sendto(fd, pk, sizeof pk, 0, (struct sockaddr *)&a, sizeof a) != sizeof pk) return -1;
    long t0 = now_ms();
    while (now_ms() - t0 < timeout_ms) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 200) <= 0) continue;
        unsigned char in[256];
        int n = recv(fd, in, sizeof in, 0);
        if (n < 0 && errno != EAGAIN && errno != EINTR) return -5;   /* an ICMP error (sk_err) */
        if (n < 8) continue;
        if (in[0] != ICMP_ECHOREPLY) return -2;
        if (((in[6] << 8) | in[7]) != seq) continue;
        *rid = (in[4] << 8) | in[5];
        return memcmp(in + 8, "secos-np", 8) ? -3 : 0;
    }
    return -4;
}

static void t_ping(const char *host)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    CHECK("ping socket (SOCK_DGRAM, IPPROTO_ICMP) without privileges", fd >= 0, "errno=%d", errno);
    if (fd < 0) return;
    int rid = -1;
    int r = ping_once(fd, host, 1, &rid, 3000);
    struct sockaddr_in me; socklen_t ml = sizeof me;
    getsockname(fd, (struct sockaddr *)&me, &ml);
    CHECK("echo reply received, identifier = the socket's (kernel-chosen)", r == 0 && rid == ntohs(me.sin_port) && rid != 0x1234,
          "r=%d rid=%d port=%d", r, rid, ntohs(me.sin_port));
    unsigned char bad[16] = { 13, 0 };                    /* ICMP timestamp request */
    struct sockaddr_in a = sin_of(host, 0);
    r = sendto(fd, bad, sizeof bad, 0, (struct sockaddr *)&a, sizeof a);
    CHECK("ping socket refuses non-echo ICMP (EINVAL)", r == -1 && errno == EINVAL, "r=%d errno=%d", r, errno);
    /* identifier isolation: two sockets ping concurrently, each gets only its own */
    int f2 = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    struct sockaddr_in b1 = sin_of("0.0.0.0", 4242);
    r = bind(f2, (struct sockaddr *)&b1, sizeof b1);
    int f3 = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    int r3 = bind(f3, (struct sockaddr *)&b1, sizeof b1);
    CHECK("bind chooses the identifier; a taken one -> EADDRINUSE", r == 0 && r3 == -1 && errno == EADDRINUSE, "r=%d r3=%d errno=%d", r, r3, errno);
    close(f3);
    int okc = 1, cross = 0;
    pid_t ch = fork();
    if (ch == 0) {
        int bad = 0;
        for (int i = 0; i < 10; i++) { int id = -1; if (ping_once(f2, host, 100 + i, &id, 3000) || id != 4242) bad++; }
        _exit(bad);
    }
    for (int i = 0; i < 10; i++) {
        int id = -1;
        if (ping_once(fd, host, 200 + i, &id, 3000)) okc = 0;
        if (id == 4242) cross++;
    }
    int st = 0; waitpid(ch, &st, 0);
    /* Nothing of the other socket's left in ours (the other used seq 100+). */
    int stray = 0;
    for (;;) { unsigned char in[128]; int n = recv(fd, in, sizeof in, MSG_DONTWAIT); if (n < 0) break; if (n >= 8 && ((in[4] << 8) | in[5]) == 4242) stray++; }
    CHECK("two concurrent pings: each socket sees only its identifier's replies",
          okc && cross == 0 && stray == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0, "ok=%d cross=%d stray=%d child=%x", okc, cross, stray, st);
    close(f2);
    /* ICMP error to a ping socket: slirp answers "net unreachable" for 192.0.2.1 */
    int one = 1;
    setsockopt(fd, IPPROTO_IP, IP_RECVERR, &one, sizeof one);
    r = ping_once(fd, "192.0.2.1", 7, &rid, 4000);
    int e1 = errno;
    char cb[256], eb[64]; struct sockaddr_in orig; struct iovec ei = { eb, sizeof eb };
    struct msghdr em = { .msg_name = &orig, .msg_namelen = sizeof orig, .msg_iov = &ei, .msg_iovlen = 1,
                         .msg_control = cb, .msg_controllen = sizeof cb };
    int n = recvmsg(fd, &em, MSG_ERRQUEUE | MSG_DONTWAIT);
    struct sock_extended_err *ee = NULL;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&em); c; c = CMSG_NXTHDR(&em, c))
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_RECVERR) ee = (struct sock_extended_err *)CMSG_DATA(c);
    CHECK("ICMP net unreachable delivered to the ping socket (IP_RECVERR)",
          r == -5 && e1 == ENETUNREACH && n >= 8 && ee && ee->ee_type == 3 && ee->ee_origin == SO_EE_ORIGIN_ICMP &&
          ee->ee_errno == ENETUNREACH && ((unsigned char *)eb)[0] == ICMP_ECHO &&
          orig.sin_addr.s_addr == inet_addr("192.0.2.1"),
          "r=%d errno=%d n=%d ee=%p type=%d", r, e1, n, (void *)ee, ee ? ee->ee_type : -1);
    int ttl = 1;
    CHECK("IP_TTL settable on a ping socket", !setsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, sizeof ttl), "errno=%d", errno);
    close(fd);
}

static int t_raw(const char *host)
{
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) { printf("NP RAW errno=%d\n", errno); return errno; }
    unsigned char pk[24]; memset(pk, 0, sizeof pk);
    pk[0] = ICMP_ECHO; pk[4] = 0x5e; pk[5] = 0xc0; pk[7] = 1; memcpy(pk + 8, "rawsecos", 8);
    unsigned short c = cksum(pk, sizeof pk); pk[2] = (unsigned char)(c >> 8); pk[3] = (unsigned char)c;
    struct sockaddr_in a = sin_of(host, 0);
    int ok = sendto(fd, pk, sizeof pk, 0, (struct sockaddr *)&a, sizeof a) == (int)sizeof pk;
    int got = 0; long t0 = now_ms();
    while (ok && !got && now_ms() - t0 < 3000) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 200) <= 0) continue;
        unsigned char in[256]; int n = recv(fd, in, sizeof in, 0);
        int hl = (in[0] & 15) * 4;                       /* raw sockets see the IP header */
        if (n >= hl + 16 && in[9] == IPPROTO_ICMP && in[hl] == ICMP_ECHOREPLY && in[hl + 4] == 0x5e &&
            in[hl + 5] == 0xc0 && !memcmp(in + hl + 8, "rawsecos", 8)) got = 1;
    }
    printf("NP RAW errno=0 reply=%d\n", got);
    close(fd);
    return got ? 0 : 100;
}

/* ================================================================ local */
static void t_unix(const char *dir)
{
    char path[100]; snprintf(path, sizeof path, "%.80s/np.sock", dir);
    unlink(path);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX; snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    int r = bind(ls, (struct sockaddr *)&a, sizeof a);
    struct stat st; int se = stat(path, &st);
    CHECK("AF_UNIX bind creates the socket file", r == 0 && se == 0, "r=%d errno=%d", r, errno);
    int r2 = bind(socket(AF_UNIX, SOCK_STREAM, 0), (struct sockaddr *)&a, sizeof a);
    CHECK("bind to an existing name -> EADDRINUSE", r2 == -1 && errno == EADDRINUSE, "r=%d errno=%d", r2, errno);
    listen(ls, 8);
    pid_t ch = fork();
    if (ch == 0) {
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(c, (struct sockaddr *)&a, sizeof a)) _exit(2);
        static char buf[100000];
        for (int i = 0; i < (int)sizeof buf; i++) buf[i] = (char)(i * 13);
        if (writen(c, buf, sizeof buf) != (int)sizeof buf) _exit(3);
        shutdown(c, SHUT_WR);
        char ack[3]; if (readn(c, ack, 3) != 3 || memcmp(ack, "ack", 3)) _exit(4);
        _exit(0);
    }
    struct sockaddr_un peer; socklen_t pl = sizeof peer;
    int s = accept4(ls, (struct sockaddr *)&peer, &pl, SOCK_CLOEXEC);
    struct ucred cr; socklen_t cl = sizeof cr;
    int cg = getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &cl);
    CHECK("accept4 + SO_PEERCRED = the connecting process", s >= 0 && cg == 0 && cr.pid == ch && cr.uid == getuid(),
          "s=%d cg=%d pid=%d/%d", s, cg, cr.pid, ch);
    static char got[100000];
    int n = readn(s, got, sizeof got);
    char x; int eof = read(s, &x, 1);
    int good = n == (int)sizeof got; for (int i = 0; good && i < n; i++) if (got[i] != (char)(i * 13)) good = 0;
    write(s, "ack", 3);
    int stc = 0; waitpid(ch, &stc, 0);
    CHECK("AF_UNIX stream: 100 KB intact, shutdown -> EOF", good && eof == 0 && WIFEXITED(stc) && WEXITSTATUS(stc) == 0,
          "n=%d eof=%d child=%x", n, eof, stc);
    close(s);
    /* peer closed: read EOF, write EPIPE (+ SIGPIPE ignored here) */
    int sv[2];
    r = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    write(sv[0], "pair", 4);
    char pb[4]; int pr = read(sv[1], pb, 4);
    close(sv[0]);
    int e = read(sv[1], pb, 4);
    signal(SIGPIPE, SIG_IGN);
    int w = write(sv[1], "x", 1);
    CHECK("socketpair: data, EOF after close, EPIPE", r == 0 && pr == 4 && e == 0 && w == -1 && errno == EPIPE, "r=%d pr=%d e=%d w=%d errno=%d", r, pr, e, w, errno);
    close(sv[1]);
    /* datagrams with names + abstract name */
    int d1 = socket(AF_UNIX, SOCK_DGRAM, 0), d2 = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un an; memset(&an, 0, sizeof an); an.sun_family = AF_UNIX; memcpy(an.sun_path, "\0np-abstract", 12);
    socklen_t al = offsetof(struct sockaddr_un, sun_path) + 12;
    r = bind(d1, (struct sockaddr *)&an, al);
    struct sockaddr_un auto_; memset(&auto_, 0, sizeof auto_); auto_.sun_family = AF_UNIX;
    int r3 = bind(d2, (struct sockaddr *)&auto_, sizeof(sa_family_t));    /* autobind */
    sendto(d2, "dg1", 3, 0, (struct sockaddr *)&an, al);
    sendto(d2, "dg22", 4, 0, (struct sockaddr *)&an, al);
    struct sockaddr_un fr; socklen_t frl = sizeof fr; char db[8];
    int m1 = recvfrom(d1, db, sizeof db, 0, (struct sockaddr *)&fr, &frl);
    int m2 = recv(d1, db, sizeof db, 0);
    CHECK("AF_UNIX dgram: abstract name, autobind sender, boundaries kept",
          r == 0 && r3 == 0 && m1 == 3 && m2 == 4 && frl == offsetof(struct sockaddr_un, sun_path) + 6 && fr.sun_path[0] == 0,
          "r=%d r3=%d m1=%d m2=%d frl=%u", r, r3, m1, m2, (unsigned)frl);
    struct sockaddr_un none; memset(&none, 0, sizeof none); none.sun_family = AF_UNIX; memcpy(none.sun_path, "\0nobody-here", 12);
    r = sendto(d2, "x", 1, 0, (struct sockaddr *)&none, al);
    CHECK("dgram to an unbound name -> ECONNREFUSED", r == -1 && errno == ECONNREFUSED, "r=%d errno=%d", r, errno);
    close(d1); close(d2);
    /* connect to a path nobody listens on any more */
    close(ls);
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    r = connect(c, (struct sockaddr *)&a, sizeof a);
    CHECK("connect to a closed socket's file -> ECONNREFUSED", r == -1 && errno == ECONNREFUSED, "r=%d errno=%d", r, errno);
    close(c);
    unlink(path);
    c = socket(AF_UNIX, SOCK_STREAM, 0);
    r = connect(c, (struct sockaddr *)&a, sizeof a);
    CHECK("connect to a missing path -> ENOENT", r == -1 && errno == ENOENT, "r=%d errno=%d", r, errno);
    close(c);
}

static void t_epoll(void)
{
    int ep = epoll_create1(EPOLL_CLOEXEC);
    CHECK("epoll_create1", ep >= 0, "errno=%d", errno);
    int pfd[2]; pipe(pfd);
    int ev = eventfd(0, EFD_NONBLOCK);
    struct epoll_event e = { .events = EPOLLIN, .data.u64 = 0xA1 };
    epoll_ctl(ep, EPOLL_CTL_ADD, pfd[0], &e);
    e.data.u64 = 0xE7; epoll_ctl(ep, EPOLL_CTL_ADD, ev, &e);
    struct epoll_event out[8];
    int n = epoll_wait(ep, out, 8, 0);
    CHECK("epoll_wait: nothing ready", n == 0, "n=%d", n);
    pid_t ch = fork();
    if (ch == 0) { usleep(100000); write(pfd[1], "p", 1); _exit(0); }
    long t0 = now_ms();
    n = epoll_wait(ep, out, 8, 3000);
    long dt = now_ms() - t0;
    waitpid(ch, 0, 0);
    CHECK("epoll_wait blocks until a pipe write (woken, not timed out)", n == 1 && out[0].data.u64 == 0xA1 && dt >= 80 && dt < 2500, "n=%d dt=%ld", n, dt);
    uint64_t one = 1; write(ev, &one, 8);
    n = epoll_wait(ep, out, 8, 100);
    CHECK("pipe + eventfd both ready", n == 2, "n=%d", n);
    int r = epoll_ctl(ep, EPOLL_CTL_ADD, ev, &e);
    CHECK("EPOLL_CTL_ADD twice -> EEXIST", r == -1 && errno == EEXIST, "r=%d errno=%d", r, errno);
    e.events = EPOLLIN | EPOLLONESHOT; e.data.u64 = 0xE7;
    epoll_ctl(ep, EPOLL_CTL_MOD, ev, &e);
    epoll_ctl(ep, EPOLL_CTL_DEL, pfd[0], NULL);
    n = epoll_wait(ep, out, 8, 0);
    int n2 = epoll_wait(ep, out, 8, 0);
    CHECK("EPOLLONESHOT reports once, CTL_DEL removes", n == 1 && n2 == 0, "n=%d n2=%d", n, n2);
    r = epoll_ctl(ep, EPOLL_CTL_DEL, pfd[0], NULL);
    CHECK("CTL_DEL of an absent fd -> ENOENT", r == -1 && errno == ENOENT, "r=%d errno=%d", r, errno);
    int fdf = open("/proc/self/stat", O_RDONLY);
    e.events = EPOLLIN; r = epoll_ctl(ep, EPOLL_CTL_ADD, fdf, &e);
    CHECK("regular file -> EPERM", r == -1 && errno == EPERM, "r=%d errno=%d", r, errno);
    close(fdf);
    /* closed fd: the registration goes away (the number is reused) */
    int sp[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    e.events = EPOLLIN; e.data.u64 = 0x55; epoll_ctl(ep, EPOLL_CTL_ADD, sp[0], &e);
    int num = sp[0]; close(sp[0]);
    int again = dup(sp[1]);                         /* likely reuses `num` */
    write(sp[1], "q", 1);
    n = epoll_wait(ep, out, 8, 50);
    int stale = 0; for (int i = 0; i < n; i++) if (out[i].data.u64 == 0x55) stale = 1;
    r = (again == num) ? epoll_ctl(ep, EPOLL_CTL_ADD, again, &e) : 0;
    CHECK("closed fd's registration dropped (re-ADD of the reused number works)", !stale && r == 0, "stale=%d r=%d again=%d num=%d", stale, r, again, num);
    close(again); close(sp[1]);
    /* EPOLLET on a socket pair: one report per arrival */
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    e.events = EPOLLIN | EPOLLET; e.data.u64 = 0x77;
    epoll_ctl(ep, EPOLL_CTL_ADD, sp[0], &e);
    write(sp[1], "a", 1);
    n = epoll_wait(ep, out, 8, 100);
    n2 = epoll_wait(ep, out, 8, 0);
    CHECK("EPOLLET: reported once per edge", n >= 1 && n2 == 0, "n=%d n2=%d", n, n2);
    close(sp[0]); close(sp[1]);
    /* as many sockets as the fd table holds */
    epoll_ctl(ep, EPOLL_CTL_DEL, ev, NULL);
    int fds[64], nf = 0;
    for (;;) { int s2[2]; if (socketpair(AF_UNIX, SOCK_DGRAM, 0, s2)) break; fds[nf++] = s2[0]; fds[nf++] = s2[1]; if (nf >= 62) break; }
    int added = 0;
    for (int i = 0; i < nf; i += 2) { e.events = EPOLLIN; e.data.u64 = (uint64_t)i; if (!epoll_ctl(ep, EPOLL_CTL_ADD, fds[i], &e)) added++; }
    for (int i = 1; i < nf; i += 2) write(fds[i], "m", 1);
    struct epoll_event big[64];
    n = epoll_wait(ep, big, 64, 1000);
    CHECK("epoll over every free fd (all sockets ready at once)", added > 4 && n == added, "added=%d n=%d (fd table full at %d)", added, n, nf);
    for (int i = 0; i < nf; i++) close(fds[i]);
    close(ep); close(ev); close(pfd[0]); close(pfd[1]);
}

static void t_eventfd(void)
{
    int fd = eventfd(3, EFD_SEMAPHORE | EFD_NONBLOCK);
    uint64_t v = 0;
    int a = read(fd, &v, 8); uint64_t v1 = v;
    read(fd, &v, 8); read(fd, &v, 8);
    int b = read(fd, &v, 8);
    CHECK("eventfd semaphore mode: 3 x 1 then EAGAIN", a == 8 && v1 == 1 && b == -1 && errno == EAGAIN, "a=%d v1=%llu b=%d", a, (unsigned long long)v1, b);
    close(fd);
    fd = eventfd(0, 0);
    pid_t ch = fork();
    if (ch == 0) { usleep(50000); uint64_t x = 5; write(fd, &x, 8); x = 2; write(fd, &x, 8); _exit(0); }
    a = read(fd, &v, 8);
    waitpid(ch, 0, 0);
    CHECK("eventfd blocking read wakes on a write (counter value)", a == 8 && (v == 5 || v == 7), "a=%d v=%llu", a, (unsigned long long)v);
    v = 0xffffffffffffffffULL;
    CHECK("eventfd write of ~0 -> EINVAL", write(fd, &v, 8) == -1 && errno == EINVAL, "errno=%d", errno);
    close(fd);
}

static void t_timerfd(void)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    struct itimerspec its = { { 0, 50000000 }, { 0, 100000000 } };   /* 100 ms, then every 50 ms */
    long t0 = now_ms();
    int r = timerfd_settime(fd, 0, &its, NULL);
    uint64_t n = 0;
    int a = read(fd, &n, 8);
    long dt = now_ms() - t0;
    CHECK("timerfd: first expiry after ~100 ms (blocking read)", r == 0 && a == 8 && n >= 1 && dt >= 90 && dt < 1000, "r=%d a=%d n=%llu dt=%ld", r, a, (unsigned long long)n, dt);
    usleep(220000);
    read(fd, &n, 8);
    CHECK("timerfd interval: expirations accumulate", n >= 3 && n <= 6, "n=%llu", (unsigned long long)n);
    struct itimerspec cur; timerfd_gettime(fd, &cur);
    CHECK("timerfd_gettime: interval kept, value pending", cur.it_interval.tv_nsec == 50000000 && (cur.it_value.tv_sec || cur.it_value.tv_nsec), "ns=%ld", cur.it_interval.tv_nsec);
    struct itimerspec off = { { 0, 0 }, { 0, 0 } };
    timerfd_settime(fd, 0, &off, NULL);
    int fl = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    r = read(fd, &n, 8);
    CHECK("disarmed timerfd: non-blocking read -> EAGAIN", r == -1 && errno == EAGAIN, "r=%d errno=%d", r, errno);
    /* absolute CLOCK_REALTIME expiry */
    close(fd);
    fd = timerfd_create(CLOCK_REALTIME, 0);
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    struct itimerspec abs = { { 0, 0 }, { now.tv_sec, now.tv_nsec } };
    abs.it_value.tv_nsec += 80000000; if (abs.it_value.tv_nsec >= 1000000000) { abs.it_value.tv_sec++; abs.it_value.tv_nsec -= 1000000000; }
    t0 = now_ms();
    timerfd_settime(fd, TFD_TIMER_ABSTIME, &abs, NULL);
    a = read(fd, &n, 8); dt = now_ms() - t0;
    CHECK("timerfd TFD_TIMER_ABSTIME on CLOCK_REALTIME", a == 8 && n == 1 && dt >= 50 && dt < 1000, "a=%d dt=%ld", a, dt);
    close(fd);
}

static volatile int g_alrm;
static void on_alrm(int s) { (void)s; g_alrm++; }
static void t_itimer(void)
{
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_alrm; sigaction(SIGALRM, &sa, NULL);
    g_alrm = 0;
    long t0 = now_ms();
    alarm(1);
    pause();
    long dt = now_ms() - t0;
    CHECK("alarm(1): SIGALRM after ~1 s, pause() returns", g_alrm == 1 && dt >= 900 && dt < 3000, "alrm=%d dt=%ld", g_alrm, dt);
    struct itimerval it = { { 0, 100000 }, { 0, 100000 } };
    g_alrm = 0;
    setitimer(ITIMER_REAL, &it, NULL);
    t0 = now_ms();
    while (g_alrm < 5 && now_ms() - t0 < 3000) pause();
    dt = now_ms() - t0;
    struct itimerval z = { { 0, 0 }, { 0, 0 } }, old;
    setitimer(ITIMER_REAL, &z, &old);
    CHECK("setitimer interval: 5 SIGALRMs in ~500 ms, then disarmed", g_alrm == 5 && dt >= 400 && dt < 2500 && old.it_interval.tv_usec == 100000,
          "alrm=%d dt=%ld", g_alrm, dt);
    /* a blocked read is interrupted with EINTR */
    int p[2]; pipe(p);
    alarm(1);
    char c; int r = read(p[0], &c, 1);
    CHECK("SIGALRM interrupts a blocking read (EINTR)", r == -1 && errno == EINTR, "r=%d errno=%d", r, errno);
    close(p[0]); close(p[1]);
}

/* ================================================================ helpers */
static int userver(const char *path)
{
    unlink(path);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX; snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 4)) { printf("NP USERVER bind errno=%d\n", errno); return 1; }
    printf("NP USERVER ready\n"); fflush(stdout);
    int s = accept(ls, NULL, NULL);
    char b[64]; int n = read(s, b, sizeof b);
    if (n > 0) write(s, b, n);
    close(s); close(ls);
    return 0;
}
static int uconnect(const char *path)
{
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX; snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    int r = connect(c, (struct sockaddr *)&a, sizeof a);
    int e = r ? errno : 0;
    if (!r) { write(c, "hi", 2); char b[4]; if (read(c, b, 2) != 2) e = 999; }
    printf("NP UCONNECT errno=%d\n", e);
    return 0;
}
static int server(int port, int n)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = sin_of("0.0.0.0", port);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 16)) { printf("NP SERVER errno=%d\n", errno); return 1; }
    printf("NP SERVER ready\n"); fflush(stdout);
    int done = 0;
    for (int i = 0; i < n; i++) {
        struct sockaddr_in p; socklen_t pl = sizeof p;
        int s = accept4(ls, (struct sockaddr *)&p, &pl, SOCK_NONBLOCK);
        if (s < 0) break;
        int fl = fcntl(s, F_GETFL); fcntl(s, F_SETFL, fl & ~O_NONBLOCK);
        char b[4096]; int k;
        while ((k = read(s, b, sizeof b)) > 0) writen(s, b, k);
        close(s); done++;
    }
    printf("NP SERVER served=%d\n", done);
    close(ls);
    return done == n ? 0 : 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *cmd = argc > 1 ? argv[1] : "";
    if (!strcmp(cmd, "net") && argc > 2) {
        t_tcp(argv[2]); t_nbconnect(argv[2]); t_udp(argv[2]); t_sockopt(); t_dns(); t_ping(argv[2]);
        printf("NP DONE net pass=%d fail=%d\n", g_pass, g_fail);
        return g_fail != 0;
    }
    if (!strcmp(cmd, "local") && argc > 2) {
        t_unix(argv[2]); t_epoll(); t_eventfd(); t_timerfd(); t_itimer();
        printf("NP DONE local pass=%d fail=%d\n", g_pass, g_fail);
        return g_fail != 0;
    }
    if (!strcmp(cmd, "userver") && argc > 2) return userver(argv[2]);
    if (!strcmp(cmd, "uconnect") && argc > 2) return uconnect(argv[2]);
    if (!strcmp(cmd, "raw") && argc > 2) return t_raw(argv[2]);
    if (!strcmp(cmd, "server") && argc > 3) return server(atoi(argv[2]), atoi(argv[3]));
    if (!strcmp(cmd, "nonet")) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        int u = socket(AF_UNIX, SOCK_STREAM, 0);
        printf("NP NONET inet=%d errno=%d unix=%d\n", fd, fd < 0 ? errno : 0, u >= 0);
        return 0;
    }
    fprintf(stderr, "usage: nptest net HOST | local DIR | userver PATH | uconnect PATH | raw HOST | server PORT N | nonet\n");
    return 2;
}
