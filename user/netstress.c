/* netstress — ring-3 network stack stress test (M47 net locking).
 * SPDX-License-Identifier: MIT
 *
 * Drives the kernel network stack from several processes at once; with SMP the
 * kernel pins forked children round-robin to the online CPUs, so the workers
 * really run in parallel on different cores. Host-side peers are provided by
 * tools/net_stress_drive.py over QEMU user networking (10.0.2.2 = the host).
 *
 *   netstress stats
 *       one "[NETSTAT] ..." line: socket table, TCP control blocks, UDP
 *       bindings, ARP entries (SYS_NETCTL NETCTL_STATS) + MemFree.
 *   netstress client <echo> <fire> <udpsink> <dns> <nproc> <rounds>
 *       <nproc> workers, each <rounds> times: a 16 KB TCP echo round trip
 *       (byte pattern checked), a firehose connection closed after 8 KB while
 *       data is still arriving, a 100-datagram UDP flood, and 3 DNS A queries
 *       (answer checked). One "[NETSTRESS] worker ..." line each, then
 *       "[NETSTRESS] DONE pass=<n> fail=<m>".
 *   netstress server <port> <nconn>
 *       listen on <port>, accept up to <nconn> connections (or until 15 s pass
 *       without one: a connection the peer resets before accept() is dropped
 *       by the kernel), echo each one in its own forked child until EOF or
 *       reset (at most 4 at a time), then
 *       "[NETSTRESS] server done conns=<n> errors=<e>".
 *
 * Output goes to stdout and is mirrored to debugcon by the kernel.
 */
#include "libsecos.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include "../net/netctl.h"

#define SYS_NETCTL 95
#define HOST_IP ip4(10, 0, 2, 2)

static long netctl(long op, long idx, void* arg) {
    return secos_syscall(SYS_NETCTL, op, idx, (long)arg, 0, 0);
}

static unsigned char pat(unsigned id, unsigned long i) {
    return (unsigned char)(i * 13u + (i >> 9) + id * 71u + 1u);
}

static long memfree_kb(void) {
    char b[512];
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return -1;
    long n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    char* p = strstr(b, "MemFree:");
    if (!p) return -1;
    p += 8;
    while (*p == ' ') p++;
    return atol(p);
}

static int do_stats(void) {
    struct secos_netstats st;
    memset(&st, 0, sizeof(st));
    long r = netctl(NETCTL_STATS, 0, &st);
    if (r != 0) { printf("[NETSTAT] netctl failed %ld\n", r); return 1; }
    printf("[NETSTAT] sockets=%u tcp=%u orphans=%u udp=%u arp=%u memfree=%ld rx=%lu tx=%lu txerr=%lu rekick=%lu tcpfull=%lu tcpnomem=%lu tcppeak=%u\n",
           st.sockets, st.tcp_conns, st.tcp_orphans, st.udp_binds, st.arp_entries, memfree_kb(),
           (unsigned long)st.rx_frames, (unsigned long)st.tx_frames,
           (unsigned long)st.tx_errors, (unsigned long)st.rx_rekicks,
           (unsigned long)st.tcp_full, (unsigned long)st.tcp_nomem, st.tcp_peak);
    return 0;
}

/* ---- client workers ---- */
static unsigned char g_buf[16384];

/* First failure seen by this worker (printed in its summary line). */
static char g_err[96];
static void fail_note(const char* what, long v) {
    if (!g_err[0]) snprintf(g_err, sizeof(g_err), " first-error=%s:%ld", what, v);
}

static int tcp_open(unsigned short port) {
    int s = socket(SOCK_STREAM);
    if (s < 0) return s;
    int r = connect(s, HOST_IP, port);
    if (r < 0) { sockclose(s); return r; }
    return s;
}

