# SSH into SecOS (Dropbear)

SecOS ships the **Dropbear** SSH server, built from source and signed like every
other ring-3 program. Since M42 it is **multi-user**: every account in
`/etc/passwd` may log in with a public key from `~user/.ssh/authorized_keys`
or with its password from `/etc/shadow`; root may log in with a key only (see
[Authentication modes](#authentication-modes)). Exec channels and pty
sessions both work.

```
$ ssh -p 2222 root@localhost 'echo hi; uname -a'
hi
SecOS secos 0.1.0-dev x86_64
```

## The port

| | |
|---|---|
| Upstream | Dropbear **2026.94** (MIT), `https://matt.ucc.asn.au/dropbear/releases/dropbear-2026.94.tar.bz2` |
| SHA-256 | `e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81` (pinned in `tools/port-dropbear.sh`; the build refuses a mismatch) |
| Signature | `dropbear-2026.94.tar.bz2.asc` checked against the Dropbear release key `F734 7EF2 EE2E 07A2 6762 8CA9 4493 1494 F29C 6773` when the hash was pinned |
| Binary | `dropbearmulti` (dropbear + dropbearkey + dbclient), about 290 KB, embedded as `crypto/user_dropbear_elf.h` and installed at boot as `/bin/dropbearmulti` |
| Rebuild | `make port-dropbear` (needs network the first time, plus python3 `cryptography` for signing). The build is reproducible: same sources give a byte-identical header. |

Files:
- `tools/port-dropbear.sh` fetches and verifies the tarball. It installs the
  hand-written `config.h` and `localoptions.h` (Dropbear's `configure` cannot
  probe SecOS), cross-compiles against the SecOS libc, links, signs and embeds.
- `user/dropbear_config.h` asserts only the `HAVE_*` features that SecOS
  actually provides.
- `user/dropbear_localoptions.h` holds the security policy (below).
- `user/dropbear_port.h` is force-included. It renames libsecos's non-POSIX
  socket prototypes away and adds the POSIX declarations the port needs.
- `user/dropbear_compat.c` holds the shims: POSIX sockets, getaddrinfo, select
  over poll, writev, waitpid, sigaction, time, O_EXCL / O_NOFOLLOW and the PTY
  calls. Credentials, the account database and `crypt()` are not shimmed: they
  are the SecOS libc's and `user/libcrypt.a` (musl), linked last. Generic ones are tagged
  `TODO(integration): move to libc`.
- `user/dropbear-include/` provides `sys/socket.h`, `netinet/*`, `arpa/inet.h`,
  `netdb.h`, `poll.h`, `sys/select.h`, `syslog.h` and `sys/un.h`.
- `user/note_dropbear.S` is the signed manifest (below).

The port compiles with `-Werror=implicit-function-declaration
-Werror=int-conversion -Werror=int-to-pointer-cast`, so a missing prototype
fails the build instead of truncating a pointer.

## Configuration (`user/dropbear_localoptions.h`)

- **Authentication:** public key and password (`DROPBEAR_SVR_PASSWORD_AUTH 1`,
  against `/etc/shadow`); PAM is compiled out, and so are the client's
  password and askpass paths. See [Authentication modes](#authentication-modes).
- **Identity:** multi-user (`DROPBEAR_SVR_MULTIUSER 1`,
  `DROPBEAR_SVR_DROP_PRIVS 1`). Accounts come from `/etc/passwd`,
  `/etc/shadow` and `/etc/group` through the libc. The shell must be listed in
  `/etc/shells` (or, without that file, be `/bin/bash` or `/bin/sh`).
- **Algorithms:** ed25519 host and user keys only (no RSA, DSS, ECDSA or SK).
  Key exchange is `mlkem768x25519-sha256` (post-quantum hybrid) or
  `curve25519-sha256`. Ciphers are chacha20-poly1305, aes128/256-gcm and
  aes128/256-ctr. MACs are hmac-sha2-256/512. No SHA-1 or CBC anywhere.
- **Off:** TCP, stream, agent and X11 forwarding, proxy/netcat helpers, zlib,
  syslog, utmp/wtmp/lastlog, MOTD, sftp, and re-exec per connection (it needs
  `/proc/self/exe` and real execve).
- **On:** `authorized_keys` options are honoured (`command=`, `no-pty`, ...).
  `MAX_AUTH_TRIES` is 6.

## Authentication modes

| Account | Public key | Password |
|---------|------------|----------|
| root (uid 0) | yes, `/root/.ssh/authorized_keys` | **refused**, even when correct |
| any other account | yes, `~user/.ssh/authorized_keys` | yes, `/etc/shadow` hash |
| locked account (`!...` or `*` in `/etc/shadow`) | yes (as on Linux; lock with `usermod -L` + remove the keys, or set the shell to `/bin/false`) | refused |
| account with an empty hash | yes | refused ("blank password") |
| unknown user | refused | refused |

- **Keys.** Dropbear's own `authorized_keys` checks apply: the file, `~/.ssh`
  and the home directory must be owned by the user or root and not writable
  by group or others, otherwise the key is ignored. `command=`, `no-pty` and
  the other key options are honoured.
- **Passwords.** The hash is read with `getspnam()` (root reads
  `/etc/shadow`, mode 0600, before the privilege drop) and checked with the
  libc `crypt()` (musl SHA-512-crypt `$6$`, SHA-256-crypt `$5$`, bcrypt `$2b$`;
  any other prefix, including a leading `!`, makes `crypt()` fail, which
  Dropbear reports as a locked account) and a constant-time comparison. No
  hash code is written here.
- **Root password logins** are off by default in the SecOS build: the port
  makes upstream's `-g` ("disable password logins for root") the default, so
  it holds even with an `/etc/inittab` that does not pass `-g` (persistent
  roots keep their inittab). Root logs in with a key; to administer with a
  password, log in as a user and use `su`/`sudo`. Turning root password logins
  on needs a rebuild (remove the `norootpass` patch in `tools/port-dropbear.sh`).
- **After authentication** the connection's process (forked by the listener,
  which stays root) calls `setgid(pw_gid)`, `initgroups(user, pw_gid)` and
  `setuid(pw_uid)` at once (`DROP_PRIVS`), so the channels, the pty
  allocation and the shell all run as the user. The session gets `HOME`,
  `SHELL`, `USER`, `LOGNAME` and the cwd from `/etc/passwd`, and `PATH` is
  `/bin:/usr/bin` (root: `/bin:/usr/bin:/sbin:/usr/sbin`).
- **Pty slave.** The kernel creates `/dev/pts/N` owned by the process that
  opened `/dev/ptmx` (here the user), group `tty` (5), mode 0620, which is
  what `pty_setowner` expects, so a user's terminal is never root's. At the
  end of a session Dropbear tries to give the (already closed) slave back to
  root and logs `chown /dev/pts/N 0 0 failed: Input/output error`; this is
  harmless.
- **Set-id manifest flag.** `dropbearmulti`'s manifest does **not** carry
  `MANIFEST_FLAG_SETID`. That flag only lets the kernel honour the S_ISUID /
  S_ISGID bits of the file being exec'd; Dropbear is started as root by init
  and only lowers its ids (root may set any id, see `kernel/cred.c`), so it
  needs no set-id execution. Leaving the flag off means a `chmod u+s` on
  `dropbearmulti` (which would also make `dbclient` set-uid) has no effect.

### Signed manifest: least privilege

`dropbearmulti` is signed with a **confined** manifest:
`CAP_ENFORCE | CAP_NET | CAP_FS_READ | CAP_FS_WRITE | CAP_PROC | CAP_IPC |
CAP_SIGNAL | CAP_TIME` (`0xFF0`). These cover sockets, the host key,
`authorized_keys` and the pid file, fork/exec of the session shell, pipes to
it, and signals and timeouts. Everything else is denied and audited, including
`CAP_POWER` and Driver Space. The flags are covered by the Ed25519 signature,
so the program cannot widen them. The session shell is a separate signed
program (`/bin/bash`) and runs under its own manifest.

## Quick start (QEMU, host port 2222 to guest port 22)

Short version:

```
ssh-keygen -t ed25519 -f ~/.ssh/secos_ed25519                  # once
make run-ssh SSH_AUTHORIZED_KEYS=~/.ssh/secos_ed25519.pub       # serial console + sshd
ssh -i ~/.ssh/secos_ed25519 -p 2222 root@localhost              # from another terminal
```

`make run-ssh` (like `run-gui`, `run-serial`, ...) boots from the persistent
system disk `secos-root.img`, created on the first run and never overwritten by
a build ([docs/devlog/M41-persistent-root.md](devlog/M41-persistent-root.md)).
The host key Dropbear generates on the first connection is stored there, so the
fingerprint stays the same across reboots and kernel upgrades, and so do keys
you add to `authorized_keys` by hand. `SSH_PORT=` picks another host port,
`ROOT=ram` boots without the disk (new host key every boot), and
`make reset-root` deletes the disk (and with it the host key).

Step by step:

1. Build with your public key baked in (it is compiled into the kernel and
   added to `/root/.ssh/authorized_keys` at boot; never commit a key):
   ```
   ssh-keygen -t ed25519 -f ~/.ssh/secos_ed25519      # once
   make iso SSH_AUTHORIZED_KEYS=~/.ssh/secos_ed25519.pub
   ```
2. Boot with a NIC and a host port forward (`make run-ssh` does exactly
   this). Use `-cpu max` (or `-enable-kvm -cpu host`) so the guest has
   RDRAND/RDSEED, which seed the kernel RNG (see
   [Security notes](#security-notes)):
   ```
   qemu-system-x86_64 -cdrom myos.iso -boot d -m 512M -cpu max \
       -netdev user,id=n0,hostfwd=tcp::2222-:22 -device e1000,netdev=n0 \
       -serial stdio -display none -no-reboot
   ```
   `/bin/init` (PID 1) takes a DHCP lease (`ifup`), starts
   `dropbearmulti dropbear -F -R -p 22` as a supervised service and opens a
   bash on the console. There are no manual steps.
3. From the host (WSL):
   ```
   ssh -i ~/.ssh/secos_ed25519 -p 2222 root@localhost
   ```
   With the persistent root (the `make run*` default) the host key survives
   reboots. On a RAMFS root (`ROOT=ram`, or an ISO booted without a system
   disk) it changes every boot: add `-o StrictHostKeyChecking=no
   -o UserKnownHostsFile=/dev/null` for such throwaway VMs.

The same `kernel.bin` carries the keys on the UEFI path (`make uefi
SSH_AUTHORIZED_KEYS=...`). Without `SSH_AUTHORIZED_KEYS` there is no
`authorized_keys` file and every login is refused until you install a key by
hand (next section).

The service and its boot order are in `/etc/inittab` (format:
[docs/devlog/M40-init.md](devlog/M40-init.md)). Dropbear's log is
`/var/log/sshd.log`; syslog is compiled out. `-R` generates the ed25519 host key
on the first connection. To use a key generated ahead of time instead, run
`dropbearmulti dropbearkey -t ed25519 -f /etc/dropbear/dropbear_ed25519_host_key`
and drop `-R` from the inittab line. `ifconfig` shows the lease; `ifup` takes a
new one.

The multi-call binary dispatches on its first argument:
`dropbearmulti dropbear|dropbearkey|dbclient ...`. On the persistent root the
kernel also installs `/bin/dropbear`, `/bin/dropbearkey` and `/bin/dbclient` as
symlinks to it; the RAMFS root has no symlinks, so there only `dropbearmulti`
exists.

## Installing your key by hand

The baked key (above) is the normal way. Dropbear reads
`~root/.ssh/authorized_keys`, which is `/root/.ssh/authorized_keys`.
`dropbear -D <dir>` selects another directory, for example `/etc/dropbear`.
At every boot the kernel appends each baked key line that is missing from the
file and leaves the other lines alone, so on the persistent root a key you add
by hand stays, and a key baked into a later build is added next to it. (The
kernel never removes a key: delete it from the file yourself.) To add a key without
rebuilding, on the host:

```
ssh-keygen -t ed25519 -f ~/.ssh/secos_ed25519
cat ~/.ssh/secos_ed25519.pub
```

In SecOS bash, paste the line. It is about 100 characters, which fits in one
console line:

```
mkdir /root/.ssh
echo 'ssh-ed25519 AAAA... you@host' > /root/.ssh/authorized_keys
```

Then connect with `ssh -i ~/.ssh/secos_ed25519 -p 2222 root@localhost ...`.

On the persistent root (`make run*`, `secos-root.img`) both the key file and
the host key survive reboots. On a RAMFS root they live in RAM and are lost.

## Security model

- **Two gates.** The code signature decides which programs may run at all;
  since M42 the credentials decide who they act for. A user's session runs
  with the user's uid/gid/groups, so file permissions apply (it cannot read
  `/etc/shadow`, write `/etc`, or chown other users' files). A root key is
  still equivalent to console access: protect it, and use `command="..."` /
  `no-pty` options in `authorized_keys` to restrict a key.
- **Passwords travel inside the encrypted channel** and are checked against
  `/etc/shadow`; `MAX_AUTH_TRIES` (6) bounds the guesses per connection. Root
  never authenticates with a password.
- **Host key on ramfs:** a key generated with `-R` lives in RAM. Each boot gets
  a new identity, so clients that pin keys will warn, and a MITM on first
  contact cannot be told apart from a reboot. Use a persistent root, or
  provision `/etc/dropbear/dropbear_ed25519_host_key` from outside, to keep a
  stable host identity. The private host key is created mode 0600 by root, so
  since M42 other users cannot read it.
- **Randomness:** see below. Host keys and key-exchange secrets come from
  `/dev/urandom`.

## Security notes (attack surface review)

Dropbear's pre-auth code is now exposed to the network, and so is the SecOS
TCP/IP stack under it. Issues found and fixed on this branch:

1. **`/dev/urandom` was a tick-seeded xorshift64**: a few thousand possible
   streams, so host keys and every DH/ML-KEM secret would have been guessable.
   It is replaced by `crypto/csprng.c`, a SHA-256 hash DRBG seeded from
   RDSEED/RDRAND (when CPUID reports them), TSC jitter and per-tick IRQ timing.
   It rekeys after every request (forward secrecy), reseeds every 1 MiB and is
   SMP-safe. **QEMU's default `qemu64` CPU has no RDRAND**, and the boot log
   then says `TSC jitter only`. That is much better than before but weak at
   early boot, so use `-cpu max` or `-cpu host`.
2. **TCP slot reuse under an accepted socket.** A peer RST (or a normal close
   or retransmit give-up) freed an accepted connection's slot while the socket
   still pointed at it. The next incoming SYN reused the slot, and the old fd
   then read and injected into the new connection's stream. Accepted slots are
   now owned by the socket until it is released.
3. **Blind RST injection.** Any RST matching the 4-tuple killed a connection.
   RSTs are now honoured only at `rcv_nxt` exactly (RFC 5961); an in-window RST
   draws a challenge ACK.
4. **Predictable ISNs.** ISNs were `ticks ^ (counter += 0x9E37)` and now come
   from the CSPRNG.
5. **SYN flood.** Every SYN allocated one of the 16 global TCP slots (128 KB
   each) for about 40 s, so 16 SYNs blocked all TCP, outbound included. Each
   listener now holds at most 4 not-yet-accepted children, and one listener
   per port.
6. **Ring-0 spinning.** `accept`/`recv` spun up to 30 s in the kernel and
   starved the CPU, and close spun up to 4 s. All socket calls are now
   non-blocking at the core; a blocking fd sleeps in the scheduler. Close is
   asynchronous: an orphaned connection finishes FIN in `tcp_tick`.
7. **Type confusion.** `lseek`/`mmap` on a pipe fd treated the `pipe_t*` as a
   `vfs_inode_t*`. Both now fail. Socket fds keep `inode == NULL`, so file
   paths cannot misinterpret them.
8. **Shared static buffer.** `sendto`/`recvfrom` used a shared static bounce
   buffer, which races on SMP. Each call now has its own buffer.

Still open (not fixed here):
- **The network stack has no locking.** The timer ISR (`net_tick`) and
  syscalls on other CPUs touch `g_conns`, the socket table and the shared TX
  scratch buffers (`g_seg`/`g_cs`) concurrently. With `-smp >1`, concurrent
  sessions can corrupt segments or connection state. This needs a net-stack
  lock (big rock).
- **Out-of-order segments are dropped.** That is correct, but a lossy link
  causes heavy retransmission. There is no window scaling (64 KB window).
- **`connect()` still spins** for the TCP handshake (only `dbclient` uses it).
- **No half-close:** `shutdown()` is a no-op (FIN goes out on the last close),
  and there is no SIGPIPE on a socket write after the peer closed (EPIPE
  only).
- **Pipes lose data under load** (pre-existing on main): `seq 1 2000 | wc`
  reports about 1998 lines. ssh sessions stream through pipes, so long command
  output can be truncated. This is reported separately.
- A socket fd inherited across fork or exec keeps working without `CAP_NET`.
  The capability gates socket creation and socket-specific calls, not plain
  `read`/`write` on an inherited fd. Dropbear closes its sockets in the session
  child before exec.

## Kernel interfaces Dropbear uses

| Need | Status |
|---|---|
| Sockets as fds: `read`/`write`/`close`/`dup2`/fork sharing, `SOCK_NONBLOCK`, `getsockname`/`getpeername` (`SYS_SOCKNAME` 80) | **done** on this branch (`net/socket.c`, `kernel/syscall*.c`) |
| `poll`/`select` | **done** (integ/phase2): `SYS_POLL` 78; `ki_fd_revents()` reports socket fds through `socket_poll()` and pty ends through `pty_poll()`; libc's +64 socket offset is gone. |
| O_NONBLOCK on pipes | **done**: a pipe read/write that would block returns -EAGAIN when O_NONBLOCK is set (`pipe2` flag or `F_SETFL`). |
| fork + exec of the shell | works with today's spawn+wait emulation. Real in-place `execve` (60) makes pids and job control exact. |
| `clock_gettime`/`nanosleep`/`fsync`/`link` | used when present (65/67/70/72), with fallbacks otherwise |
| PTY | **missing**: see the spec below |

## PTY: the kernel interface needed (for `ssh` interactive / `ssh -t`)

Dropbear is built with `USE_DEV_PTMX`, and the compat layer already implements
the standard Linux calls on top of the interface below, so it will work
unchanged once the kernel provides it. Today the `open("/dev/ptmx")` fails, and
Dropbear answers the `pty-req` with a failure (`PTY allocation request failed`);
ssh then continues without a tty.

1. **`/dev/ptmx`** (devfs char node, mode 0666). Each `open(O_RDWR|O_NOCTTY)`
   allocates a new pair N and returns the **master** fd. The pty object is
   reference counted like a pipe (fork/dup/close/exit). When the last master
   reference closes, the slave side gets hangup: reads return 0 or EIO, writes
   return EIO, and the session leader gets SIGHUP.
2. **`ioctl(master, TIOCGPTN /*0x80045430*/, unsigned* n)`** returns N.
   `ptsname()` builds `/dev/pts/N` from it.
3. **`ioctl(master, TIOCSPTLCK /*0x40045431*/, int* lock)`**: the slave cannot
   be opened while locked. A pair starts locked; `unlockpt()` passes 0.
   `grantpt()` is a no-op (the slave is created owned by the opener, group
   tty, mode 0620).
4. **`/dev/pts/N`** (dynamic devfs directory `/dev/pts`). Opening it returns a
   **slave** fd. The slave is a full TTY with the same line discipline as the
   console (`kernel/tty.c`): cooked mode (echo, erase, kill, ^D EOF), raw mode
   via `TCSETS`, VMIN/VTIME, and ISIG (^C to SIGINT, ^Z to SIGTSTP, ^\ to
   SIGQUIT for the foreground pgrp). Output post-processing (OPOST|ONLCR, \n to
   \r\n) must be honoured. Bytes written to the master are slave input (through
   the line discipline). Bytes written to the slave are master output.
5. **Sessions and controlling tty:** `setsid()` must create a real session (the
   process has no controlling tty afterwards).
   `ioctl(slave, TIOCSCTTY /*0x540E*/, 0)` makes the slave the controlling tty
   of the caller's session. `open("/dev/tty")` must then resolve to the
   controlling tty (Dropbear verifies this), and must fail with ENXIO when
   there is none (`TIOCNOTTY` 0x5422 drops it).
   `tcgetpgrp`/`tcsetpgrp` (`TIOCGPGRP`/`TIOCSPGRP`) act per tty, not on the
   single global `foreground_pgid`. A background pgrp reading the slave gets
   SIGTTIN, and writing with TOSTOP gets SIGTTOU.
6. **Window size:** `TIOCSWINSZ /*0x5414*/` on the master (or slave) stores a
   `struct winsize` per pty and sends **SIGWINCH (28)** to the slave's
   foreground pgrp. `TIOCGWINSZ` on the slave returns it. Dropbear calls
   `TIOCSWINSZ` on `pty-req` and on every `window-change` message.
7. **termios layout:** `c_cc` indices must follow Linux (`VINTR 0, VQUIT 1,
   VERASE 2, VKILL 3, VEOF 4, VTIME 5, VMIN 6, ...`). SecOS currently uses
   `VMIN 4 / VTIME 5`, which clashes with `VEOF 4`. Dropbear applies the
   client's terminal modes with `tcsetattr` on the slave. Flags it sets:
   `IGNPAR ISTRIP INLCR IGNCR ICRNL IXON IXANY IXOFF ISIG ICANON ECHO ECHOE
   ECHOK ECHONL NOFLSH TOSTOP IEXTEN OPOST ONLCR CS7 CS8 PARENB PARODD`. All are
   declared with Linux values in `user/dropbear_port.h`; the kernel should
   accept and store them even when they have no effect.
8. **poll()** on both ends: master POLLIN when slave output is pending, POLLOUT
   when the input queue has room; POLLHUP on the master when every slave fd is
   closed. The same goes for the slave.
9. Nice to have: `chown`/`chmod` on `/dev/pts/N` succeed (Dropbear's
   `pty_setowner` calls them and exits if `stat` fails), and `ttyname()` works
   (`TIOCGPTN` + `/dev/pts`, or a readlink of `/proc/self/fd/N`).

## Selftest gate

`tools/selftest.sh`, section **SSH: Dropbear**, log `/tmp/secos_selftest_ssh.log`.
The gate boots the default image with `-netdev user,hostfwd=tcp:127.0.0.1:<port>-:22`,
using a per-run port in 22000-22999. init has already taken the lease and
started dropbear. The gate installs a key generated at test time by hand (no
private key is committed) and checks from the host:
exec output and exit status 0; stderr and exit status 3; wrong key, unknown
user and a key-less root client refused; a 6th sequential session; pty sessions
and window changes; the host key generated on first use (`/var/log/sshd.log`);
no `[EXC]`/`[PANIC]`. It SKIPs if the host lacks `ssh`/`ssh-keygen`, or if the
kernel has no `SYS_POLL`.

The **MUPORTS** gate (`tools/mu_ports_test.sh`, port 26000-26999) covers the
multi-user modes: key and password login as a test user written straight into
`/etc/passwd`/`/etc/shadow`/`/etc/group` (uid, groups, HOME, cwd, pty slave
`tuser:tty 0620`, `/etc/shadow` unreadable), wrong and locked passwords
refused, root's correct password refused while root's key works.

The **INIT** gate (end of `tools/selftest.sh`, port 23000-23999) covers the
zero-step boot: an image built with `SSH_AUTHORIZED_KEYS=<throwaway key>`
must accept `ssh ... 'uname -a; ps'` with no console input at all.

## What's pending

- **PTY sessions work** on integ/phase2 (feat/pty merged): `ssh -t` runs the
  command on `/dev/pts/N`, the pty-req window size is applied and a
  window-change resizes the pty and raises SIGWINCH (selftest SSH gate). Note
  that Dropbear closes the pty master when the client sends EOF (upstream
  behaviour), so `ssh -tt host cmd </dev/null` hangs the shell up early.
  Still to check by hand: the login shell (`-bash`) of a session with no
  command on a real terminal.
- **Init-managed service: done** (M40). `/bin/init` takes the DHCP lease
  through `SYS_NETCTL` and supervises Dropbear. The host key persists on a
  persistent root. See [docs/devlog/M40-init.md](devlog/M40-init.md).
- Kernel O_NONBLOCK for pipes is done (the user-space emulation and the
  poll/select shims are gone; passwd/group come from libc's `/etc/passwd`
  reader, shared with bash). Still in the port: the POSIX sockets API (libc
  keeps the SecOS-native `socket(type)` signatures), getaddrinfo, inet_*,
  sigaction, sscanf.
- `scp` (optional) and `sftp-server` are not built.
- A real DNS-backed `getaddrinfo` (dbclient to host names). Only numeric IPv4
  and `localhost` work today.
