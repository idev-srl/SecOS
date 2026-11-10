# Users and logging in

SecOS is a multi-user system (M43, `docs/devlog/M43-multiuser.md`). Two
separate rules apply:

- the **code signature** decides *which programs* may run at all;
- the **user** (uid/gid and file modes) decides *what a running program may
  touch*.

Root (uid 0) is exempt from file permissions. Everyone else gets the usual
Unix owner/group/other checks.

## Logging in for the first time

By default init runs `/bin/login` on the console. A fresh system has no
password: root's entry in `/etc/shadow` is `!`, which means locked. You have
three ways in:

1. **Bake a root password into the build** (best for VMs and test images):

       make iso ROOT_PASSWORD='choose-one'
       make iso ROOT_PASSWORD_HASH="$(openssl passwd -6)"   # or pass a ready hash

   The password is hashed on the build host with the same musl SHA-512 crypt
   that the system uses, and only the hash goes into the image. At boot the
   kernel gives root this hash, but only while root's password is still unset
   (`!`). A password set on the system later always wins. The hash is cached in
   `build/root_pwhash.h` (ignored by git), and the salt only changes when the
   password changes. A password typed on the `make` command line ends up in
   your shell history, so use `ROOT_PASSWORD_HASH` if that matters.
2. **First-boot setup on the console.** If no account has a password yet,
   `login` first asks for a root password, then offers to create a regular
   user. That user gets a home under `/home/<name>` and joins `wheel` (sudo),
   `video` and `input` (the desktop). Press Enter to skip either step.
3. **SSH with a baked key** (`make iso SSH_AUTHORIZED_KEYS=~/.ssh/id.pub`). This
   logs you in as root and needs no password.

A failed login costs 3 seconds and is logged. After three failures `login`
exits, and init starts a new one.

### Development builds: `CONSOLE=root`

`make iso CONSOLE=root` (`-DSECOS_CONSOLE_ROOT`) brings back the old console:
a root bash with no login. `tools/selftest.sh` builds this way. The setting
goes into the default `/etc/inittab` and into `secos.console=` on
`/proc/cmdline`. On a persistent root an *unmodified* default inittab follows
the build's setting. Once you edit `/etc/inittab` it is yours, so change the
`console:` line by hand (`/bin/login` or `/bin/bash -i`).

## Managing accounts (as root)

    useradd alice                    # uid/gid from 1000 up, group "alice", home /home/alice (0700, from /etc/skel), shell /bin/bash
    passwd alice                     # set a password (the account stays locked until you do)
    usermod -aG wheel,video,input alice
    usermod -s /bin/bash -d /home/a2 -m alice
    usermod -L alice / usermod -U alice    # lock / unlock
    userdel -r alice                 # also removes the home (only if alice owns it)
    groupadd devs; groupdel devs
    printf 'alice:secret\n' | chpasswd     # non-interactive

Other `useradd` options: `-M` (no home), `-r` (system account, ids 100-999,
no home), `-u UID`, `-g GROUP`, `-G G1,G2`, `-d HOME`, `-s SHELL`,
`-c COMMENT`, `-p HASH`.

Every change takes the lock file `/etc/.pwd.lock`. The tool writes the new
file next to the old one with its final owner and mode, fsyncs it, and
renames it into place. A crash leaves either the old file or the new one,
never a mix, and `/etc/shadow` is never readable by anyone but root, not even
briefly. `userdel` refuses an account that still has processes running
(`-f` overrides).

Anyone can run these:

    id [-u|-g|-G] [-n] [-r] [USER]
    whoami
    groups [USER]
    passwd              # your own password: asks for the current one, then the new one twice (6 characters or more)

## su and sudo

- `su [-] [-c CMD] [USER]` asks for the **target's** password (root is never
  asked). `su -` gives a login shell in the target's home with a clean
  environment.
- `sudo [-u USER] [-i|-s] CMD...` has one fixed rule. There is no sudoers
  file: **members of `wheel` may run any command as any user, after typing
  their own password.** Root is never asked. Everyone else is refused and is
  not asked for a password. `sudo -l` shows the rule. The command runs with
  a clean environment (`TERM` kept, a fixed `PATH`, and
  `SUDO_USER`/`SUDO_UID`/`SUDO_GID`/`SUDO_COMMAND` set).
