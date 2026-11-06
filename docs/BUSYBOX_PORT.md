# BusyBox on SecOS

SecOS ships **BusyBox 1.36.1**, built from unmodified upstream source, as its
main Unix userland: one signed multi-call binary, `/bin/busybox`, plus a signed
stub under each of its 121 applet names in `/bin`. GNU bash stays the shell
(`/bin/bash`); BusyBox is configured without a shell.

Licensing: BusyBox is GPL-2.0. Its source is fetched at build time and never
committed here. See `docs/THIRD_PARTY.md` for versions, URLs, SHA-256 sums and
the source offer.

## Rebuild

```bash
make port-busybox     # fetch + verify + configure + build + sign + embed
make iso
```

`tools/port-busybox.sh` does the following:

1. Downloads `busybox-1.36.1.tar.bz2` and `musl-1.2.5.tar.gz` into
   `/tmp/secos-busybox` (override with `BUSYBOX_PORT_WORK`) and checks both
   against pinned SHA-256 sums. Only musl's `regex`, `fnmatch` and `getopt`
   sources are used.
2. Configures BusyBox from the hand-written fragment `user/busybox.config`. It
   starts from `make defconfig`, turns off every applet that the fragment does
   not list (`tools/bb_apply_config.py`), and keeps the upstream defaults for
   the `FEATURE_*` options of the applets that remain. Main choices: static
   build, no shell (ash/hush off), no IPv6, no PAM/SELinux/utmp/syslog, MMU
   build (fork + COW), no SHA-NI asm, small `nslookup`, BusyBox's own
   `/etc/passwd` parser.
3. Compiles everything with BusyBox's own Kbuild and these flags:
   `-nostdinc`, the header overlay `user/busybox-include/`, the SecOS libc
   headers, and the force-included `user/busybox_port.h`. `ip` also needs the
   host's Linux UAPI headers, which are searched last. The build uses
   `-Werror=implicit-function-declaration -Werror=int-conversion
   -Werror=int-to-pointer-cast`: on x86-64 an undeclared function returns a
   32-bit `int`, which truncates any pointer it returns. That bug crashed bash.
4. Applies two small source patches (`tools/bb_patch_src.py`): `stat.c` and
   `touch.c` read whole-second `st_*time` because SecOS has no `st_*tim`.
5. Links `user/busybox.elf` with `user/note_net.S` (CAP_NET manifest, needed by
   wget and nslookup), signs it, and embeds it as `crypto/user_busybox_elf.h`.
   It also builds and signs the applet stub `user/bbstub.c` into
   `crypto/user_bbstub_elf.h`, and regenerates the applet list
   `kernel/busybox_applets.h`.

Binary size: **565 KB** for busybox (about 470 KB of text at `-Os` with
`-mcmodel=large`) and 15 KB for the stub. This is well below the 2 MB
`SYS_SPAWN` load buffer and the 16 MB `SYS_EXECVE` image limit, so embedding
is fine and no loading from disk is needed.

## Port layer (all MIT, outside `user/libc.c`)

| File | Role |
|------|------|
| `user/busybox_port.h` | Force-included. It gives `read/write/close/dup/dup2`, the path calls, `isatty`, `exec*` and `exit` new link names (asm labels, same prototype) so they reach the shims. After the SecOS headers are parsed, macros route the POSIX `socket/connect/.../mmap/mount/open/waitpid/ioctl/signal` calls to POSIX-shaped shims. It also renames BusyBox's own `spawn()`, which clashes with libsecos. |
| `user/busybox-include/` | Header overlay. Each header either `#include_next`s the SecOS libc header and adds the missing POSIX parts (types, errno, `O_*`, termios flags, signals/sigaction), or is a complete header (stdio, sockets, netdb, regex, poll, mntent, ...). |
| `user/busybox_compat.c` | The shims (about 1.9k lines). What libc now provides (exec family, time/calendar, poll/select, statfs, sysinfo, klogctl, rename/link/truncate/sync, access, pipe2, ttyname/pty API, termios helpers, strsignal, system, sysconf) was removed in the Phase 2 integration; what is left is BusyBox-specific (POSIX socket shapes, private stdio, errno-setting wrappers, DNS client, mntent, wide chars). |
| `user/bbstub.c` | The applet stub. |

