# M40 — /bin/init as PID 1: a ready system at boot

**Goal:** boot the ISO and use it. Before this milestone, getting a usable
system meant five manual steps: leave bash, run `dhcp` in the kernel shell,
start bash again, type an `authorized_keys` line by hand, and start Dropbear.
Now the kernel starts `/bin/init` as PID 1, which takes a DHCP lease, supervises
the SSH server and keeps a bash on the console. A key baked at build time makes
`ssh` work straight away:

```
make iso SSH_AUTHORIZED_KEYS=~/.ssh/secos_ed25519.pub
qemu-system-x86_64 -cdrom myos.iso -boot d -m 512M -cpu max \
    -netdev user,id=n0,hostfwd=tcp::2222-:22 -device e1000,netdev=n0 \
    -serial stdio -display none -no-reboot
ssh -i ~/.ssh/secos_ed25519 -p 2222 root@localhost
```

The console shows:

```
Starting /bin/init (PID 1)...
[init] SecOS init (pid 1): 3 entries from /etc/inittab
ifup: eth0: requesting a DHCP lease (timeout 10 s)
ifup: eth0: lease 10.0.2.15/24 gw 10.0.2.2 dns 10.0.2.3
[init] started sshd (/bin/dropbearmulti) pid 3
[init] started console (/bin/bash) pid 4
bash-5.2#
```

## Boot flow

`m10_shell_idle_entry` (`kernel/kernel.c`, the kernel shell running as the
scheduler idle task) installs the userland as before, plus:

- `/bin/init` (kept in step with the embedded copy, so a persistent root
  populated by an older build does not keep the M32 demo init);
- `/etc/inittab` (only when absent, so a persistent root keeps its own);
- `/bin/{netcfg,ifup,ifdown,ifconfig}`;
- `/root/.ssh/authorized_keys` from the keys baked at build time, if any;
- `/var/log`.

Then `shell_exec_foreground("/bin/init")` (`kernel/shell.c`) loads and verifies
init, records its pid with `process_set_init_pid()`, puts it in its own
foreground process group and waits for it. A stop (SIGSTOP) is undone with
SIGCONT.

Fallbacks, each with a console message and a `[INIT]` debugcon line:

| Situation | Behaviour |
|---|---|
| `/bin/init` missing | the bash login shell, then the kernel shell (the M39 boot) |
| `/bin/init` fails its signature check | same (`-DSECOS_INIT_TAMPER_TEST` exercises it) |
| PID 1 exits or is killed | the kernel logs `[INIT] PID 1 (/bin/init) exited, status=...`, SIGKILLs every user process left (console bash, services), and the kernel shell takes the console |
| built with `-DSECOS_NO_INIT` | the M39 boot, unchanged |

PID 1 is not protected from `kill -9 1`; that is how the selftest checks the
death path.

## `/etc/inittab`

```
# <id>:<action>:<command> [args...]
net:sysinit:/bin/ifup -t 10
sshd:respawn:/bin/dropbearmulti dropbear -F -R -p 22
console:console:/bin/bash -i
```

- **id**: up to 15 characters. It names the entry in messages and the service
  log `/var/log/<id>.log`.
- **action**:
  - `sysinit`: run at boot in file order and wait for it (killed after 60 s).
    Output goes to the console.
  - `respawn`: run in its own session (`setsid`, no controlling tty), stdin
    `/dev/null`, stdout and stderr appended to `/var/log/<id>.log`. Restarted
    when it exits.
  - `console`: respawned on the console as a session leader, with the console
    as controlling tty (`TIOCSCTTY`) and its process group in the foreground
    (`tcsetpgrp`).
  - `once`: started at boot like `respawn`, never restarted.
  - `shutdown`: run on the console when the system goes down, and waited for
    (10 s).
- **command**: an absolute path and its arguments, split on blanks. There is no
  shell and no quoting. Children get `PATH=/bin:/usr/bin HOME=/root TERM=linux
  SHELL=/bin/bash USER=root`, default signal dispositions and an empty mask.

At most 16 entries, 192 bytes per command. A malformed line is reported and
skipped. With no usable inittab, init runs `console:console:/bin/bash -i`.

**Why this format.** It is the sysvinit/BusyBox shape minus runlevels, so it
reads familiar, and it parses in a few dozen lines of C with no shell. Service
dependencies are expressed by order: `sysinit` entries finish before any
service starts.

**Backoff.** A service that ran for at least 10 s restarts at once. One that
died sooner waits 1 s, then 2, 4, 8, 16 and 30 s. At 30 s init says once that
it keeps failing and stops repeating the message. With no NIC, Dropbear cannot
listen and settles at one retry every 30 s.

**Orphans.** When a process exits, the kernel hands its children to init
(`process_reparent_children()` in `kernel/process.c`, called from both exit
paths in `kernel/sched.c`) and wakes init's wait if one of them is already a
zombie. init reaps everything with `waitpid(-1)` (`SYS_WAITANY`) and logs
`reaped orphan pid N`. With no init running, nothing changes: the reaper frees
orphans as before.

