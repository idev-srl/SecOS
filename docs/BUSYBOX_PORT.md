# BusyBox on SecOS

SecOS ships **BusyBox 1.36.1**, built from unmodified upstream source, as its
main Unix userland: one signed multi-call binary, `/bin/busybox`, plus a signed
stub under each of its 123 applet names in `/bin`. GNU bash stays the shell
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

Binary size: **566 KB** for busybox (470 KB of text at `-Os` with
`-mcmodel=large`) and 15 KB for the stub. This is well below the 1 MB
`SYS_SPAWN` load buffer, so embedding is fine and no loading from disk is
needed.

## Port layer (all MIT, outside `user/libc.c`)

| File | Role |
|------|------|
| `user/busybox_port.h` | Force-included. It gives `read/write/close/dup/dup2`, the path calls, `isatty`, `exec*` and `exit` new link names (asm labels, same prototype) so they reach the shims. After the SecOS headers are parsed, macros route the POSIX `socket/connect/.../mmap/mount/open/waitpid/ioctl/signal` calls to POSIX-shaped shims. It also renames BusyBox's own `spawn()`, which clashes with libsecos. |
| `user/busybox-include/` | Header overlay. Each header either `#include_next`s the SecOS libc header and adds the missing POSIX parts (types, errno, `O_*`, termios flags, signals/sigaction), or is a complete header (stdio, sockets, netdb, regex, poll, mntent, ...). |
| `user/busybox_compat.c` | The shims (about 2.4k lines). Generic libc material is marked `TODO(integration): move to libc`. |
| `user/bbstub.c` | The applet stub. |

What `busybox_compat.c` provides:

- **errno and paths.** Several libsecos wrappers return a bare -1. The kernel
  resolves relative paths only for `open/stat/lstat/chdir`, so every other path
  call is made absolute first. Also: real `rmdir`/`unlink`/`rename` semantics,
  `O_EXCL`/`O_DIRECTORY` emulation, `opendir` for directories with more than 64
  entries, and `realpath`.
- **Private stdio** (`bbio_*` link names). The libc FILE reads one byte per
  syscall and has no `getline`/`scanf`/`popen`. This one buffers reads and
  writes and has a printf engine with `%m %j %z %ll %hh` (floating point is
  delegated to libc) and an `sscanf`.
- **Sockets.** A POSIX API over the SecOS socket syscalls (AF_INET, TCP/UDP).
  Descriptors start at 64, can be `dup2`ed, and I/O is chunked to the kernel's
  2 KB bounce buffer. Also a small UDP DNS client
  (`/etc/hosts` and `/etc/resolv.conf` are honoured; QEMU's 10.0.2.3 is the
  default) behind `getaddrinfo`/`gethostbyname`.
- **Processes.** `waitpid` with `-1`/`WNOHANG` over `SYS_WAIT`/`SYS_WAITANY`.
  The exec emulation forwards termination signals to the spawned program.
  `execvp` honours `PATH` and falls back to bash for `sh`. Also `system`,
  `popen` and `sigaction`.
- **Time.** UTC `gmtime/mktime/strftime/strptime`.
- **System information.** `sysinfo` (from `/proc/meminfo`), `uname` (sysname
  `SecOS`), `sysconf`, `getmntent` (from `/proc/mounts`), and C-locale
  wide-character functions for musl's regex.

## Installation and dispatch

`busybox_install()` in `kernel/kernel.c` runs at boot, right after the SecOS
coreutils are installed. It writes `/bin/busybox`, then installs the stub as
`/bin/<applet>` for each applet, replacing any same-named SecOS coreutils
program. It also creates a root-only `/etc/passwd` and `/etc/group`.

- **Why stubs, not symlinks or copies.** The ramfs root has no symlinks, and
  `SYS_SPAWN` does not follow them on ext2/ext4. Each program is signature
  checked when it is loaded, so a link must resolve to the signed image. A full
  copy per applet would cost about 70 MB of RAM.
- **What the stub does.** Each stub is itself signed. It spawns
  `/bin/busybox` with the same `argv`, and BusyBox dispatches on
  `basename(argv[0])`. The stub then waits and exits with BusyBox's status,
  re-raising the signal if BusyBox was killed by one.
- **Cost.** 123 stubs take 1.8 MB of ramfs. Each command costs two spawns
  instead of one, until a real in-place `execve` lands. Then the stub becomes
  an `execve` of `/bin/busybox`, or the stubs can be replaced by symlinks.

### Which program wins for each name

| Name | Program | Why |
|------|---------|-----|
| `bash`, `sh` | GNU bash | BusyBox is built without a shell, and `execvp("sh")` falls back to `/bin/bash` |
| `help`, `coreutils` | SecOS coreutils | BusyBox has no `help` |
| `cat echo wc head tail rev nl yes seq ls mkdir rm touch cp basename dirname stat true false uname sleep clear hexdump uptime` | BusyBox | Prefer BusyBox wherever it has the applet. `uname` prints `SecOS`; `uptime` works through `sysinfo()` |
| `poweroff reboot halt shutdown` | SecOS power commands | Not in the BusyBox config |
| `lua` | Lua 5.4.7 | |

