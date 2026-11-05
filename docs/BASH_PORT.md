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

## Status (M39, current): bash is the boot login shell

> The sections above are the original pre-port plan, kept for history. Stages 1–4
> are done; stage 5 (polish) is the open work.

- **Built from source** (`2881cf4`, `make port-bash` / `tools/port-bash.sh`):
  140 objects cross-compiled against SecOS libc with `user/bash_port.h`,
  `user/bash_compat.c` (~60 shims) and `user/port-include/` stub headers;
  `--without-readline --enable-minimal-config`. Embedded signed as
  `crypto/user_bash_elf.h` (548 KB), demo `M39_BASH_DEMO`.
- **The fork/pipe crash under bash is FIXED** (`8762198`). It was not a COW bug but
  two scheduler bugs: eager zombie reaping lost the child's exit status (bash's
  `waitpid(pid)` got -1 → `abort()`), and there was no `waitpid(-1)`. Zombies now
  follow Unix semantics (persist until collected) and `SYS_WAITANY` (55) was
  added. Pipelines, subshells, functions, `case`, loops all work.
- **Interactive + boot into bash** (`e66a57f`): errno mapping in libc wrappers, a
  stable `st_ino`, cwd-relative `stat`/`lstat`, `SYS_FSTAT` (56). The signed bash
  is installed to `/bin/bash` at boot and launched `run /bin/bash -i`; the kernel
  shell is the fallback when bash exits. `-DSECOS_NO_BASH` drops it.

**Still open (stage 5 polish, tracked in `TASKS.md`):**
- real in-place `execve` (today emulated as spawn + wait + exit → pids change,
  job control approximate, env not inherited across exec);
- raw-mode termios honoured by `kernel/tty.c` (`TCSETS` sets `g_tty_canon`/
  `g_tty_echo`, but the console stays cooked) — prerequisite for readline;
