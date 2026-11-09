# U10 — network userland for musl programs (`feat/net-posix`)

Userland plan §3 U10 (with D10 for time, certificates and the resolver), plus
ICMP sockets so that `ping` works for a normal user. musl programs now get
Linux-shaped sockets from the kernel (AF_INET TCP/UDP, ICMP ping sockets, raw
IP sockets, AF_UNIX), `epoll`, `eventfd`, `timerfd`, `setitimer`/`alarm`,
musl's own DNS resolver, the netdb files, a CA bundle, iputils `ping`, BusyBox
`ntpd` + `hwclock`, and an NTP step at boot. This is what U11 (OpenSSL, curl,
wget) builds on.

## Quick start

```
tools/netposix_test.sh [--udrv] [--lockdep] [--smp N] [--iso FILE] [--keep]   # the NETPOSIX gate
make port-iputils      # iputils ping on musl  -> crypto/user_ping_elf.h
make port-bbnet        # BusyBox ntpd + hwclock on musl -> crypto/user_bbnet_elf.h
make fetch-cabundle    # Mozilla CA bundle (curl.se) -> kernel/ca_bundle.h
```

On SecOS: `ping 10.0.2.2`, `ping -e 0 HOST` (raw socket, root),
`ntpd -n -q -p SERVER`, `hwclock -r -u` / `-w -u`, `getent`-style lookups in
any musl program through `/etc/hosts` and `/etc/resolv.conf`.

## Kernel

### New calls (512 + Linux number, Linux arguments, -errno)

| File | Calls |
|---|---|
| `kernel/lx_net.c` | `socket` 553, `connect` 554, `accept` 555, `sendto` 556, `recvfrom` 557, `sendmsg` 558, `recvmsg` 559, `shutdown` 560, `bind` 561, `listen` 562, `getsockname` 563, `getpeername` 564, `socketpair` 565, `setsockopt` 566, `getsockopt` 567, `accept4` 800 |
| `kernel/lx_event.c` | `getitimer` 548, `setitimer` 550 (ITIMER_REAL), `epoll_create` 725, `epoll_wait` 744, `epoll_ctl` 745, `epoll_pwait` 793, `timerfd_create` 795, `eventfd` 796, `timerfd_settime` 798, `timerfd_gettime` 799, `eventfd2` 802, `epoll_create1` 803, `epoll_pwait2` 953 |

The musl translation layer passes all of them through unchanged
(`user/musl-secos/src/secos/syscall.c`); `alarm` is `setitimer`, and `pause`
(missing until now: it returned at once) is `sigsuspend` with the current
mask. `syscall_lx_dispatch()` now gets the trapframe, so these calls can
block. All of them run without the BKL (`kernel/bkl.c` `lx_lockfree()`).

### Blocking: `lx_wait()` (`kernel/syscall_trap.c`)

Every lower layer is non-blocking. A call that cannot progress calls
`lx_wait(tf, wake_obj, recheck_ticks, timeout_ns, timeout_ret)`: it returns
`-EINTR` when a signal is pending (a pending stop stops and restarts the
call), `timeout_ret` once the call's deadline has passed, and otherwise
blocks with `wait_pipe = wake_obj` and a timed re-check, rewinding `rip` so
the call re-runs from the start when woken. The deadline is fixed at the
first attempt of a call site (`process_t.lx_wait_rip`/`lx_deadline`) and kept
across re-runs, so `SO_RCVTIMEO` ends on time even when the task is woken
often; the dispatcher clears it when a call completes. The M24 read/write
path on sockets uses the same helper, so `SO_RCVTIMEO`/`SO_SNDTIMEO` also
bound `read()`/`write()` on a socket.

AF_UNIX sockets, eventfd and timerfd are woken by their peers (no polling).
AF_INET sockets still re-check every 2 ms, as since M24: the RX path has no
wake hook yet (M47 open issue).

### Anonymous-inode fds (`kernel/anonfd.c`)

