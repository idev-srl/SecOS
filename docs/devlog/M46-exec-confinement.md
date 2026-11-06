# M46 — Exec confinement (`no_new_privs`) + Phase 3 leftovers

Branch `feat/exec-confine`. Phase 4 hardening. Three leftovers from Phase 3
are fixed on the same branch: ACPI S5 power-off, `mount -o ro`/remount, and
user lines broken by kernel lines on the debugcon.

## 1. The problem

M35 confines a program whose signed manifest sets `CAP_ENFORCE` to the caps it
declares. Nothing bound what that program *started*: a confined process with
`CAP_PROC` could `spawn("/bin/bash")` or `execve("/bin/busybox")`, and the
child ran under its own (ambient) manifest. Every capability the parent was
denied was one exec away. The same hole let a confined process run a SETID
program (`sudo`, `su`) and get euid 0, and gain a Driver Space binding by
spawning a signed driver.

## 2. The rule (as implemented)

`process_t` gains a sticky `no_new_privs` bit (`kernel/process.h`).

- **Effective set** (`cap_effective()`, `kernel/cap.c`). A confined process
  has `cap_mask`. An ambient one has every implicit cap (`FS_READ`,
  `FS_WRITE`, `PROC`, `IPC`, `SIGNAL`, `TIME`, `POWER`) plus the explicit caps
  its own manifest declares (`NET`, `INPUT`, `DISPLAY`). Kernel context
  (no process, or pid 0) has all of them.
- **Who gets the bit.**
  - Every confined image gets it automatically when it is loaded.
  - Any process can set it with `prctl(PR_SET_NO_NEW_PRIVS, 1)`.
  - It cannot be cleared: `prctl(PR_SET_NO_NEW_PRIVS, 0)` returns `-EINVAL`.
  - `fork` copies it, and every image started under it keeps it.
- **New image under `no_new_privs`** (`cap_exec_transition()`). This covers a
  `SYS_SPAWN` child, where the bound is the spawner, and an `execve`, where
  the bound is the old image:

  ```
  effective(new) = effective(bound) & effective(new manifest)
  confined(new)  = confined(new manifest) || confined(bound)
  no_new_privs(new) = 1
  ```

  Each cap that the new manifest asked for but the bound withheld is audited
  as `[AUDIT] pid=… DENY <CAP> (spawn|execve)`. A `[CAPX]` line records the
  result.
- **Without `no_new_privs`** (an ambient program, init, the kernel shell, a
  session gateway), the new image gets its own signed manifest set, as
  before. Explicit caps then work like Linux file capabilities: ambient bash
  can still start `secwm`, which holds `CAP_INPUT|CAP_DISPLAY`.
- **`fork`** is a copy (`*c = *parent`): same caps, same confinement, same
  bit.
- **The emulated `execve`.** libc's `execve` is now the real in-place
  `SYS_EXECVE`, and the rule is applied in `process_exec`. The older
  spawn+wait+exit path goes through `SYS_SPAWN` and gets the same rule.
- **Kernel- and init-started processes** have no bound, so they start with
  their manifest's full set. This covers `/bin/init`, the kernel shell's
  `run`, the boot demos and every `process_create_from_elf*` call.

### `SYS_PRCTL` (90)

The syscall uses the Linux option numbers. Syscall numbers 90–94 had been
reserved for the GUI work, which never used them.

| option | meaning |
|---|---|
| `PR_SET_NO_NEW_PRIVS` (38), arg 1 | set the bit (any other arg: `-EINVAL`) |
| `PR_GET_NO_NEW_PRIVS` (39) | 0 / 1 |
| `PR_SECOS_GET_CAPS` (0x53430001) | effective caps (MANIFEST bits), plus bit 30 if confined and bit 29 if `no_new_privs` |

The BusyBox compat `prctl()` is still a stub. Nothing in the userland needs the
real call yet.

## 3. Decisions

**SETID vs confinement.** We follow Linux `no_new_privs`. Under the bit, the
S_ISUID/S_ISGID bits of the exec'd file are ignored, even for a program with
a SETID manifest on the root fs. The uid does not change and the caps do not
grow. The refusal is audited as `[AUDIT] … DENY SETID (setid-nnp)`. The check
uses the bit of the process that runs the exec, as it was *before* the new
image loaded, so a confined SETID program started by an ambient parent still
works. Uid changes a process may already make (root calling `setuid()`) are
not new privileges and are unaffected.

