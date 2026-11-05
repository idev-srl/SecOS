# Pseudo-terminals, sessions and controlling terminals

SecOS has Unix 98 pseudo-terminals: `/dev/ptmx` hands out a master, `/dev/pts/N`
is its slave. They are what an SSH server (Dropbear), `script`, `screen`/`tmux`
or a graphical terminal needs to run an interactive program as if it sat on a
real terminal.

## Model

- **One line discipline for every terminal.** `kernel/tty.c` keeps all termios
  state in a `struct tty` (`kernel/tty.h`). The console is one instance (input
  from the keyboard/serial ring, output to the VT); every pty slave is another
  (input pushed by master writes, output into a ring the master reads). ICANON,
  ECHO/ECHOE/ECHOK/ECHOCTL/ECHOKE/ECHONL, ISIG (VINTR/VQUIT/VSUSP), VERASE,
  VKILL, VWERASE, VREPRINT, VLNEXT, VEOF/VEOL/VEOL2, VMIN/VTIME, ICRNL/INLCR/
  IGNCR/ISTRIP/IUTF8, OPOST/ONLCR, NOFLSH and TOSTOP behave the same on both.
- **Master write → slave input** through the ldisc (echo goes back to the
  master). **Slave write → master read** with OPOST/ONLCR applied. The slave's
  output ring is 4 KB; its input queue 4 KB plus a 1 KB canonical line.
- **Blocking** follows pipes: a read/write that cannot progress blocks the
  caller (woken by the other side) or returns `EAGAIN` with `O_NONBLOCK`
  (set at `open()` or with `ioctl(fd, FIONBIO, &one)`; `fcntl(F_SETFL)` also
  works once the kernel `fcntl` from the execve branch is merged — it stores
  `O_NONBLOCK` in the same `fds[].flags` bit). Writes are short when the
  other side is full.
- **Hangup.** The last master close hangs the slave up: SIGHUP + SIGCONT to
  its foreground process group and its session leader; slave reads return
  what is left, then `EIO`; slave writes `EIO`. The last slave close makes
  master reads return `EIO` once the output is drained (Linux behaviour: an
  SSH server reads the master until `EIO`).
- **Lifetime.** Every fd that holds an end (open, dup, dup2, fork, spawn
  stdio inheritance) and every process that has the pty as controlling
  terminal holds a reference; the pty is freed, and its number reused, when
  the last one goes. At most 64 ptys exist at once.

## Sessions and the controlling terminal

- `setsid()` (SYS 85) starts a session with no controlling terminal; `EPERM`
  for a process-group leader (POSIX). `getsid(pid)` is SYS 86.
- A session leader without a controlling terminal acquires a free pty slave
  it opens without `O_NOCTTY`, or with `ioctl(fd, TIOCSCTTY, 0)` (`1` steals
  it from another session). The terminal's foreground group becomes the
  leader's group.
- `/dev/tty` opens the caller's controlling terminal: its pty slave, the
  console when it never detached, `ENXIO` after `setsid()` with none.
- `TIOCNOTTY` gives the terminal up; a session leader doing so (or exiting)
  hangs its session up (SIGHUP + SIGCONT to the foreground group).
- fork and spawn (and therefore the libc `execve` emulation) inherit the
  session and the controlling terminal.
- Job control on a pty that is the caller's controlling terminal: a
  background reader gets SIGTTIN (`EIO` when blocked/ignored); with TOSTOP a
  background writer gets SIGTTOU; `TCSETS*`/`TCFLSH`/`TCXONC`/`TIOCSPGRP` from
  the background raise SIGTTOU unless it is blocked or ignored.

## ioctls on a pty fd

| request | side | meaning |
|---|---|---|
| `TIOCGPTN` (0x80045430) | master | slave number N (`unsigned *`) |
| `TIOCSPTLCK` (0x40045431) | master | lock (≠0) / unlock (0) the slave (`int *`) |
| `TIOCGPTLCK` (0x80045439) | master | lock state |
| `TCGETS`/`TCSETS`/`TCSETSW`/`TCSETSF` | both | the slave's termios |
| `TIOCGWINSZ`/`TIOCSWINSZ` | both | window size; a change sends SIGWINCH to the foreground group |
| `FIONREAD` | both | master: output waiting; slave: input readable |
| `TIOCOUTQ` | slave | output not yet read by the master |
| `TCFLSH`, `TCSBRK`, `TCXONC` | both | as on the console |
| `TIOCGPGRP` | master, or slave that is the caller's ctty | foreground group |
| `TIOCSPGRP` | slave that is the caller's ctty | set it (group must be in the session) |
| `TIOCGSID` | both | session it controls (`ENOTTY` if none) |
| `TIOCSCTTY` / `TIOCNOTTY` | slave | acquire / give up as controlling terminal |
| `FIONBIO` | both | non-blocking I/O on this fd |

Unknown requests return `EINVAL`. There is no packet mode (`TIOCPKT`) and no
`TIOCGPTPEER`.

## libc

`<stdlib.h>`: `posix_openpt`, `grantpt` (no ownership to change: single user),
`unlockpt`, `ptsname`, `ptsname_r`. `<pty.h>`: `openpty`, `forkpty`. `<utmp.h>`:
`login_tty`. `<unistd.h>`: `ttyname`, `ttyname_r`, `setsid`, `getsid`.
`<termios.h>`: the request numbers above.

```c
int m;
struct winsize ws = { rows, cols, 0, 0 };
int pid = forkpty(&m, NULL, NULL, &ws);   /* child: new session, slave = ctty + 0/1/2 */
if (pid == 0) { execv("/bin/bash", (char*[]){ "bash", "-i", 0 }); _exit(127); }
int one = 1; ioctl(m, FIONBIO, &one);
/* relay: read(m) -> network, network -> write(m); read(m) == -1/EIO: the shell is gone */
ioctl(m, TIOCSWINSZ, &new_ws);            /* window-change request -> SIGWINCH */
```

## For poll/select

`pty_poll(inode, events)` (`kernel/pty.h`) returns the `POLLIN`/`POLLOUT`/
`POLLHUP`/`POLLERR` bits that hold for a pty end (`tty_console_poll()` for the
console). A poller that has to wait sets `current->wait_pty = PTY_WAIT_ANY` and
blocks: every pty state change (data in or out, room freed, close, hangup) and
the periodic tty tick (every 8 ms) wake it to re-check.

## Tests

- `/bin/ptytest` (`user/ptytest.c`): PT-OPEN/SESSION/CANON/SIGINT/WINCH/RAW/HUP/
  EOF/CYCLE/BASH/IBASH, then `PT-DONE 11/11` (`ptytest fast` skips the bash cases).
- `/bin/script`: the same binary relays the console to an interactive bash on
  a new pty, like script(1) without the typescript file.
- `/bin/tty` (coreutils) prints the terminal on stdin.
- `tools/selftest.sh` "Pseudo-terminals" gate (log `/tmp/secos_selftest_pty.log`),
  and `user/sysfuzz.c` fuzzes pty ends, terminal ioctls and setsid/getsid.

## Limitations

- A wakeup that races a blocking caller between "nothing to do" and the block
  is picked up by the periodic tty tick, so it costs up to ~8 ms.
- The console stays outside the session model's checks: it is the implicit
  controlling terminal of every session that never detached, and its job
  control keeps the global foreground group (`TIOCSPGRP` on the console).
- One lock serialises all ptys; no packet mode, no `TIOCSTI`, no per-pty
  `IXON` flow control (`VSTART`/`VSTOP` are stored, not acted on).