The `cmdsubst`, `userland` and `BURST` selftest gates run against these
BusyBox applets. Example: `seq 1 50 | wc` must print exactly `50 50 141`.

## Applet status (QEMU, ramfs root, user-mode networking)

Legend: **works** = verified in QEMU; **partial** = runs, but some features
are limited (noted); **needs** = compiled, but the kernel feature it depends on
is missing.

| Applet | Status | Notes |
|--------|--------|-------|
| `[` `[[` `test` `true` `false` `expr` `printf` `echo` `seq` `yes` | works | `expr 6 \* 7`, `test -d`, `seq -s,` |
| `grep` `egrep` `fgrep` | works | `-c -o -i -E`, musl/TRE regex |
| `sed` | works | `-e`, `s///g`, `-i` (the temp file is renamed by copy) |
| `awk` | works | arithmetic, `printf %f`, `sqrt`, `length`, `substr` |
| `sort` `uniq` `cut` `tr` `head` `tail` `wc` `cat` `tac` `rev` `tee` `nl` `fold` `paste` `split` `comm` `expand` `unexpand` `tsort` `shuf` `factor` | works | `sort -n -k`, `uniq -c` |
| `xargs` | partial | works; a command line is capped at 8 arguments of 127 bytes (`SYS_SPAWN` limit) |
| `find` | partial | `-name -type -print`; `-exec` hits the same 8-argument spawn limit |
| `ls` `cp` `mv` `rm` `mkdir` `rmdir` `touch` `stat` `du` `readlink` `realpath` `basename` `dirname` `pwd` `mktemp` | works | `cp -r`, `rm -r`, `mkdir -p`. `mv` of a directory is copy + remove (no rename syscall). Times are whole seconds |
| `ln` | needs | symlinks on the ramfs root and hard links (no link syscall) |
| `chmod` `chown` | partial | applied on ext2/ext4; accepted and ignored on ramfs/FAT (no metadata) |
| `truncate` | partial | only to size 0 (no truncate syscall) |
| `sync` | partial | succeeds; there is no sync syscall (the ext journal commits each operation) |
| `md5sum` `sha1sum` `sha256sum` `sha512sum` `cksum` `crc32` | works | match the host values |
| `od` `hexdump` `xxd` `strings` `base64` `cmp` `diff` `dd` | works | |
| `tar` `gzip` `gunzip` `zcat` `bzip2` `bunzip2` `bzcat` | works | create/extract/compress round trips in `/tmp` |
| `xz` `unxz` `xzcat` | works | decompression only (upstream BusyBox has no xz compressor) |
| `vi` | partial | starts, ex commands (`:s`, `:w`, `:q`) work; insert mode can't be left: the console TTY is cooked-only and drops ESC and other control bytes |
| `less` `more` | partial | page through a file and quit with `q`; the cooked TTY delivers keys only at Enter |
| `clear` `reset` `tty` `stty` | partial | `stty` shows and stores the flags; the TTY ignores raw mode |
| `date` `cal` | partial | `cal` is correct. `date` formats and parses, but the clock is uptime since 1970 (no wall clock yet) |
| `sleep` `usleep` `time` `timeout` `watch` | works | `timeout` reaches the real program through the stubs (SIGTERM forwarding) |
| `id` `whoami` `hostname` `uname` `nproc` | works | single user `root`; hostname from `/etc/hostname` (default `secos`) |
| `env` `printenv` | partial | run, but see no variables: `SYS_SPAWN` passes no environment |
| `which` | works | |
| `free` `uptime` | works | from `/proc/meminfo` and the tick count (no load average) |
| `kill` | works | |
| `mount` `umount` | partial | lists `/proc/mounts`; mounts through `SYS_MOUNT` (no flags, no remount/bind) |
| `df` | needs | `statfs` syscall |
| `ps` `top` `pidof` `killall` | needs | `/proc/<pid>` directories |
| `dmesg` | needs | kernel log device (`/proc/kmsg` or `/dev/kmsg`) |
| `wget` | works | HTTP from a host server (`wget http://10.0.2.2:8099/...`); no HTTPS (TLS off) |
| `nslookup` | works | `nslookup example.com` through QEMU's DNS |
| `nc` `telnet` | needs | `poll`/`select` readiness: the shim reports every socket as readable, so `nc` blocks on the network before it has sent stdin |
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
  (was 96) and `SYS_GETDENTS` returns up to 256 names (was 64).

## Still open (kernel features BusyBox would use)

The `TODO(integration)` markers in `busybox_compat.c` show where each one plugs
in:

- an in-place `execve` that carries `envp` and more than 8 arguments (`env`,
  `xargs`, `find -exec`, and dropping the stubs);
- `poll`/`select`, needed by `nc` and `telnet`, and by `vi`/`top` for timeouts;
- a raw-mode TTY that passes ESC and control bytes (`vi`, `less`, `top`);
- `statfs` (`df`, `stat -f`), `/proc/<pid>` (`ps`, `top`, `pidof`,
  `killall`), and a kernel log device (`dmesg`);
- `rename`, `link`, `truncate`, `sync` and a wall clock (`date`).
