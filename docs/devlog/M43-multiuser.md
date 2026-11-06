# M43 — Multi-user: credentials, permissions, login and account tools

Phase 3, item 2 (`TASKS.md`). User guide: `docs/USERS.md`.

M43 is numbered after M41 (persistent root, exFAT) and M42 (NTFS). The kernel
credential work was written earlier on `feat/multiuser`, and its code comments
still carry the tag `[M42]`. Read those `[M42]` tags in `kernel/cred.*`,
`fs/*` and `kernel/syscall.c` as this milestone.

## Revising M26

M26 said that POSIX metadata is **stored and exposed, not enforced**, and that
the signature is the only trust boundary. **That no longer holds: it is
enforced now.** There are two independent gates:

| gate | decides | where |
|---|---|---|
| code signature (M9) | *which code* may run | `mm/elf_sign.c` |
| credentials + modes (M43) | *who* the running code acts for | `kernel/cred.c` |

A signed program run by alice can do what alice may do.

## Kernel (branch `feat/multiuser-base`)

- `struct proc_cred`: real, effective and saved uid and gid, plus 32
  supplementary groups. fork copies them; spawn and exec inherit them.
- Syscalls 100–109: `getuid`/`geteuid`/`getgid`/`getegid` (with a getres*
  form), `setuid`, `setgid`, `setresuid`, `setresgid`, `getgroups`,
  `setgroups`. They follow POSIX rules. Root may set any id. Other users may
  only switch among their own real, effective and saved ids. `setgroups` is
  root only.
- VFS checks: search permission on every directory walked; read, write and
  exec on files. Exec needs an x bit even for root. Creating needs w+x on the
  parent, as does deleting, plus the sticky-bit rule in `/tmp`. Only the
  owner (or root) may chmod or chown, and only root may give a file away.
  New files get euid and egid, or the directory's gid when the directory is
  set-gid.
- Signals: root, or the sender's real or effective uid must equal the
  target's real or saved uid.
- `/proc/<pid>` private entries are visible to root, or to a caller whose ids
  all match the target's.
- devfs node modes: the console is root 0600 until a login hands it out; ptys
  are owner:tty 0620; block devices root:disk 0660; fb0 root:video 0660;
  input root:input 0660 (gid 105).
- **Set-id exec is rooted in the signature.** A set-user-id or set-group-id
  bit is honoured only when the file is on the root filesystem **and** its
  signed manifest carries `MANIFEST_FLAG_SETID` (bit 15). Otherwise the bit is
  ignored, and the kernel logs `[CRED] set-id bit ignored`.

## User space (this branch, `feat/multiuser-tools`)

- `user/acct.{c,h}`: the shared code.
  - Database edits: take the lock `/etc/.pwd.lock` with `O_CREAT|O_EXCL` (a
    stale lock left by a dead process is broken); write `<file>+` with the
    final owner and mode; fsync; rename.
  - Hashing uses only `secos_make_salt` + `crypt_r` (`$6$`). Checking uses
    only `secos_verify_password`. Unknown users and locked accounts are
    checked against a dummy hash so they take the same time.
  - Password prompts read `/dev/tty` with echo off. A signal restores the
    terminal. Buffers are wiped.
  - `acct_become`: initgroups, then setresgid, then setresuid, then a check
    that the ids changed and that `setuid(0)` now fails.
  - `/var/log/auth.log` is root 0600.
- `login`, `su`, `sudo`, `passwd`: signed with `user/note_setid.S`
  (flags 0x8000) and installed root 04755 by `kernel/sysroot.c`.
  - Failure delays: login 3 s, the others 2 s.
  - sudo's policy is fixed: wheel members, with their own password, no
    cache.
  - passwd: a user can change only their own password (decided by the real
    uid), must give the old one, and the new one needs at least 6 characters.
    `-d` locks the account rather than leaving it without a password.
- `usertools` (plain manifest): `useradd`, `userdel`, `usermod`
  (`-aG/-G/-g/-s/-d/-m/-c/-L/-U`), `groupadd`, `groupdel`, `chpasswd`, `id`,
  `whoami`, `groups`. It is installed after BusyBox, so its `id` and `whoami`
  replace BusyBox's. useradd defaults: `/home/NAME` 0700 filled from
  `/etc/skel`; `/bin/bash`; a personal group; ids from 1000; password locked
  until `passwd`.
- `kernel/sysroot.c` adds:
  - `/etc/skel/{.profile,.bashrc}` and `/etc/profile`;
  - the `[USERS]` boot marker;
  - the console policy in the default inittab, `INITTAB_TEXT(console_line)`:
    `/bin/login`, or `/bin/bash -i` with `CONSOLE=root`. An inittab that is
    still byte-identical to either default follows the running build;
  - `ROOT_PASSWORD`: `tools/gen-root-pwhash.sh` builds `tools/pwhash_host`
    (the same musl code, compiled for the host) and writes
    `build/root_pwhash.h`. The kernel applies that hash only while root's
    shadow field is exactly `!`.
- init (`user/init.c`): before each console session it calls
  `fchown(0, 0, 0)` and `fchmod(0, 0600)`, so the previous user loses the
  console. With no inittab at all, it falls back to `/bin/login`, or to bash
  when `secos.console=root` is on the command line.
- secwm:
  - terminals run the user's shell from `/etc/passwd`;
  - `--auto` started by root first switches to the user named by `user=NAME`
    in `/etc/gui-autostart`, and hands the console to that user;
  - with no such user it refuses to start, except on a `CONSOLE=root` build;
  - device access needs the `video` and `input` groups as well as the
    capabilities. The first-boot user is added to both. Other users need
    `usermod -aG video,input`. Remote (SSH) users cannot grab the screen.
- sysfuzz now draws syscalls 100–109 too. After every batch, a run started
  without root checks that it did not end up with uid, euid or suid 0
  (`[sysfuzz] ESCALATION`).

## Login and first boot

`/bin/login` on the console prompts `host login:` and then `Password:`. After
three failures it exits and init respawns it.

**First boot** happens when root is still `!` and no account holds a hash. On
the console, as root, login asks for a root password and offers to create a
wheel+video+input user. With `GUI_AUTOSTART` it also writes `user=` for the
desktop. `login -f USER` (root only) skips authentication.

## Tests

- `tools/selftest.sh` exports `CONSOLE=root`, so existing gates still drive a
  root bash.
- New gate **"Account tools"** (15 checks): a `CONSOLE=login` image with
  `ROOT_PASSWORD`, covering:
  - a wrong and then the right console login;
  - useradd, usermod and chpasswd;
  - home and set-uid modes;
  - a user login that takes over the console;
  - su with the right and a wrong password;
  - sudo as a wheel member, and refusal outside wheel;
  - passwd followed by a login with the new password;
  - no password echoed;
  - sysfuzz as an unprivileged user with no escalation, and the system still
    alive afterwards.
- `bx.sh` fix: the bash-exec gate now marks its script executable, since exec
  needs an x bit.

## Open

- A dedicated hostile multi-user gate (a user probing every boundary, with
  exact errnos) is not written yet. The kernel rules are listed above; the
  gate is a follow-up.
- Per-user SSH: Dropbear multi-user mode (`feat/multiuser-ports`).
- bash still shows `root@` in `\u` for other users, because of the bash port's
  uid stubs (`feat/multiuser-ports`).
- If init is missing or unsigned, the kernel still falls back to a root bash
  on the console, which is a recovery path with no authentication.