static int t_echo(unsigned id, unsigned short port) {
    int s = tcp_open(port);
    if (s < 0) { fail_note("echo-connect", s); return 0; }
    for (unsigned long i = 0; i < sizeof(g_buf); i++) g_buf[i] = pat(id, i);
    long off = 0;
    while (off < (long)sizeof(g_buf)) {
        long n = send(s, g_buf + off, (long)sizeof(g_buf) - off);
        if (n <= 0) { fail_note("echo-send", n); sockclose(s); return 0; }
        off += n;
    }
    unsigned char rb[2048];
    unsigned long got = 0; int ok = 1;
    while (got < sizeof(g_buf)) {
        long n = recv(s, rb, sizeof(rb));
        if (n <= 0) { fail_note("echo-recv", n); ok = 0; break; }
        for (long k = 0; k < n; k++)
            if (rb[k] != pat(id, got + (unsigned long)k)) { if (ok) fail_note("echo-data-at", (long)(got + (unsigned long)k)); ok = 0; }
        got += (unsigned long)n;
    }
    sockclose(s);
    return ok && got == sizeof(g_buf);
}

static int t_fire(unsigned short port) {
    int s = tcp_open(port);
    if (s < 0) { fail_note("fire-connect", s); return 0; }
    unsigned char rb[1024];
    long got = 0;
    while (got < 8192) {
        long n = recv(s, rb, sizeof(rb));
        if (n <= 0) { fail_note("fire-recv", n); break; }
        got += n;
    }
    sockclose(s);                     /* data is still streaming in */
    return got >= 8192;
}

static int t_udp(unsigned id, unsigned round, unsigned short port) {
    int s = socket(SOCK_DGRAM);
    if (s < 0) return 0;
    struct secos_sockaddr sa = { HOST_IP, port, 0 };
    unsigned char d[512];
    int sent = 0;
    for (int i = 0; i < 100; i++) {
        memset(d, (int)(id * 16 + (unsigned)i), sizeof(d));
        d[0] = (unsigned char)id; d[1] = (unsigned char)round; d[2] = (unsigned char)i;
        if (sendto(s, d, sizeof(d), &sa) == (long)sizeof(d)) sent++;
    }
    sockclose(s);
    return sent == 100;
}

/* One DNS A query for w<id>r<round>q<q>.secos.test; the host answers
 * 10.<id>.<round>.<q>. Returns 1 on a matching answer within 2 s. */
static int t_dns_one(unsigned id, unsigned round, unsigned q, unsigned short port) {
    int s = socket(SOCK_DGRAM | 0x800 /*SOCK_NONBLOCK*/);
    if (s < 0) return 0;
    unsigned char m[128];
    unsigned qid = (id << 10) ^ (round << 4) ^ q ^ 0x5a5a;
    memset(m, 0, sizeof(m));
    m[0] = (unsigned char)(qid >> 8); m[1] = (unsigned char)qid;
    m[2] = 0x01; m[5] = 1;
    char lbl[32];
    int ll = snprintf(lbl, sizeof(lbl), "w%ur%uq%u", id, round, q);
    int o = 12;
    m[o++] = (unsigned char)ll; memcpy(m + o, lbl, (size_t)ll); o += ll;
    m[o++] = 5; memcpy(m + o, "secos", 5); o += 5;
    m[o++] = 4; memcpy(m + o, "test", 4); o += 4;
    m[o++] = 0;
    m[o++] = 0; m[o++] = 1; m[o++] = 0; m[o++] = 1;
    struct secos_sockaddr sa = { HOST_IP, port, 0 };
    int ok = 0;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        if (sendto(s, m, o, &sa) != o) continue;
        unsigned long dl = getticks() + 2000;
        while (!ok && getticks() < dl) {
            struct pollfd pf = { s, POLLIN, 0 };
            poll(&pf, 1, 200);
            unsigned char r[256];
            struct secos_sockaddr from;
            long n = recvfrom(s, r, sizeof(r), &from);
            if (n < 0) continue;
            if (n < 12 + 4 || r[0] != m[0] || r[1] != m[1] || !(r[2] & 0x80)) continue;
            /* the answer RR is the last 16 bytes: ... type A, class IN, ttl, len 4, addr */
            const unsigned char* a = r + n - 4;
            ok = a[0] == 10 && a[1] == id && a[2] == round && a[3] == q;
        }
    }
    sockclose(s);
    return ok;
}

