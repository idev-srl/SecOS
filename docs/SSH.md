# SSH into SecOS (Dropbear)

SecOS ships the **Dropbear** SSH server, built from source and signed like every
other ring-3 program. It supports **public-key authentication only**, for a
single `root` identity. This is stage 1: non-interactive `ssh host 'command'`
works end to end. An interactive shell over a PTY is still pending (see
[What's pending](#whats-pending)).

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
  over poll, writev, waitpid, sigaction, the root account, time, O_EXCL /
  O_NOFOLLOW, O_NONBLOCK emulation and the PTY calls. Generic ones are tagged
  `TODO(integration): move to libc`.
- `user/dropbear-include/` provides `sys/socket.h`, `netinet/*`, `arpa/inet.h`,
  `netdb.h`, `poll.h`, `sys/select.h`, `syslog.h` and `sys/un.h`.
- `user/note_dropbear.S` is the signed manifest (below).

The port compiles with `-Werror=implicit-function-declaration
-Werror=int-conversion -Werror=int-to-pointer-cast`, so a missing prototype
fails the build instead of truncating a pointer.

## Configuration (`user/dropbear_localoptions.h`)

- **Authentication:** public key only. Password and PAM auth are compiled out,
  and so are the client's password and askpass paths.
- **Identity:** non-multiuser (`DROPBEAR_SVR_MULTIUSER 0`). The only account
  that can log in is the one the server runs as. The compat layer's passwd
  database contains just `root` (uid 0, home `/root`, shell `/bin/bash`,
  password locked). Any other user name does not exist and is rejected before
  keys are checked. Dropbear also confirms that `getgroups()` fails with
  ENOSYS, i.e. that the kernel really has no users.
- **Algorithms:** ed25519 host and user keys only (no RSA, DSS, ECDSA or SK).
  Key exchange is `mlkem768x25519-sha256` (post-quantum hybrid) or
  `curve25519-sha256`. Ciphers are chacha20-poly1305, aes128/256-gcm and
  aes128/256-ctr. MACs are hmac-sha2-256/512. No SHA-1 or CBC anywhere.
- **Off:** TCP, stream, agent and X11 forwarding, proxy/netcat helpers, zlib,
  syslog, utmp/wtmp/lastlog, MOTD, sftp, and re-exec per connection (it needs
  `/proc/self/exe` and real execve).
- **On:** `authorized_keys` options are honoured (`command=`, `no-pty`, ...).
  `MAX_AUTH_TRIES` is 6.

### Signed manifest: least privilege

`dropbearmulti` is signed with a **confined** manifest:
`CAP_ENFORCE | CAP_NET | CAP_FS_READ | CAP_FS_WRITE | CAP_PROC | CAP_IPC |
CAP_SIGNAL | CAP_TIME` (`0xFF0`). These cover sockets, the host key,
`authorized_keys` and the pid file, fork/exec of the session shell, pipes to
it, and signals and timeouts. Everything else is denied and audited, including
`CAP_POWER` and Driver Space. The flags are covered by the Ed25519 signature,
so the program cannot widen them. The session shell is a separate signed
program (`/bin/bash`) and runs under its own manifest.

## How to run it (QEMU, host port 2222 to guest port 22)

1. Boot with a NIC and a host port forward (`make run-ssh` does exactly this). Use `-cpu max` (or
   `-enable-kvm -cpu host`) so the guest has RDRAND/RDSEED, which seed the
   kernel RNG (see [Security notes](#security-notes)):
   ```
   make iso
   qemu-system-x86_64 -cdrom myos.iso -boot d -m 512M -cpu max \
       -netdev user,id=n0,hostfwd=tcp::2222-:22 -device e1000,netdev=n0 \
       -serial stdio -display none -no-reboot
   ```
2. Bring the network up. DHCP currently lives in the kernel shell, so leave
   bash with `exit`, run `dhcp`, then start bash again with `run /bin/bash`.
3. Install your public key, as in the next section.
4. Start the server in the foreground or as a background job. `-R` generates
   the ed25519 host key on first use:
   ```
   dropbearmulti dropbear -F -R -p 22 &
   ```
   Dropbear logs to stderr; syslog is compiled out, so there is no `-E` flag.
   To use a key generated ahead of time instead:
   `dropbearmulti dropbearkey -t ed25519 -f /etc/dropbear/dropbear_ed25519_host_key`
   followed by `dropbearmulti dropbear -F -p 22`.
5. From the host (WSL):
   ```
   ssh -p 2222 root@localhost 'echo hi; uname -a'
   ```
   The host key changes every boot unless the root filesystem is persistent.
   Add `-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null` for
   throwaway VMs, or pin the fingerprint that Dropbear logs.

The multi-call binary dispatches on its first argument:
`dropbearmulti dropbear|dropbearkey|dbclient ...`. The ramfs root has no
symlinks, so `/bin/dropbear` does not exist. On a persistent ext2/ext4 root you
can `ln -s dropbearmulti /bin/dropbear`.

## Installing your key

Dropbear reads `~root/.ssh/authorized_keys`, which is
`/root/.ssh/authorized_keys`. `dropbear -D <dir>` selects another directory,
for example `/etc/dropbear`. On the host:

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

On the default ramfs root, the key file and the host key live in RAM and are
lost at reboot. On a persistent ext2/ext4 root (`make sysdisk-ext4`, root
adopted through `/.secosroot`), both survive.

## Security model

- **The signature is the trust boundary.** SecOS has no users or passwords, so
  "who may log in" is "who holds a private key listed in `authorized_keys`".
  An authenticated session gets a root bash, which can run any signed program.
  An SSH key is therefore equivalent to console access. Protect it accordingly,
  and use `command="..."` / `no-pty` options in `authorized_keys` to restrict a
  key.
- **Password auth cannot be enabled at runtime:** the code is not in the
  binary. The selftest checks that a password-only client is refused with
  `Permission denied (publickey)`.
- **Host key on ramfs:** a key generated with `-R` lives in RAM. Each boot gets
  a new identity, so clients that pin keys will warn, and a MITM on first
  contact cannot be told apart from a reboot. Use a persistent root, or
  provision `/etc/dropbear/dropbear_ed25519_host_key` from outside, to keep a
  stable host identity. SecOS does not enforce file modes, so the private host
  key is readable by any signed program on the box. This follows from
  signature-as-trust: any signed program is trusted.
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
| `poll`/`select` | `SYS_POLL` 78 from feat/kernel-ifaces. `kernel/kiface.c` `ki_fd_revents()` must call `socket_poll(pid, e->is_sock - 1)` for `is_sock` entries, and the interim libc "+64 socket fd" offset must go (both done on the integration test branch). Without it, dropbear prints `this SecOS kernel has no poll()` and exits. |
| O_NONBLOCK on pipes | **emulated in user space** (`db_fcntl`/`db_read`/`db_write` poll first). TODO: the kernel should return -EAGAIN for an O_NONBLOCK pipe (`SYS_FCNTL` 61 already stores the flag). |
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
   `grantpt()` is a no-op (single user; nodes are created 0620 root).
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
using a per-run port in 22000-22999. It takes a lease, installs a key generated
at test time (no private key is committed), starts dropbear, and checks from
the host:
exec output and exit status 0; stderr and exit status 3; wrong key, unknown
user and password-only client refused; a 6th sequential session; no
`[EXC]`/`[PANIC]`. It SKIPs if the host lacks `ssh`/`ssh-keygen`, or if the
kernel has no `SYS_POLL`.

## What's pending

- **PTY + interactive shell** (`ssh -p 2222 root@localhost` with no command):
  needs the kernel interface above. After that, enable the login shell
  (`-bash`) and window-change handling. `ssh -t` currently degrades to no tty.
- **Init-managed service:** start `dropbear -R -p 22` from `/bin/init`, after
  DHCP, with the host key on the persistent root. DHCP should also run from
  init or userland, not only the kernel shell.
- Kernel O_NONBLOCK for pipes (drop the user-space emulation), and move the
  generic shims (`TODO(integration)`) into libc: sockets API, getaddrinfo,
  inet_*, poll/select, writev, sigaction, sscanf.
- `scp` (optional) and `sftp-server` are not built.
- A real DNS-backed `getaddrinfo` (dbclient to host names). Only numeric IPv4
  and `localhost` work today.
