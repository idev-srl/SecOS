# /bin/sh and the sudo credential cache (`feat/sh-sudo-cache`)

Two usability changes the user asked for while testing: SecOS had no `sh` at
all (`sudo sh -c ...` gave "sh: command not found"), and sudo asked for the
password on every call. That was the M43 design. Linux sudo instead asks once
per terminal for 15 minutes.

## /bin/sh

- `kernel/sysroot.c`: `sh` is an alias of `bash` in the system-file table.
  On a disk root it is a relative symlink, `/bin/sh -> bash`, made like every
  other multi-call name. The upgrade boot of an existing disk creates it (the
  persist gate removes the link with debugfs before the upgrade boot and
  checks that it comes back, `linked=1`).
- On a ramfs root it is the BusyBox applet stub (`user/bbstub.c`). Started as
  `sh` or `-sh`, the stub now re-executes `/bin/bash` instead of
  `/bin/busybox`. A copy of bash would cost 1.4 MB of RAM. The stub alone was
  rebuilt (`crypto/user_bbstub_elf.h`, 14.7 KB) with the same commands as
  `tools/port-busybox.sh` steps 5–7. BusyBox itself did not change.
- bash checks `argv[0]`: under the name `sh` it runs in POSIX mode
  (`$SHELLOPTS` contains `posix`), and `-sh` is a login shell.
- The kernel's `execve` has no `#!` handling: it returns ENOEXEC. bash then
  runs the script in a bash subshell. So a `#!/bin/sh` script started from bash
  works, but it runs as bash and not in POSIX mode. `sh script` and `sh -c`
  do run in POSIX mode. For every script to get its interpreter, the kernel
  needs `#!` support in execve (a follow-up).
  BusyBox's `execvp` already had a fallback from `sh` to `/bin/bash`. It stays,
  and is now dead code. musl's `system()`/`popen()` use `/bin/sh` directly, so
  they work now.
- Dropbear lists `/bin/sh` in `COMPAT_USER_SHELLS`, so an account whose login
  shell is `/bin/sh` can log in. The MUPORTS gate now has one (`wuser`).

## sudo credential cache

### Behaviour

- After a successful password check, sudo does not ask again for
  `TS_TIMEOUT` = 15 min (a constant in `user/sudo.c`) **in the same terminal
  session**. Every successful sudo restarts the timer, as Linux sudo does.
- `-k` drops this session's record. `-k CMD` asks for the password and leaves
  the record alone. `-K` drops all of the user's records. `-v` authenticates
  (the cache counts) and refreshes the record.
- A wrong password never creates or refreshes a record. With no controlling
  terminal there is no record.
- Every reuse is logged to `/var/log/auth.log`:
  `carol : cached authentication reused ; TTY=/dev/console ; SID=42`. The
  usual command line follows it.
- su and login never use the cache.

### Where the record lives: the kernel, not `/run`

The brief allowed a root-only file under a RAM filesystem or a kernel-side
store. I chose the kernel (`kernel/sudo_ts.{c,h}`), because it is both simpler
and safer here:

- **No RAM filesystem.** SecOS has no tmpfs. On a persistent root, `/run`
  would be on the ext4 disk (it would survive reboots and need cleanup), and
  the ramfs driver is a single global instance that is the root on RAM boots.
- **The key comes from the kernel's view of the caller,** never from a file
  or an argument. User space can only name its controlling tty indirectly
  (ttyname of fd 0/1/2, `TIOCGSID` on the console), and stdin can be a
  different terminal. The kernel uses `p->ctty` itself.
- **Nothing on disk to protect,** so there are no owner/mode/symlink checks to
  get wrong. Because no file exists, the brief's rule that a record not owned
  by root or with the wrong mode is ignored has nothing to apply to.

The interface is one SecOS-specific prctl option, next to M46's
`PR_SECOS_GET_CAPS`: `prctl(PR_SECOS_SUDO_TS = 0x53430002, op, arg)`. It adds
no new syscall number.

| op | meaning | result |
|---|---|---|
| 1 CHECK | `arg` = timeout in seconds | 1 = a fresh record for the caller's key, else 0 (also with no ctty); an expired record is deleted |
| 2 UPDATE | create or refresh the caller's record | 0, `-ENOTTY` with no ctty |
| 3 INVALIDATE | `sudo -k` | 0 |
| 4 REMOVE_ALL | `sudo -K`: every record of the real uid | number of records removed |