- `poll`/`select` (disabled in bash's `config.h` today), wider `ioctl` coverage;
- readline / job-control polish; serial input drops the first char of a line in
  QEMU (PS/2 unaffected).

**Build hygiene (2026-11):**
- `tools/port-bash.sh` no longer compiles with `-w`: it uses
  `-Werror=implicit-function-declaration -Werror=int-conversion
  -Werror=int-to-pointer-cast`. An implicitly declared libc function returns
  `int`, so a pointer result is truncated to 32 bits — `strsignal()` crashed bash
  whenever a child exited with status >= 128 or died by a signal. Every shim bash
  calls is now prototyped in `user/bash_port.h`; a new gap fails the port.
- `<signal.h>` defines `NSIG 32` and the port trims the native `signames.h` to
  signals 0..31 + `DEBUG`/`ERR`/`RETURN`, so `trap … ERR|RETURN|DEBUG` work.
- Overrides: `BASH_PORT_WORK` (source tree), `BASH_PORT_OBJ` (object dir),
  `BASH_PORT_WFLAGS` (warning flags).

## Job control and the full feature set (2026-11, branch fix/lua-jobs)

bash was configured with `--enable-minimal-config`, which also turns off job
control, so `jobs`, `fg`, `bg`, `disown` and `suspend` did not exist. The
port now starts from the minimal config and turns on everything SecOS can back:
job control, aliases, `history`/`fc` (no `!` expansion), `help`, arrays, brace
expansion, `[[ ]]`, `(( ))`, `for ((;;))`, `pushd`/`popd`/`dirs`, `select`,
`time`, extglob and the case-modifying expansions. Process substitution stays
off (it needs `/dev/fd`). readline is on since the line-editing work below. The signed bash grows from 548 KB to 810 KB. A work dir
configured with different options is reconfigured automatically (`.secos-conf`
stamp).

Kernel/libc support used by job control:
- `tcgetpgrp`/`tcsetpgrp` drive the kernel `foreground_pgid` (TIOCGPGRP/
  TIOCSPGRP, also on a dup of the console fd); `getpgid` (SYS 57),
  `sigsuspend` (SYS 58), `kill(-pgid)`/`killpg`, and `waitpid(-1, WUNTRACED|
  WCONTINUED)` reporting stopped (`(sig<<8)|0x7f`) and continued (`0xffff`)
  children.
- A background reader of the console gets SIGTTIN, sent to its whole group.
- A blocking syscall interrupted by a stop restarts after SIGCONT instead of
  failing with EINTR.
- A spawned program stays in its spawner's process group, because `execve` is
  still spawn+wait+_exit, so `kill %1` and Ctrl-Z reach the real program.
- A console `read()` blocks the reader instead of spinning in ring 0. Before
  this, a background job made no progress while bash sat at its prompt.

Async Ctrl-Z/Ctrl-C need the PS/2 keyboard (the QEMU GTK window, VMware).
Serial input is polled, so the selftest uses `kill -STOP/-CONT %1` instead.
Still approximate: `$!` and `jobs -l` show the pid of bash's forked child
(the execve shim), not the program's own pid.

Rebuild: `make port-bash` reads the prebuilt `user/libsecos.o`/`libc.o`, so
run `make user-progs` first after a libc change. Signing runs from the repo
root, where pyenv's python has `cryptography`.

## Line editing: GNU readline on a real termios console (2026-11, feat/line-editing)

bash is built with its bundled GNU readline (`--enable-readline
--without-installed-readline`): arrow-key history, Left/Right/Home/End/Delete
editing, Ctrl-A/E/K/U/W/D, Tab completion of file and command names (double
Tab lists the candidates), Ctrl-R incremental search, `bind` and `/etc/inputrc`.
Progcomp (`complete`/`compgen`) stays off.

What made it possible:
- **Console termios** (`kernel/tty.c`, ABI in `kernel/tty.h` = libc
  `<termios.h>`, Linux flag values and `c_cc` indices): canonical or raw mode,
  ECHO*, ISIG (VINTR/VQUIT/VSUSP -> SIGINT/SIGQUIT/SIGTSTP to the foreground
  group), ICRNL/OPOST/ONLCR, VMIN/VTIME, TCSANOW/TCSADRAIN/TCSAFLUSH,
  TIOCGWINSZ (real console size)/TIOCSWINSZ, FIONREAD, TCFLSH. A lone ESC waits
  ~10 ms for the rest of a sequence so serial input is not split; readline has
  no `select()` here and decides "bare Escape or arrow key" with FIONREAD.
- **Keyboards send terminal bytes**: PS/2 and USB HID emit xterm sequences
  (arrows `ESC [ A..D`, Home/End `ESC [ H/F`, `ESC [ 2/3/5/6 ~`, F1-F12,
  Shift-Tab), CR for Enter, DEL for Backspace, Alt = ESC prefix.
- **ANSI console** (`drivers/vt.c`): cursor motion/addressing, erase,
  insert/delete, scroll regions, SGR colors, alternate screen, UTF-8 (one cell
  per code point; common punctuation folded to ASCII, box drawing mapped).
- **termcap**: SecOS has no termcap/terminfo database; `user/termcap.c`
  compiles in the console entry (`tgetent`/`tgetstr`/`tgoto`/`tputs`), returned
  for any `TERM` except `dumb`. `/etc/bash.bashrc` (written at boot when absent)
  exports `TERM=linux`; `/etc/inputrc` adds VT220/xterm Home/End/Delete variants
  and turns bracketed paste off (also off by default in the port's `rlconf.h`).

Port notes: `lib/sh/shtty.c` is compiled now (the old stubs never applied the
settings, so `read -s/-n` did nothing); `lib/readline` is compiled except the
helpers duplicating bash's (`xmalloc.c`, `xfree.c`, `shell.c`, `tilde.c`); the
readline SIGALRM timeout path has an upstream macro bug (`timerset(&x.y, ...)`)
that the port patches. `/bin/ttytest` and the selftest "line editing" section
drive all of it over serial; `tools/soak_drive.py` types some lines with edits.