static int worker(unsigned id, unsigned short echo, unsigned short fire,
                  unsigned short udps, unsigned short dns, int rounds) {
    int pass = 0, fail = 0;
    int e = 0, f = 0, u = 0, d = 0;
    for (int r = 0; r < rounds; r++) {
        if (t_echo(id, echo)) e++;
        if (t_fire(fire)) f++;
        if (t_udp(id, (unsigned)r, udps)) u++;
        for (unsigned q = 1; q <= 3; q++) if (t_dns_one(id, (unsigned)r, q, dns)) d++;
    }
    pass = e + f + u + d;
    fail = rounds * 6 - pass;
    printf("[NETSTRESS] worker %u pid %d echo=%d/%d fire=%d/%d udp=%d/%d dns=%d/%d %s%s\n",
           id, getpid(), e, rounds, f, rounds, u, rounds, d, rounds * 3, fail ? "FAIL" : "OK", g_err);
    return fail ? 1 : 0;
}

static int do_client(int argc, char** argv) {
    if (argc < 8) return 2;
    unsigned short echo = (unsigned short)atoi(argv[2]), fire = (unsigned short)atoi(argv[3]);
    unsigned short udps = (unsigned short)atoi(argv[4]), dns = (unsigned short)atoi(argv[5]);
    int nproc = atoi(argv[6]), rounds = atoi(argv[7]);
    if (nproc < 1 || nproc > 8 || rounds < 1) return 2;
    int pids[8];
    for (int i = 0; i < nproc; i++) {
        int pid = fork();
        if (pid == 0) _exit(worker((unsigned)i + 1, echo, fire, udps, dns, rounds));
        pids[i] = pid;
    }
    int pass = 0, fail = 0;
    for (int i = 0; i < nproc; i++) {
        if (pids[i] < 0) { fail++; continue; }
        int st = waitpid(pids[i]);
        if (st == 0) pass++; else fail++;
    }
    printf("[NETSTRESS] DONE pass=%d fail=%d\n", pass, fail);
    return fail ? 1 : 0;
}

/* ---- server ---- */
static int echo_conn(int c) {
    unsigned char b[4096];
    long total = 0;
    for (;;) {
        long n = recv(c, b, sizeof(b));
        if (n <= 0) break;                /* EOF or reset by the host */
        long off = 0;
        while (off < n) {
            long w = send(c, b + off, n - off);
            if (w <= 0) { off = -1; break; }
            off += w;
        }
        if (off < 0) break;
        total += n;
    }
    sockclose(c);
    return total > 0 ? 0 : 1;
}

static int do_server(int argc, char** argv) {
    if (argc < 4) return 2;
    unsigned short port = (unsigned short)atoi(argv[2]);
    int nconn = atoi(argv[3]);
    int s = socket(SOCK_STREAM | 0x800 /*SOCK_NONBLOCK*/);
    if (s < 0 || bind(s, port) < 0 || listen(s, 4) < 0) {
        printf("[NETSTRESS] server setup failed\n");
        return 1;
    }
    printf("[NETSTRESS] server listening port=%u\n", port);
    int live[4]; int nlive = 0, conns = 0, bad = 0;
    unsigned long idle_until = getticks() + 15000;
    while (conns < nconn && getticks() < idle_until) {
        struct pollfd pf = { s, POLLIN, 0 };
        poll(&pf, 1, 500);
        int c = accept(s);
        if (c == -11) continue;           /* -EAGAIN: nothing pending yet */
        if (c < 0) { bad++; if (bad > 50) break; continue; }
        idle_until = getticks() + 15000;
        conns++;
        if (nlive == 4) {                 /* bound the forked handlers */
            if (waitpid(live[0]) != 0) bad++;
            for (int k = 1; k < 4; k++) live[k - 1] = live[k];
            nlive--;
        }
        int pid = fork();
        if (pid == 0) { sockclose(s); _exit(echo_conn(c)); }
        sockclose(c);
        if (pid > 0) live[nlive++] = pid;
    }
    for (int k = 0; k < nlive; k++) waitpid(live[k]);
    sockclose(s);
    printf("[NETSTRESS] server done conns=%d errors=%d\n", conns, bad);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "stats") == 0) return do_stats();
    if (argc >= 2 && strcmp(argv[1], "client") == 0) return do_client(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "server") == 0) return do_server(argc, argv);
    printf("usage: netstress stats | client <echo> <fire> <udp> <dns> <nproc> <rounds> | server <port> <nconn>\n");
    return 2;
}