**Driver Space.** `execve` already refused a driver manifest (M11). A
`no_new_privs` spawner that spawns a signed driver now gets a plain user
process: the binding is dropped, the type is reset to USER and the event is
audited as `DENY DRIVER (spawn-nnp)`. This is the same outcome as "a bogus
claim degrades to USER".

**Session gateway (Dropbear).** `dropbearmulti` is confined (`0xFF0`), but its
job is to start authenticated sessions. Under the plain rule, every ssh
session would inherit Dropbear's set: no `CAP_POWER`, and no `sudo`/`su`
(SETID ignored). A new signed manifest flag, `MANIFEST_FLAG_SESSION_GATEWAY`
(bit 16), exempts a confined program from the *automatic* bit only:

- Dropbear's own syscalls stay confined.
- The session shell it starts runs under bash's own manifest, as before M46.
- If the bit is already set (inherited or voluntary), the flag has no effect.

The flag makes explicit something that was always true: a compromised login
server can start a full session. Only `user/note_dropbear.S` carries it
(`0x10FF0`), and `crypto/user_dropbear_elf.h` was regenerated with
`make port-dropbear`. The only change is the flag word and the signature.

**Ambient parents are not bounded by default.** Bounding them would break the
normal model (bash → secwm, bash → netcfg with `CAP_NET`), which treats
explicit caps as properties of the program. A process that wants the bound
asks for it with `prctl`.

## 4. Test gate (`M46_CONFINE_DEMO`, tools/selftest.sh "M46")

`user/m46_confine.c` is linked three times (`make m46-progs`):

- `m46conf`: confined, `CAP_ENFORCE|FS_READ|PROC|IPC|SIGNAL|TIME` = `0xF60`.
- `m46probe`: ambient, plus the explicit `NET|INPUT|DISPLAY` = `0x6010`.
- `m46setid`: the SETID manifest, installed root `04755`.

The kernel demo provisions `/bin`, installs the three programs and runs two
cases through the real `ksys_spawn_argv` path:

- **The ambient control** (`m46probe ctl`). The probe gets a socket, opens
  `/dev/input/event0` and writes `/tmp`. As uid 1000, `m46setid` gets euid 0
  through both spawn and execve. After a voluntary `prctl(PR_SET_NO_NEW_PRIVS)`,
  the SETID program keeps euid 1000.
- **The confined driver** (`m46conf`). Every way of starting the ambient probe
  gives it `caps=0xF40` and confinement, with no socket, no input device and
  no file write: spawn, fork, and fork + execve. BusyBox `touch` and bash
  `echo > file` both fail and the file stays absent. As uid 1000, the SETID
  program stays at euid 1000 through both spawn and execve. The bit cannot be
  cleared.

The gate has 27 checks, all passing. `/dev/fb0` does not exist yet when the
boot demos run (the framebuffer comes up later), so the probe uses
`/dev/input/event0`, which goes through the same explicit-cap gate
(`cap_check_explicit`).

## 5. Leftover: ACPI power-off