- **Credential cache.** After you type your password, sudo does not ask again
  for **15 minutes in the same terminal session**. Each successful sudo
  restarts the 15 minutes. The cache is per user and per terminal session:
  another terminal (another SSH login, another secwm window, `script`),
  another user in the same terminal (`su`), or a new login on a reused
  `/dev/pts/N` or on the console always gets the prompt. A program with no
  controlling terminal (for example `ssh host 'sudo ...'` without `-t`) gets
  no cache at all. A wrong password never creates or extends the cache.
  - `sudo -k` forgets this session's cache. `sudo -k CMD` asks for the
    password and leaves the cache as it was.
  - `sudo -K` forgets all of your caches (every terminal).
  - `sudo -v` asks only if needed and restarts the 15 minutes.

  The kernel keeps the cache (not a file): a small table, readable and
  writable only by a set-uid root program, keyed by your real uid, the
  controlling terminal, the session id and the start time of the session's
  leader. It uses the monotonic clock, so changing the date does not extend
  it, and it is gone at reboot. su and login never use it.
- **`/bin/sh`** is GNU bash. Started as `sh`, bash runs in POSIX mode, so
  `#!/bin/sh` scripts, `sh -c`, `sudo sh -c '...'`, `system()` and `popen()`
  work. On a disk root `/bin/sh` is a symlink to `bash`.

`login`, `su`, `sudo` and `passwd` are the only set-uid programs. They are
installed root `04755`, and the kernel honours the bit only because their
**signed** manifest carries `MANIFEST_FLAG_SETID`. A `chmod u+s` on any other
program does nothing, and a modified copy of one of the four no longer passes
the signature check, so it will not run at all.

Passwords are hashed only with SHA-512 crypt (`$6$`, the vendored musl crypt)
and compared in constant time. They are never echoed, and the buffers that
held them are wiped. Unknown users and locked accounts take the same time to
reject as a wrong password.

## The log

`/var/log/auth.log` (root `0600`) records logins, failed logins, every su,
every sudo attempt with its command (and a `cached authentication reused` line
when the cache answered instead of a password), password changes and account
changes.

## Logging in, the terminal, logging out

`login` gives the terminal to the user who logged in (owner = user, group
`tty`, mode `0620`). It then sets the supplementary groups, the gid and the
uid, in that order, and checks them. It sets `HOME`, `USER`, `LOGNAME`,
`SHELL` and `PATH`, changes to the home directory and starts the shell from
`/etc/passwd` as a login shell, so `/etc/profile` and `~/.profile` are read.
When the session ends, init gives the console back to root before it starts
the next `login`.

## The desktop (secwm)

`startgui` runs as the user who starts it. Opening `/dev/fb0` (root:video
0660) and `/dev/input/eventN` (root:input 0660) requires **both** the signed
capability (`CAP_DISPLAY`/`CAP_INPUT`, which only secwm has) and membership in
`video` and `input`. To use the desktop:

    usermod -aG video,input alice

Then alice logs in on the console and runs `startgui`. The terminal windows
run alice's own shell as alice. A user logged in over SSH cannot take over
the screen, because the console belongs to whoever logged in on it.

**GUI at boot** (`make GUI_AUTOSTART=1`, or `1` in `/etc/gui-autostart`):
init starts the desktop as root, and it immediately switches to the user
named by a `user=NAME` line in `/etc/gui-autostart`. The first-boot setup
writes that line for the user it creates. With no `user=` line, the desktop
does not start, except on a `CONSOLE=root` build.

## SSH per user

Not done yet. Dropbear still logs everyone in as root, using
`/root/.ssh/authorized_keys`. A separate branch (`feat/multiuser-ports`) is
rebuilding Dropbear for multi-user mode, with `~user/.ssh/authorized_keys`
and password login. Until it lands, give users access through the console
and `su`/`sudo`.
