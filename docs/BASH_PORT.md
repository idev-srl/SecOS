# Porting GNU bash to SecOS — status & plan (M39)

Goal: compile **GNU bash from source** (and sign it) for SecOS, the way lua 5.4.7
was ported in M34. This document records the concrete, evidence-based state after
the M39 "POSIX shell-from-source foundation" landed, and the staged path to a
running `bash`.

## TL;DR

The M39 foundation closes the **largest** gaps (cwd, fd duplication, terminal
ioctl/termios, environment, exec). bash 5.2 is a **272-`.c`-file autoconf project**
with deep POSIX assumptions; the remaining work is a **hand-curated `config.h`** plus
~15–20 more small libc shims plus bash's own bundled libraries — a multi-session
effort, not a single build. This is the honest scope; nothing here fakes a working
bash.

## What M39 delivered (validated in ring 3 — `M39_POSIX_DEMO`)

Syscalls 49–54 + libc:
- **cwd**: per-process `cwd` in `process_t` (fork-inherited); `chdir`/`getcwd`;
  `ksys_open` resolves relative paths against cwd (`.`/`..`/`//` normalized).
- **fd duplication**: `dup`/`dup2` with pipe refcounting — `dup2(pipe_w, 1)` (the
  shell redirection primitive) verified end-to-end.
- **terminal**: `ioctl` with `TCGETS`/`TCSETS` (cooked-TTY model), `TIOCGWINSZ`,
  `TIOCG/SPGRP`; libc `<termios.h>` (`tcgetattr`/`tcsetattr`/`cfmakeraw`/
  `tcget|setpgrp`).
- **environment**: real `environ` + `getenv`/`setenv`/`unsetenv`/`putenv`.
- **exec**: `execve`/`execv`/`execvp` (emulated `spawn`+`waitpid`+`_exit`).
- **identity/misc**: `getppid`, `getuid`/`geteuid`/`getgid`/`getegid` (→0, single-
  user = signature is the trust boundary), `setuid`/`setgid`/`umask`/`getgroups`,
  `gettimeofday` (uptime), `fcntl` (`F_DUPFD`/`F_GETFL`/`F_SETFL`), `sigsetjmp`/
  `siglongjmp` (alias `setjmp`).

## What bash 5.2 still needs (from a source audit of its 272 `.c` files)

Functions bash references that SecOS does NOT yet provide (counts = files using):
- **passwd/group db**: `getpwnam`/`getpwuid` (1), `getgrgid` — needs a static
  "root" `struct passwd`/`group` (single-user). Easy shim.
- **resource/limits/time**: `times` (9), `getrlimit`/`setrlimit`, `sysconf` — return
  sane constants / `RLIM_INFINITY`. Easy shims.
- **I/O multiplexing**: `select` (4)/`poll` — bash uses these for input readiness;
  a stub that reports "ready" (we're line-at-a-time) is enough to start.
- **locale/wide-char**: `wcwidth`/`mbrtowc` (1)/`mbsrtowcs` — a byte==char,
  width==1 stub (no locale) suffices for ASCII.
- **bundled libs** (bash ships its own — just compile them in): `lib/glob`
  (`glob`/`fnmatch`), `lib/sh`, `lib/tilde`, `lib/malloc` (or use SecOS sbrk
  malloc), and **`--without-bash-malloc --without-readline`** to drop the two
  biggest sub-builds initially.

Already satisfied by SecOS now: `fork`, `execve`, `waitpid`, `pipe`, `dup2`,
`getcwd`/`chdir`, `ioctl`, `tcsetattr`, `tcget/setpgrp`, `sigaction`(`signal`),
`sigprocmask`, `kill`, `getenv`/`setenv`, `umask`, `getuid…`, `gettimeofday`,
`fcntl`, `sigsetjmp`.

## The real blocker: `config.h`

bash's `configure` runs **compiled test programs on the build host** to decide
hundreds of `HAVE_*` macros — it cannot probe SecOS (there is no `x86_64-secos`
cross toolchain; SecOS reuses host gcc with `-ffreestanding -nostdlib` + its own
libc, exactly like the lua port). So a SecOS bash needs a **hand-written
`config.h`** asserting the subset SecOS provides and `#undef`-ing the rest — the
same pattern as `user/lua_port.h`, but far larger. Authoring and iterating that
config against link errors is the bulk of the remaining effort.

## Staged plan

1. **Scaffold**: `third_party/bash/` (vendored source) + `user/bash_port.h`
   (force-included) + a hand-written `config.h` seeded from `config.h.in`.
2. **Shim layer** (`user/bash_compat.c`): the ~15 stubs above (passwd/group,
   times/sysconf/getrlimit, select/poll, wcwidth/mbrtowc).
3. **Build subset**: `configure … --without-readline --without-bash-malloc
   --disable-nls --enable-minimal-config`; compile core + `lib/{sh,glob,tilde}`;
   resolve missing symbols iteratively.
4. **Sign + run**: `secos-sign` the ELF, run ring-3 from the VFS like lua; start
   with `bash -c 'echo hi; for i in 1 2 3; do echo $i; done'` (non-interactive),
   then interactive once termios cooked/raw is fully wired in `kernel/tty.c`.
5. **Polish**: in-place `execve` (replace `spawn`+wait emulation) for correct pids
   and job control; wire `g_tty_canon`/`g_tty_echo` into `kernel/tty.c` so raw
   mode actually changes console behavior.

## Why not finished this session

bash is ~10× the size and POSIX-surface of lua. The foundation (the part that
needed kernel work) is done and validated; the rest is a large but mechanical
libc-completion + config-authoring grind best done iteratively against the bash
build. The M39 foundation is independently valuable: it is what **any** non-trivial
source port (dash, busybox, coreutils-from-upstream, …) needs first.