## Shutdown

`/bin/poweroff`, `/bin/reboot` and `/bin/halt` (`user/power.c`) check that
`/proc/1/comm` is `init` and signal it: SIGTERM powers off, SIGUSR2 reboots,
SIGUSR1 halts. init then:

1. runs the `shutdown` entries;
2. sends SIGTERM and SIGHUP (an interactive bash ignores SIGTERM) plus SIGCONT
   to each service's process group;
3. waits up to 3 s, reaping, then SIGKILLs what is left;
4. calls `sync()` and `SYS_REBOOT`, which terminates anything else, syncs and
   unmounts again, and powers off.

`poweroff -f`, a system without init, or an init that has not finished within
30 s go straight to `SYS_REBOOT`, as before. init ignores SIGINT, SIGQUIT,
SIGTSTP, SIGTTIN, SIGTTOU, SIGHUP and SIGPIPE.

## Console session and job control

The console entry forks, and the child calls `setsid()`, `ioctl(0, TIOCSCTTY)`
and `tcsetpgrp(0, getpid())` before `execve("/bin/bash", ...)`. The console's
foreground group is the kernel's `foreground_pgid`, so PS/2 Ctrl-C and Ctrl-Z
reach bash's jobs as before. Checked in QEMU with `sendkey ctrl-c` and
`sendkey ctrl-z`: `sleep 20` is interrupted, `sleep 30` stops, and `jobs`/`fg`
work. Over the serial console, Ctrl-C still does not interrupt a running job.
That is a pre-existing limitation, identical with `-DSECOS_NO_INIT`: serial
input is polled, not delivered from an interrupt. `exit` (or `exec cmd`) on the
console makes init start a new bash.

Services run in their own sessions with no controlling terminal, so console
signals never reach them.

## Network bring-up from userspace: `SYS_NETCTL` (95)

Two options were considered:

1. **A userspace DHCP client** over the socket layer. It needs to send from
   0.0.0.0 to 255.255.255.255:67 before the interface has an address, and to
   receive the broadcast reply. The socket layer has no `SO_BROADCAST`, no bind
   to an unconfigured interface and no raw sockets. Adding them would duplicate
   `net/dhcp.c` and widen the attack surface of the socket API.
2. **Expose the existing kernel client** (chosen). The RFC 2131 exchange already
   works and is validated on VMware.

The kernel client used to spin on `hlt` inside the caller, which is fine for the
kernel shell but would pin a CPU in ring 0 for up to 8 s inside a syscall. It is
now an asynchronous state machine (`dhcp_start`/`dhcp_poll`, `net/dhcp.c`): the
UDP callback only records OFFER/ACK/NAK, and `dhcp_poll()` sends REQUEST,
retries (4 attempts, 1 s per step) or gives up and restores the previous
address. The kernel shell's `dhcp` spins on it as before.

`SYS_NETCTL` (`net/netctl.c`, ABI in `net/netctl.h`) is a non-blocking control
call:

| op | |
|---|---|
| `NETCTL_COUNT` | number of NICs |
| `NETCTL_INFO idx, *ifinfo` | name, MAC, ip/mask/gateway/dns, flags (UP, LINK, DHCP lease), IRQ mode |
| `NETCTL_SET idx, *ifinfo` | static ip/mask/gateway/dns (`-EBUSY` during DHCP) |
| `NETCTL_DHCP_START idx` | send DISCOVER; `-EBUSY` while another exchange runs |
| `NETCTL_DHCP_POLL idx` | 0 running, 1 bound, `-ETIMEDOUT` |

It needs **CAP_NET** in the caller's signed manifest, like the socket calls.
User pointers go through `copy_from_user`/`copy_to_user`. An exchange whose
caller was killed is taken over by the next `NETCTL_DHCP_START` after 2 s
instead of staying busy forever. Syscalls 90–94 are left to the GUI work.

`user/netcfg.c` is one signed multi-call program with a CAP_NET manifest
(`user/note_net.S`), installed under four names:

```
ifup [-t SECONDS] [IFACE]    # DHCP; exits 1 with no NIC / no lease (boot goes on)
ifdown [IFACE]
ifconfig [IFACE]             # also: netcfg
ifconfig IFACE ADDR [netmask M] [gw G] [dns D]
```

```
eth0      Link encap:Ethernet  HWaddr 52:54:00:12:34:56
          inet 10.0.2.15/24  Mask:255.255.255.0  Gateway:10.0.2.2  DNS:10.0.2.3
          UP RUNNING dhcp  rx:polled
```

BusyBox `ifconfig` fails (`SIOCGIFCONF: Inappropriate ioctl`, no
`/proc/net/dev`) and BusyBox `ip` needs netlink, so netcfg replaces
`/bin/ifconfig`. `ifup` polls every 50 ms and gives up at its timeout (default
10 s; the kernel client itself gives up after about 4 s without an OFFER). With
no NIC it exits at once. Either way init goes on to start the services.