eventfd, timerfd, epoll instances and AF_UNIX sockets are `anonfd`s: kernel
objects behind an fd that live in no filesystem. Each embeds a `vfs_inode_t`
whose `ops` pointer marks it (harmless stub ops for any generic path), plus
an ops table (read/write/poll/ioctl/release) and an atomic fd reference
count. One hook each in fd dup/close, fork, exec (close-on-exec), exit,
`read`/`write`/`ioctl`/`lseek`/`mmap` and `poll` serves every kind; fstat
reports the inode's mode (`S_IFSOCK` for AF_UNIX sockets). A blocking
read/write that gets `-EAGAIN` arms `wait_pipe` = the object and returns -2;
`syscall_trap.c` then blocks until `anonfd_wake()` (= `sched_wake_pipe`,
which also wakes `poll`/`epoll` sleepers).

`proc_fd_entry` gained a `gen` field (unique per fd opening, set by
`fd_alloc`), so epoll can tell a registered fd from a new file that reused
its number.

### AF_INET (`net/socket.c`, `kernel/lx_net.c`)

Four kinds of socket share the M24 table: TCP, UDP, ICMP ping sockets and raw
IP sockets.

- Options: `SO_REUSEADDR/REUSEPORT/KEEPALIVE/BROADCAST/LINGER/RCVTIMEO/
  SNDTIMEO/TIMESTAMP/TIMESTAMPNS/PRIORITY/OOBINLINE/MARK/BINDTODEVICE`
  (the last two root only), `SO_RCVBUF/SNDBUF` (read-only: fixed sizes),
  `SO_ERROR/TYPE/DOMAIN/PROTOCOL/ACCEPTCONN`; `IP_TTL/TOS/HDRINCL/RECVERR/
  RECVTTL/RECVTOS/MTU_DISCOVER/MULTICAST_TTL/LOOP/IF/MTU/PKTINFO`;
  `TCP_NODELAY/MAXSEG/CORK/KEEPIDLE/KEEPINTVL/KEEPCNT/USER_TIMEOUT`;
  `ICMP_FILTER` on raw ICMP sockets. Unknown ones: `ENOPROTOOPT`.
  `SO_LINGER {1, 0}` closes with RST. Keepalive and the TCP tuning options
  are stored, not acted on (no keepalive probes yet).
- `shutdown` (FIN after the queued data; the socket stays readable),
  `MSG_PEEK`, `MSG_WAITALL` (waits without consuming until the request fits),
  `MSG_DONTWAIT`, `MSG_TRUNC` (in and out), `MSG_NOSIGNAL` (else `EPIPE` on a
  stream also posts `SIGPIPE`; `write()` on an AF_INET socket still does not,
  as before), `MSG_FASTOPEN` (no TFO: connect then send; musl's resolver
  needs it), `MSG_ERRQUEUE`.
- `sendmsg/recvmsg` with iovecs; cmsgs out: `SO_TIMESTAMP`, `SO_TIMESTAMPNS`,
  `IP_TTL`, `IP_TOS`, `IP_RECVERR` (errqueue). `SCM_RIGHTS`/`SCM_CREDENTIALS`
  in are refused with `EOPNOTSUPP`.
- Non-blocking connect (`EINPROGRESS`, poll `POLLOUT`, `SO_ERROR`), a refused
  port (`ECONNREFUSED`, both forms), bind to port 0 / an address (`INADDR_ANY`
  or the interface's; `EADDRNOTAVAIL` otherwise), `EADDRINUSE` (a TCP bind
  conflicts with a listener, another bound socket, or — without
  `SO_REUSEADDR` — a live connection on the port; the M24 call keeps its
  permissive behaviour), a TCP connect from a bound port, `listen` autobinding.