QEMU powered off correctly in both cases (MB2/i440fx and UEFI/q35, `pm1a=0x604`,
`SLP_TYPa=0`, which is QEMU's real `\_S5`). The only bug was a false failure
message. Under TCG the PM1 write is handled asynchronously, so the vCPU went
on to print `[POWER] ACPI poweroff unavailable; halting` while QEMU was
already shutting down. `acpi_poweroff()` (`arch/x86/acpi.c`) now:

- waits about 500 ms after each attempt before falling back or giving up;
- switches legacy firmware into ACPI mode first (`SMI_CMD <- ACPI_ENABLE`
  when `SCI_EN` is clear; real chipsets ignore `SLP_EN` otherwise);
- writes `SLP_TYP` and then `SLP_EN`, keeping the other PM1 bits;
- uses `X_PM1a/b_CNT_BLK` when the 32-bit FADT fields are 0;
- parses `\_S5` only as a `NameOp` target, with multi-byte `PkgLength` and
  Byte/Word/DWord constants.

Verified that QEMU exits by itself (rc 0, no "unavailable" line) on MB2
i440fx, MB2 q35, MB2 with the persistent ext4 root on virtio, and UEFI/OVMF
q35. The selftest UEFI power-off section gained the "no unavailable message"
check.

## 6. Leftover: `mount -o ro` and remount

- **FAT32 and ext2/ext4** honour `ro`, and also a device without a write path:
  - `VFS_FS_RDONLY` is set on the driver's ops, so `cred_inode_perm` returns
    EROFS before any DAC check.
  - Every mutating entry point returns `-EROFS`.
  - The lowest write helper (`sec_write` / `blk_write_direct`) refuses too.
  - `/proc/mounts` shows `ro`.
- **Remount.** `vfs_fs_ops_t` has a new optional `remount(int ro)` hook.
  `SYS_MOUNT` takes a `remount` option token (no device or fstype needed), and
  BusyBox's compat `mount()` now forwards `MS_REMOUNT` instead of returning
  EINVAL. `crypto/user_busybox_elf.h` was regenerated.
  - FAT32 and ext2/ext4 implement the hook.
  - exFAT and NTFS have no hook, so remount returns `-EINVAL`. exFAT `-o ro`
    at mount time already worked; NTFS is always read-only.
  - Going ro on ext2 retires the journal (`e2_sync`). Every transaction is
    committed when its operation returns, so nothing else is pending.
  - Files already open for writing get EROFS on their next write. There is no
    EBUSY check like Linux has.
- **The journal under `ro`: Linux semantics.**
  - A plain `ro` mount still replays a dirty journal. The committed
    transactions are part of the filesystem, and reading it without them shows
    a stale tree.
  - `noload` / `norecovery` (accepted only together with `ro`, as on Linux)
    leaves the journal alone. The kernel logs that the view may be stale and
    refuses `remount,rw` until the volume is mounted again normally.
  - Fixed on the way: at boot, the persistent-root probe (`ext2_mount_root`)
    replayed the journal of *every* ext data disk before it looked for
    `/.secosroot`. It now looks for the marker first, so a disk that is not
    the system root is never written. The marker is made with the disk and
    never rewritten, so the unreplayed tree has it.
- **Gate "RO"** (q35, 4 AHCI disks: vfat, a clean ext4, a dirty-journal ext4
  and an ext4 for remount) has 22 checks, all passing:
  - vfat: `ro`, EROFS on every change, remount rw then ro;
  - ext4 `ro`: mkdir, write, chmod and symlink all refused, and the image is
    byte-identical afterwards;
  - ext4 `remount,rw`: the write is on disk and e2fsck-clean, then
    `remount,ro` refuses writes again;
  - `noload` without `ro` returns EINVAL;
  - `ro,noload` shows the old content (the boot probe did not replay the
    journal either), and `remount,rw` is refused;
  - plain `ro` replays the journal and shows the new content, and the volume
    is e2fsck-clean.

## 7. Leftover: debugcon line interleaving

`[UA-TEST] ident-read-0x100000 OK` is built from several `write()` calls. A
`[SCHED] preempt` line printed between two of them landed mid-line, and the
check failed about one run in N. The console mirror in `ksys_write` now goes
through `debugcon_user_write()` (`kernel/klog.c`):

- bytes are held until `\n` (or a full 256-byte buffer) and emitted under the
  debugcon line lock, so kernel lines only appear *between* user lines;
- `sched_on_timer_tick` flushes an unterminated tail (a shell prompt) after
  200 ticks.

The selftest `declutter` helper is now redundant but harmless.

## 8. Open issues

- The BusyBox compat `prctl()` is a stub, so BusyBox applets cannot set the bit
  themselves. The kernel side is complete.
- `mem_limit` (manifest `max_mem`) is not bounded by the parent. Only
  capabilities, set-id and Driver Space are.
- exFAT and NTFS have no remount hook. Remounting ro does not refuse while
  files are open for writing.
- The ACPI path still has no HW-reduced ACPI support (`SLEEP_CONTROL_REG`) and
  no EFI `ResetSystem` fallback. Both need real-hardware validation.