What `busybox_compat.c` provides:

- **errno and paths.** Several libsecos wrappers return a bare -1, so the
  shims set errno; the kernel's own errno (`EACCES`, `EPERM`, `EROFS`,
  `ENOTDIR`, `ENOSPC`, ...) is always kept, and only a bare -1 falls back to a
  best guess (until M42 `open`/`mkdir` replaced every error with `EACCES`, so
  a read-only filesystem said "Permission denied"). Path calls are made
  absolute (lexically normalised). `rename`,
  `link` and `rmdir` are the kernel's atomic syscalls. Also: `O_DIRECTORY`
  checks, `opendir` for large directories, and `realpath`.
- **Private stdio** (`bbio_*` link names). The libc FILE reads one byte per
  syscall and has no `getline`/`scanf`/`popen`. This one buffers reads and
  writes and has a printf engine with `%m %j %z %ll %hh` (floating point is
  delegated to libc) and an `sscanf`.
- **Sockets.** A POSIX API over the SecOS socket syscalls (AF_INET, TCP/UDP).
  A socket is a kernel fd (read/write/close/dup2/fork/exec/poll work on it);
  the layer only keeps the type and addresses per fd and asks `SYS_SOCKNAME`
  for the rest. Also a small UDP DNS client
  (`/etc/hosts` and `/etc/resolv.conf` are honoured; QEMU's 10.0.2.3 is the
  default) behind `getaddrinfo`/`gethostbyname`.
- **Processes.** `waitpid` with `-1`/`WNOHANG` over `SYS_WAIT`/`SYS_WAITANY`.
  `execve`/`execv*`/`execl*` are the kernel's in-place `SYS_EXECVE` (the
  private stdio is flushed first); `execvp` searches `PATH`, runs a file
  without an ELF header through bash (POSIX ENOEXEC rule) and maps `sh` to
  bash. Also `popen` and `sigaction`.
- **Time.** libc's wall clock and UTC calendar; `strptime` lives here.
- **Credentials.** Nothing: `get/set[res][ug]id`, `getgroups`/`setgroups`,
  `initgroups`, `fchown`/`fchmod`/`lchown`, `eaccess`, `getpass` and `crypt`
  are the SecOS libc's (kernel/cred.c, M42). The single-user stubs that
  claimed root are gone. `getlogin()` names the real uid. BusyBox's own
  libpwdgrp reads `/etc/passwd` and `/etc/group` for names.
- **System information.** `uname` is the kernel's with the node name from
  `/etc/hostname`; `getmntent` (from `/proc/mounts`), and C-locale
  wide-character functions for musl's regex.

## Installation and dispatch

`busybox_install()` in `kernel/kernel.c` runs at boot, right after the SecOS
coreutils are installed. It writes `/bin/busybox`, then installs the stub as
`/bin/<applet>` for each applet, replacing any same-named SecOS coreutils
program. The account files (`/etc/passwd`, `/etc/group`, `/etc/shadow`) come
from `kernel/sysroot.c`.

- **Why stubs, not symlinks or copies.** The ramfs root has no symlinks (on
  an ext2/ext4 root `execve` follows them to the signed image, so symlinks
  would work there). Each program is signature checked when it is loaded, so a
  link must resolve to the signed image. A full copy per applet would cost
  about 70 MB of RAM.
- **What the stub does.** Each stub is itself signed. It `execve`s
  `/bin/busybox` with the same `argv` and environment (in place: same pid,
  fds, signals), and BusyBox dispatches on `basename(argv[0])`.
- **Cost.** 121 stubs take 1.8 MB of ramfs, and each command costs one extra
  in-place exec (a signature check of the 565 KB image).

### Which program wins for each name