- Datagrams are queued one allocation each (bounded by 212992 bytes per
  socket, Linux's default), with source, TTL, TOS and arrival time, so
  `MAX_SOCK` and `TCP_MAX_CONN` could go to **1024**: the table costs about
  460 KiB of BSS (socket) + 180 KiB (TCP), and a connection's 128 KiB of
  buffers come from the heap only while it is open. UDP bindings follow
  (1024), the TCP listen backlog went 4 → 128.
- IPv4 (`net/ipv4.c`): per-socket TTL and TOS; a datagram whose next hop is
  not in the ARP cache waits (bounded, 3 s) instead of being dropped (TCP keeps
  its own SYN retry); `IP_HDRINCL` output with the header completed by the
  kernel (length, checksum, id, source); input now verifies the IP header
  checksum, drops fragments (there is no reassembly: they used to be
  delivered as if whole) and checks ICMP checksums.

### ICMP sockets — the rules

**Ping sockets** (`socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)`), Linux
semantics, for any process with network access (`CAP_NET`):
- the kernel owns the echo identifier: it is the socket's "port"; `bind()`
  chooses it (`EADDRINUSE` if another ping socket has it), else the first send
  picks a free one; `getsockname` reports it;
- only ICMP echo requests (type 8, code 0) may be sent (`EINVAL` otherwise);
  the kernel writes its identifier into them, recomputes the checksum and
  builds the IP header (`IP_TTL`, `IP_TOS` apply);
- the socket receives only echo replies carrying its identifier (from its
  peer when connected), as the ICMP message without the IP header, and the
  ICMP errors that quote one of its requests; every other echo reply (the
  kernel shell's `ping`, other sockets) never reaches it.

**Raw sockets** (`SOCK_RAW`, any protocol): **root, or a program whose signed
manifest declares the new explicit capability `CAP_NET_RAW`** (manifest bit
17); everyone else gets `EPERM`, audited (`[AUDIT] ... NET_RAW`). Like
`CAP_INPUT`/`CAP_DISPLAY` it is never ambient, and it is in
`CAP_LOCAL_SIGN_CEILING_DENY` (`kernel/cap.h`): the D9 machine-key signer
(U12a, not written yet — there is no local signing ceiling in the tree to add
it to) must never grant it without root's approval. A raw socket gets a copy
of every received IPv4 packet of its protocol, IP header included
(`ICMP_FILTER`, a bound address and a connected peer filter it), and sends a
payload (the kernel adds the header) or, with `IP_HDRINCL` / `IPPROTO_RAW`, a
whole packet.

**ICMP errors** (destination unreachable, time exceeded, parameter problem)
are matched to the UDP or ping socket whose packet they quote (source port +
connected peer, or echo identifier) and handled as on Linux: errno from
`icmp_err_convert`'s table; with `IP_RECVERR` an entry in the socket's error
queue (`recvmsg(MSG_ERRQUEUE)`: the quoted transport header as data, the
original destination as `msg_name`, a `(SOL_IP, IP_RECVERR)` cmsg with
`struct sock_extended_err` (origin ICMP, type, code, info) followed by the
ICMP sender's address) and `sk_err`; without it only a hard error on a
connected socket sets `sk_err`. `sk_err` makes the next call fail once with
that errno (so a connected UDP socket to a closed port gets
`ECONNREFUSED`). Raw ICMP sockets see the error messages themselves. TCP
ignores ICMP errors (as before).

### AF_UNIX (`kernel/unix_sock.c`)

`SOCK_STREAM` and `SOCK_DGRAM`, `socketpair`, `SO_PEERCRED` (pid/uid/gid of
the peer at connect time), `shutdown`, `MSG_PEEK/WAITALL/DONTWAIT/TRUNC`,
`FIONREAD`, timeouts. Names:
- **paths**: `bind()` creates the file (mode 0777 & ~umask, owned by the
  caller, after the same checks as `creat`; `EADDRINUSE` if the name
  exists); `connect`/`sendto` resolve it with search permission on every
  directory and need **write permission on the socket file**, as Linux; the
  binding remembers the file's inode, its st_ino and its canonical path, and
  connect must match all three, so a socket file that was removed and
  recreated elsewhere never reaches the old socket (a new bind to a reused
  inode also retires any older binding to it); `close()` leaves the file
  (remove it with `unlink`). The file is a regular file on disk: none of
  the SecOS filesystems stores `S_IFSOCK` yet (fstat of the *socket fd* does
  say `S_IFSOCK`);
- **abstract names** (`sun_path[0] == 0`): a kernel table, no permissions;
  `bind()` with only the family autobinds a 5-hex-digit name.
A connected datagram socket only accepts datagrams from its peer (`EPERM`
otherwise); a full receiver blocks the sender. One spinlock (`g_unix_lock`,
S13) guards every socket; data is copied through kernel bounce buffers outside
it, wakes happen after the unlock. Not yet: `SCM_RIGHTS`, `SOCK_SEQPACKET`.

### epoll, eventfd, timerfd, ITIMER_REAL (`kernel/lx_event.c`)

- **eventfd**: 64-bit counter, `EFD_SEMAPHORE/NONBLOCK/CLOEXEC`, blocking both
  ways, `EINVAL` for ~0.
- **timerfd**: `CLOCK_REALTIME/MONOTONIC/BOOTTIME` (+ `_ALARM`), relative or
  `TFD_TIMER_ABSTIME`, intervals; read returns the expiration count; a
  blocked reader sleeps until the expiry (or a new setting).
- **epoll**: an interest list of (fd number, fd generation, events, data)
  evaluated with the same readiness function as `poll()` (`kiface_fd_revents`,
  exported); `EPOLLONESHOT`, `EPOLLET`, `EPOLLRDHUP`; `EPERM` for regular
  files, `EEXIST`/`ENOENT`, a closed fd's registration disappears. epoll fds
  are pollable themselves. Differences from Linux (documented in the file): the
  registration lives in the caller's fd table (an epoll fd shared across fork
  is evaluated against the waiter's table); `EPOLLET` reports a readiness bit
  when it appears and again if it is still set 20 ms later (an edge between two
  scans is seen at most that late; an fd left ready does not cause a busy
  loop); blocking re-checks every 2 ms when sockets/evdev/timerfds are
  watched, else on any pipe/anonfd/pty/console wake or 10 ms.
- **setitimer/getitimer(ITIMER_REAL)** (pulled forward from U2: BusyBox ntpd
  `-q` gives up through `alarm(10)` and iputils ping ends `-c N` with
  `SIGALRM`): the BSP tick posts `SIGALRM` (scan only while a timer is armed);
  fork clears it, exec keeps it. `ITIMER_VIRTUAL/PROF` → `EINVAL`.
- One spinlock `g_ev_lock` (S14); epoll snapshots its list and evaluates the
  watched fds with it dropped.

### /dev/rtc0

`RTC_RD_TIME` / `RTC_SET_TIME` (`fs/devfs.c`, `rtc_write()` in
`drivers/rtc.c`: the update cycle held off, the firmware's BCD/binary and
12/24 h encoding kept). Mode 0600 root; setting it also needs `CAP_TIME`. The
CMOS clock holds UTC.

`ioctl` requests are now truncated to 32 bits (Linux: `unsigned int`): musl
passes `ioctl`'s `int` request sign-extended, so `0x80247009` arrived as
`0xffffffff80247009` and matched nothing.

## User space

| What | Where | |
|---|---|---|
| iputils 20250605 `ping` | `/bin/ping` (replaces BusyBox's, which needs raw sockets) | `tools/port-iputils.sh`: SHA-256 pinned, `sha256sums.asc` PGP-checked (Petr Vorel, `2016FEA4858B1C36B32E833AC0DEC2EE72F33A5F`); only ping, no libcap/libidn2/gettext, no source patch; CAP_NET manifest (no `CAP_NET_RAW`) |
| BusyBox 1.36.1 `ntpd`, `hwclock` on musl | `/bin/bbnet` + `/bin/ntpd`, `/bin/hwclock` | `tools/port-bbnet.sh`: a second, two-applet BusyBox built against the musl sysroot (the main BusyBox is still on the old libc until U7); signature `C9E9416F76E610DBD09D040F47B70C55ACC9965B` (Denys Vlasenko) |
| Mozilla CA bundle 2026-11-05 (118 certificates) | `/etc/ssl/certs/ca-certificates.crt` (system file) | `tools/fetch-cabundle.sh` + `tools/cabundle2h.py`: curl.se's `cacert-2026-11-05.pem`, SHA-256 pinned and cross-checked with curl's `.sha256`; MPL-2.0 data |
| `/etc/hosts`, `/etc/services`, `/etc/protocols` | created when missing | `kernel/netdb_files.h` (IANA numbers) |
| `/etc/resolv.conf` | written by `ifup` (lease) and `ifconfig ... dns D` | atomically, with a marker line; a file without the marker (the administrator's) is left alone |

ping is a normal user's tool: iputils tries the ICMP ping socket first, so a
user's `ping HOST` works through it; `ping -e 0` (identifier 0) forces a raw
socket, which works for root and fails for a user. musl's resolver
(`getaddrinfo`) now works: `/etc/hosts`, then `/etc/resolv.conf` over UDP,
with the TCP fallback for truncated answers (`MSG_FASTOPEN` path).

### Time sync (Q13: the plan's default, on)

The default `/etc/inittab` gains
```
ntp:once:/bin/ntpd -n -q -p pool.ntp.org
rtc:shutdown:/bin/hwclock -w -u
```
After `ifup` (a sysinit entry), init starts one NTP step in the background
(log `/var/log/ntp.log`); `ntpd -q` sets the clock only for an offset over
0.125 s and gives up after 10 s when the server cannot be reached (offline
machines keep the RTC's time, UTC). At shutdown the system clock goes back
into the RTC. An untouched pre-U10 default inittab is recognised and
upgraded (`update_default_inittab` knows the old texts); an edited one is
left alone (add the two lines by hand).

## Gate NETPOSIX (`tools/netposix_test.sh`, `tools/netposix_drive.py`)

Host: the musl sysroot, `user/netposix/nptest.c` built three times (CAP_NET;
CAP_NET + CAP_NET_RAW via `user/note_netraw.S`; the default manifest without
CAP_NET), no compiler warning, an ext2 disk, a `CONSOLE=root` image.

The guest runs **inside a private network namespace** (`unshare -rmn`: `lo`
up, a tmpfs `/tmp`, `/etc/resolv.conf` bind-mounted to `nameserver
127.0.0.1` so slirp's DNS forwarder 10.0.2.3 reaches the stub): every host
service is local (the guest sees it as 10.0.2.2), nothing touches the real
network, and no host port can collide (2222/8000 are never used). Host
services: TCP echo, UDP echo, a closed TCP port, a closed UDP port (slirp turns
the host's `ECONNREFUSED` into ICMP port unreachable), a DNS stub (UDP + TCP;
`big.secos` is truncated over UDP), an NTP server running a fixed offset
ahead, a host client for a guest server.

Checks (each `nptest` line is one gate check; `net` and `local` run as root
**and** as an unprivileged user):
- boot, `ifup` wrote `/etc/resolv.conf`, test disk, CA bundle (118 certs),
  ping/ntpd/hwclock + netdb files installed, the inittab time lines;
- `nptest net`: blocking TCP echo of 64 KiB, `SOCK_CLOEXEC`, getsockname/
  getpeername, `TCP_NODELAY`, `MSG_PEEK`+`MSG_WAITALL`, `FIONREAD`,
  `MSG_DONTWAIT`, `SO_RCVTIMEO` (recv and read), `shutdown(SHUT_WR)` → EOF,
  `EPIPE` with `MSG_NOSIGNAL`; non-blocking connect + poll + `SO_ERROR`; a
  refused port (non-blocking and blocking); UDP echo with `MSG_PEEK`, `IP_TTL`
  and `SO_TIMESTAMP` cmsgs, `MSG_TRUNC` (out and in), connected UDP; ICMP port
  unreachable → `ECONNREFUSED` + the `IP_RECVERR` queue entry, then `EAGAIN`;
  socket options and `ENOPROTOOPT`, `AF_INET6` → `EAFNOSUPPORT`, bind port 0,
  `EADDRINUSE`, a user's bind below 1024 → `EACCES`; DNS through musl's
  resolver (A record, TCP fallback with 30 records, NXDOMAIN, `/etc/hosts`
  wins, `/etc/services` + `/etc/protocols`); ping sockets: echo reply with the
  kernel's identifier, non-echo refused, bind chooses the identifier, two
  concurrent sockets (fork) each see only their replies, ICMP net unreachable
  (slirp, for 192.0.2.1 with no route) delivered with `IP_RECVERR`, `IP_TTL`;
- `nptest local`: AF_UNIX stream (100 KB, `SO_PEERCRED`, `accept4`), EOF/EPIPE
  after close, socketpair, dgram with abstract names + autobind, refused/
  missing names; epoll (blocking wake by a pipe, eventfd, `EEXIST`, ONESHOT,
  DEL/`ENOENT`, regular file `EPERM`, closed-fd registration dropped,
  `EPOLLET`, every free fd at once); eventfd (semaphore, blocking, `EINVAL`);
  timerfd (relative, interval, gettime, disarm, ABSTIME on REALTIME);
  `alarm`/`pause`, interval `setitimer`, `EINTR` of a blocked read;
- AF_UNIX path permissions between two users (`EACCES` on a 0700 socket file,
  works after `chmod 0777`);
- raw sockets: a user without `CAP_NET_RAW` → `EPERM` (audited), root → the
  echo reply with its IP header, a user running a `CAP_NET_RAW`-signed
  program → allowed; a program without `CAP_NET` → `EACCES` for AF_INET but
  AF_UNIX works;
- iputils ping: a user (`SOCK_DGRAM`, 3/3), root with `-e 0` (`SOCK_RAW`, 2/2),
  a user with `-e 0` refused, two concurrent pings 6/6 each without
  duplicates, "Destination Net Unreachable" for 192.0.2.1, `-t 1`;
- time: init's boot-time NTP step happened (pool.ntp.org → the stub, clock ≈
  host + offset), a manual `ntpd -n -q` follows the server's +5000 s move,
  `hwclock -w` then `-r` through `/dev/rtc0` shows the NTP time;
- a guest TCP server (`accept4`) echoes 3 host connections (hostfwd);
- the socket table is back to its baseline (`netstress stats`), `poweroff`
  works (runs the `rtc:shutdown` entry), no panic/EXC/SMAP/lockdep line.

## Tests run

| Run | Result |
|---|---|
| NETPOSIX, in-kernel e1000, `-smp 2` | see below |
| NETPOSIX `--udrv` (q35 + intel-iommu,intremap=on + e1000e, ring-3 driver), `-smp 2` | |
| NETPOSIX `--lockdep --smp 4` (`-DSECOS_LOCKDEP` image) | |
| NETSMP (`tools/net_stress_drive.py`, lockdep image, smp4-msix / smp4-poll / smp1-msix) | |
| M58 gate, MUSL-BASE, NANO, persist, smoke `--mb2` / `--uefi` | |
| warnings (`make`, `make CFLAGS_EXTRA=-DSECOS_LOCKDEP`), base 10179d8 vs U10 | 0 / 0 vs 0 / 0 |

## Deviations from the plan

- **epoll with 500 fds**: the fd table is still 32 entries (U4a grows it), so
  the gate fills every free fd instead (about 25 sockets); the interest list
  itself takes up to 4096 entries. Re-run with 500 once U4a lands.
- **setitimer/getitimer/alarm** (U2 territory) came in here: ntpd and ping
  need them.
- **`pause()`** in the translation layer (a gap from U1).
- **CA bundle as a system data file** in the kernel image (200 KB), not a
  package: no package channel ships updates yet (U6 has no update service);
  moving it into a signed `.spkg` is a one-line change in `kernel/sysroot.c`.
- The plan's gate driver name `netposix_drive.py` is kept; the gate entry
  point is `tools/netposix_test.sh` as the step asked.

## Bugs found on the way

- **ioctl requests sign-extended** (musl): any request with bit 31 set (all
  `_IOR` reads, e.g. `RTC_RD_TIME`, `FIONREAD` is fine) never matched.
  Fixed in `ksys_ioctl`.
- **`pause()` returned immediately** for every musl program (`SYS_pause` was
  `-ENOSYS`).
- **IPv4 accepted fragments as whole datagrams and never checked the header
  checksum**; ICMP checksums were not checked either.
- **The first UDP datagram to a new neighbour was lost** (ARP miss → the send
  failed with `ENOBUFS`); DNS's first query relied on a retry.

## For U11 and later

- TLS: `/etc/ssl/certs/ca-certificates.crt` is in place; `curl --cacert` is
  not needed. curl/wget need `CAP_NET`; OpenSSL's `getrandom` path is U2's
  (the layer emulates it on `/dev/urandom`).
- curl uses `poll`, non-blocking connect + `SO_ERROR`, `MSG_NOSIGNAL`,
  `TCP_NODELAY`, `SO_KEEPALIVE` (+ `TCP_KEEPIDLE/INTVL`, stored only): all
  there. wget uses `select` (the layer's poll emulation, 64 fds).
- **No loopback interface**: `127.0.0.1` is unreachable (`connect` →
  `ENETUNREACH`, bind → `EADDRNOTAVAIL`). Test servers must listen on the
  host (10.0.2.2) or on the guest's own address. A `lo` device is the next
  network item.
- No IPv6 (`EAFNOSUPPORT`; musl falls back cleanly), no IP reassembly, no
  `SCM_RIGHTS`, no `IP_PKTINFO` delivery (ntpd `-l` needs it), keepalive
  probes not sent.
- AF_INET readiness is still polled every 2 ms (no RX wake hook): fine for
  clients, a latency floor for servers under load.
- The clock is stepped at boot when the network answers: a TLS gate that
  moves the guest clock should stop `ntpd` first or run with no DNS.