Every op needs **euid 0** (else `-EPERM`). Only set-uid root sudo, or root,
can create, refresh or even probe a record. A user cannot find out whether
they have one.

**Key** = (real uid, controlling-tty id, session id, the session leader's
`ki.start_ns`):

- the tty id is the pty pool slot + 1 (`pty_ctty_id()` in `kernel/pty.c`,
  under `g_pty_lock`; a hung-up pty counts as none), or `PTY_CTTY_CONSOLE`;
- the session leader is looked up with `process_get_by_pid(sid)`. It must
  still lead that session and must not be a zombie. Otherwise there is no
  record.

**Time** is `kclock_mono_ns()`, so setting the date cannot extend a record.
A record whose time is in the future is treated as expired.

**Table**: 32 slots behind one leaf spinlock (`LK_S64_LEAF`, "sudo_ts"). The
lock is taken only after the key is computed, so `g_pty_lock` and
`proc_lock` are released by then. There are no user copies and nothing
blocks. When the table is full, the oldest record is evicted (that user just
gets a prompt). The table is empty at every boot.

The timeout comes from the caller, because `user/sudo.c` owns the policy. The
kernel clamps it to one year so the ns product cannot overflow. A test kernel
built with `-DSECOS_SUDO_TS_TEST_TIMEOUT=<s>` caps it further. This is a
build-time define for the selftest image only: users cannot set it at
runtime, and sudo itself is unchanged. I did not add an `/etc/sudo.conf`
override. Even root-owned, it is one more parser in a set-uid program for
little gain.

### Threats considered

| Threat | Outcome |
|---|---|
| Another terminal of the same user (2nd SSH login, secwm window, `script`) | different tty id + sid: prompt |
| A new login on a reused `/dev/pts/N`, or log out and log in again on the console | new session leader (new pid, new start time): prompt. The gate opens `script` twice on `/dev/pts/0` |
| Another user in the same terminal (`su erin`, then sudo) | different ruid: prompt |
| A detached process (`setsid`, daemon, ssh without `-t`) | no ctty: no record is used or created |
| stdin redirected from another terminal | irrelevant: the kernel uses the controlling tty, not fd 0 |
| Changing the clock | monotonic clock: no effect |
| Forging or extending a record | needs euid 0. sudo creates one only after `acct_check_password` succeeds |
| pid reuse for the session id | the leader's start time is in the key (SecOS pids also only increase) |
| Leader exits while children keep the session | no leader: no record (safe side) |
| Several CPUs | one leaf lock around the table. The key is a snapshot of the caller itself |

Same as Linux's `timestamp_type=tty`, and accepted: any process of the same
user in the same terminal session within the window (for example a malicious
script run from that shell) can use the record.

## Tests

- **Account tools gate** (`tools/selftest.sh`, image built with
  `-DSECOS_SUDO_TS_TEST_TIMEOUT=60`, guest timeout 210 → 450 s, new wheel user
  `erin`). The SC* steps check the following:
  - a second sudo in the same session does not prompt;
  - `sudo -k` → prompt;
  - three wrong passwords → no record (the next sudo prompts);
  - `script` (a new pty session) prompts, then has its own record;
  - the console record survives the other session;
  - a second `script` on the same `/dev/pts/0` prompts;
  - `su erin -c sudo` in the same terminal prompts for erin;
  - su still asks while a record is valid;
  - `sudo -v` refreshes without asking, and after 70 s the record has expired;
  - `sudo -K` → prompt;
  - every reuse is in auth.log.

  The SH* steps check `sudo sh -c` (as root, `$0` = sh), that a `#!/bin/sh`
  script runs, that `sh -c` runs in POSIX mode, `xargs sh -c`, `awk system()` and `find -exec sh -c`.
- **MUPORTS**: new `wuser` (wheel, login shell `/bin/sh`, key login). The
  SSH command runs through `/bin/sh` with an `sh -c` inside. sudo with no
  controlling terminal takes the password from stdin (NV1 = 0), and the next
  sudo with `</dev/null` fails (NV2 = 1, no record).
- **M41 persist**: boot 1 has `/bin/sh -> bash` on the disk. Before the
  upgrade boot the link is removed. Boot 3 recreates it (`linked=1`), and it
  runs.
- PERM and smoke `--mb2`/`--uefi` are unchanged and must still pass.