| Name | Program | Why |
|------|---------|-----|
| `bash`, `sh` | GNU bash | BusyBox is built without a shell, and `execvp("sh")` falls back to `/bin/bash` |
| `help`, `coreutils` | SecOS coreutils | BusyBox has no `help` |
| `cat echo wc head tail rev nl yes seq ls mkdir rm touch cp basename dirname stat true false uname sleep clear hexdump uptime` | BusyBox | Prefer BusyBox wherever it has the applet. `uname` prints `SecOS`; `uptime` works through `sysinfo()` |
| `poweroff reboot halt shutdown` | SecOS power commands | Not in the BusyBox config |
| `login su sudo passwd useradd userdel groupadd usermod id whoami groups` | SecOS account tools (`feat/multiuser-tools`) | The BusyBox applets are configured out, see below |
| `lua` | Lua 5.4.7 | |

The `cmdsubst`, `userland` and `BURST` selftest gates run against these
BusyBox applets. Example: `seq 1 50 | wc` must print exactly `50 50 141`.

### Applets configured out for the account tools

SecOS has its own account tools (one policy for login, su, sudo, password
changes and the account files), so `user/busybox.config` forces these BusyBox
applets off, and no `/bin` stub is installed for them:

| Applet | Was built before M42 | SecOS replacement |
|--------|----------------------|-------------------|
| `id` | yes | `id` |
| `whoami` | yes | `whoami` |
| `groups` | no | `groups` |
| `login` | no | `login` |
| `su` | no | `su` (and `sudo`) |
| `sulogin` | no | console login policy of init |
| `passwd`, `chpasswd`, `cryptpw`, `mkpasswd` | no | `passwd` |
| `adduser`, `deluser` | no | `useradd`, `userdel`, `usermod` |
| `addgroup`, `delgroup` | no | `groupadd` |
| `vlock`, `getty` | no | (none) |

## Applet status (QEMU, ramfs root, user-mode networking)

Legend: **works** = verified in QEMU; **partial** = runs, but some features
are limited (noted); **needs** = compiled, but the kernel feature it depends on
is missing.

| Applet | Status | Notes |
|--------|--------|-------|
| `[` `[[` `test` `true` `false` `expr` `printf` `echo` `seq` `yes` | works | `expr 6 \* 7`, `test -d`, `seq -s,` |
| `grep` `egrep` `fgrep` | works | `-c -o -i -E`, musl/TRE regex |
| `sed` | works | `-e`, `s///g`, `-i` (atomic rename) |
| `awk` | works | arithmetic, `printf %f`, `sqrt`, `length`, `substr` |
| `sort` `uniq` `cut` `tr` `head` `tail` `wc` `cat` `tac` `rev` `tee` `nl` `fold` `paste` `split` `comm` `expand` `unexpand` `tsort` `shuf` `factor` | works | `sort -n -k`, `uniq -c` |
| `xargs` | works | real `execve`: no argument-count limit (`seq 1 30 \| xargs -n 10 echo`) |
| `find` | works | `-name -type -print -exec` (real `execve`) |
| `ls` `cp` `mv` `rm` `mkdir` `rmdir` `touch` `stat` `du` `readlink` `realpath` `basename` `dirname` `pwd` `mktemp` | works | `cp -r`, `rm -r`, `mkdir -p`. `mv` is an atomic rename (also of directories) within one filesystem. Times are whole seconds. `ls -l` / `stat -c %U:%G` show the names from `/etc/passwd` and `/etc/group`; directories of any size list whole (up to 4096 entries per `SYS_GETDENTS`) |
| `ln` | partial | `ln` / `ln -s` work on ext2/ext4 (SYS_LINK, symlinks); the ramfs root supports neither (EPERM) |
| `chmod` `chown` | works | `chown user:group`, `-R`; the kernel's owner/root rules apply (`Operation not permitted` for a non-owner). On disk on ext2/ext4; kept on the in-memory inode on ramfs/devfs (`stat` shows it), not persisted on FAT |
| `truncate` | works | any size (SYS_TRUNCATE) |
| `sync` | works | SYS_SYNC (flushes the mounted filesystems) |
| `md5sum` `sha1sum` `sha256sum` `sha512sum` `cksum` `crc32` | works | match the host values |
| `od` `hexdump` `xxd` `strings` `base64` `cmp` `diff` `dd` | works | |
| `tar` `gzip` `gunzip` `zcat` `bzip2` `bunzip2` `bzcat` | works | create/extract/compress round trips in `/tmp` |
| `xz` `unxz` `xzcat` | works | decompression only (upstream BusyBox has no xz compressor) |
| `vi` | works | raw TTY: insert mode, ESC, `:wq` (selftest + scripted session) |
| `less` `more` | works | raw TTY, keys act at once, `q` quits |
| `clear` `reset` `tty` `stty` | works | `stty -a` shows the real termios and window size; `tty` names `/dev/console` or `/dev/pts/N` |
| `date` `cal` | works | RTC-anchored wall clock (UTC) |
| `sleep` `usleep` `time` `timeout` `watch` | works | `timeout` reaches the real program through the stubs (SIGTERM forwarding) |
| `hostname` `uname` `nproc` | works | hostname from `/etc/hostname` (default `secos`) |
| `env` `printenv` | works | the environment crosses `execve`; bash exports PATH/HOME (`/etc/bash.bashrc`) |
| `which` | works | |
| `free` `uptime` | works | from `sysinfo()`; real load averages |
| `kill` | works | |
| `mount` `umount` | partial | lists `/proc/mounts`; mounts through `SYS_MOUNT` (no flags, no remount/bind) |
| `df` | works | SYS_STATFS; the ramfs root reports data + free RAM like tmpfs |
| `ps` `top` `pidof` `killall` | works | `/proc/<pid>` (`ps aux`, `top -n1`, `pidof bash`, `killall -0`) |
| `dmesg` | works | kernel log ring (`klogctl`, `/dev/kmsg`) |
| `wget` | works | HTTP from a host server (`wget http://10.0.2.2:8099/...`); no HTTPS (TLS off) |
| `nslookup` | works | `nslookup example.com` through QEMU's DNS |
| `nc` `telnet` | works | real `poll()` readiness (`echo x \| nc -w 5 10.0.2.2 PORT` round trip); `telnet` uses the same path (not run) |
| `ping` | needs | raw ICMP sockets |
| `ifconfig` `ip` `netstat` | needs | `SIOCGIF*` ioctls, netlink and `/proc/net/*` |