## sshd and the host key

init runs `dropbearmulti dropbear -F -R -p 22` as the `sshd` service. `-R`
creates `/etc/dropbear/dropbear_ed25519_host_key` on the first connection. On
the ramfs root that happens every boot (a new host identity each time). On a
persistent ext2/ext4 root (`make sysdisk-ext4`, root adopted through
`/.secosroot`) the file survives, and the second boot presents the same key.
This was checked with `StrictHostKeyChecking=yes` across two boots of an ext4
sysdisk, and the volume stayed `e2fsck`-clean. Dropbear's log is
`/var/log/sshd.log`.

## Baking the authorized key

```
make iso SSH_AUTHORIZED_KEYS="~/.ssh/a.pub ~/.ssh/b.pub"
```

`tools/gen-authorized-keys.py` turns the files into `build/authorized_keys.h`.
`build/` is git-ignored, so a key is never committed. The script rewrites the
header only when the key set changes, so an unchanged key set does not rebuild
the kernel. It refuses anything that looks like a private key or is not a
public-key line. The header is compiled into `kernel/kernel.c`, so the same
`kernel.bin` carries the keys on both boot paths: MB2/ISO and UEFI
(`make uefi` copies `kernel.bin` to `dist/kernel.elf`). At boot the kernel
creates `/root/.ssh/authorized_keys`, or appends the baked block to an existing
file (persistent root) when it is missing. It also sets `/root` and
`/root/.ssh` to root:root 0700 and the file to 0600. Dropbear refuses keys
under a home directory that root does not own, and a sysdisk staged with
`mkfs.ext4 -d` carries the build user's uid (1000).

Without the variable the header is empty and no file is created: Dropbear
refuses every login (`Permission denied (publickey)`), as before. Keys can still
be installed by hand.

The public key is not secret. Baking it into a signed kernel binds "who may log
in" to the build, which fits signature-as-trust. Rebuilding the kernel is the
way to change it on a ramfs root.

## Bug fixed on the way: ext2/ext4 VFS inode slots were recycled under open fds

On the persistent ext4 root, the first SSH login failed with `Login attempt for
nonexistent user`: `/etc/passwd` contained Dropbear's log lines. `fs/ext2.c`
kept a fixed pool of 32 VFS inodes and, once full, handed its **last slot** to
every further file. The VFS has no inode refcount, and an open fd keeps its
`vfs_inode_t*`. So the sshd log fd, opened when the pool was already full, was
silently re-pointed at `/etc/passwd` by Dropbear's next lookup, and its writes
went there. Every persistent-root boot fills the pool, because the userland
installer looks up hundreds of files. The pool now grows in kmalloc'd chunks of
64, with one slot per on-disk inode that is never reused while mounted (up to
32768 distinct inodes per mount). A remount reuses the chunks.

## Testing

`tools/selftest.sh`, **INIT gate** (end of the file; host port 23000–23999, never
2222):

- (a) Image built with a throwaway key generated in the test. Checks: init is
  PID 1 (`[INIT] ... (PID 1)`, `ps`, `/proc/1/cmdline`); the default inittab is
  read; the baked key is installed; `ifup` took a lease; sshd is supervised;
  `ssh ... 'uname -a; ps; ...; ifconfig'` works at once (rc 0, `dhcp` shown);
  the console bash answers over serial and respawns after `exit`; an orphan
  (`bash -c "sleep 1 &"`) is reaped by init; `poweroff` goes through init and
  QEMU exits by itself (rc 0).
- (b) NIC on a socket netdev with no DHCP server: `ifup` times out and the
  console works; `kill -9 1` is logged and the kernel shell takes over.
- (c) `-DSECOS_INIT_TAMPER_TEST`: the tampered init is refused and the bash
  fallback works.

Gates that assumed `exit` drops to the kernel shell were adapted. The SSH gate
no longer runs `exit`/`dhcp`/`run /bin/bash`/`dropbear &` by hand, and reads the
host-key message from `/var/log/sshd.log`. The execve gate now expects init to
respawn the console after `exec /bin/echo`. The heap-debug gate reaches the
kernel shell's `heapcheck` with `kill -9 1`.

## Known gaps

- No runlevels, dependencies or readiness checks beyond file order. `respawn`
  services start right after the `sysinit` steps.
- No per-service user, capability or resource settings. Every service runs
  under its own signed manifest only.
- PID 1 can be killed with SIGKILL. The kernel then falls back to its shell
  instead of panicking.
- Serial-console Ctrl-C does not interrupt a running job (pre-existing; PS/2
  works).
- `ifup` handles one interface (the first, or a named one). There is no DHCP
  lease renewal, so a lease that expires is not renewed.
- On ramfs the SSH host key changes every boot. Use a persistent root for a
  stable identity.