## Kernel fixes made for this port

- **`fs/ramfs.c`.** Removing a file shifted the entry table, which re-pointed
  every open file behind it at its neighbour (gzip's output went into another
  file). Removal now leaves a reusable slot, and the VFS inode cache refreshes
  path and type when it hands out an inode.
- **`kernel/syscall.c`, spawn and cwd.** A spawned program now starts in its
  spawner's cwd, as `execve` keeps the cwd. Before, `cd /tmp; tar cf a.tar f1`
  ran `tar` in `/`.
- **`fs/ramfs.h` and `kernel/syscall.c`, limits.** Ramfs now holds 256 entries
  (was 96) and `SYS_GETDENTS` returns up to 256 names (was 64); since M42 it
  fills the caller's buffer, up to 4096 names.

## Multi-user rebuild (M42, feat/multiuser-ports)

Rebuilt against the libc with the real id syscalls: the compat layer no longer
fakes root, keeps the kernel's errno, and the account applets are configured
out (table above). `tools/mu_ports_test.sh` (selftest "MUPORTS") checks
`ls -l`/`ls -ln`/`stat` owner names, `chown user:group`, `chmod`, the
`ENOTDIR` and `EROFS` messages, `EACCES`/`EPERM` for a non-root user over SSH,
and `ls`/`find` over a 1000-entry directory.

## Phase 2 integration (2026-11, integ/phase2)

With real `execve`, the environment across exec, raw TTY/termios, ptys,
`/proc/<pid>`, `/dev/kmsg`, the wall clock, `poll`, `statfs`,
`rename`/`link`/`truncate`/`sync` and sockets as file descriptors, the port
layer now uses the kernel interfaces directly (see the table above for the
applets that moved from partial/needs to works).

## Still open

- `ping`: raw ICMP sockets; `ifconfig`/`ip`/`netstat`: `SIOCGIF*` ioctls,
  netlink and `/proc/net/*`.
- Symlinks and hard links on the ramfs root (`ln`, and applet symlinks instead
  of stubs).
- `alarm`/`setitimer` are accepted but never fire (no SIGALRM timer).
