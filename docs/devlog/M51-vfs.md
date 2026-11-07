# M51 C1 — VFS locks: namespace lock, unmount draining, atomic O_APPEND

Phase C, step C1 of the BKL removal (`docs/BKL_REMOVAL_PLAN.md` §4, branch
`feat/vfs-locks`, base 42a04cb). The BKL stays on and the default boot
behaves as before. This step makes the VFS layer itself safe for the day
syscalls run concurrently. It also defines the contract that the FS drivers
(C2 ramfs/devfs/procfs/sysfs, C3 ext2/ext4, C4 fat32/exfat/ntfs) wire their
per-instance K30 klocks into.

## Locks

| Level | Lock | Kind | Covers |
|---|---|---|---|
| K20 | `vfs_ns_lock` (`fs/vfs.c`) | klock | namespace changes that do FS I/O: the mount syscall (the driver's probe and `vfs_mount`, or a remount), the umount (drain, busy check, sync, detach), `vfs_unmount_all` |
| K25 | `vfs_append_lock` (`fs/vfs.c`, **new level** `LK_K25_VFS_APPEND`) | klock | the O_APPEND fallback ("current end of file, then write there") and the truncates that move the end, for an FS without `->append` |
| S20 | `g_mount_lock` (unchanged role) | spinlock irqsave | the mount table, plus the per-slot ref counts and draining flags. It is never held across a filesystem call. |

The nesting is K20 → K25 → K30 (FS) → K40 (block) → S20 … S40 (page cache).
`g_mount_lock` can be taken while an FS lock is held, because drivers call
`vfs_mount_point_of()` from inside their ops.

## Mount slots, refs and draining

`g_mounts[]` stays dense and still shifts on unmount. Each mount, the root
included, now owns a **stable slot id** (`vfs_mount_t.sid`) for its
lifetime. The slot holds:
- the mount-point string (the old `g_mp_store`; strings handed out stay
  valid until the slot is reused);
- `g_sid_active[sid]`, the number of VFS operations in flight on the mount;
- `g_sid_draining[sid]`, which is set while an unmount is in progress.

`vfs_resolve()` now runs under `g_mount_lock`. It used to read the table
unlocked and return a pointer into the array, which an unmount on another
CPU could shift. It returns a snapshot (`vfs_mref_t`: ops, mount point,
fs name, sid) and **takes a ref** on the slot. Every path-based entry point
holds that ref while it is inside the filesystem and drops it with
`vfs_mput()`:
- lookup;
- readdir;
- create, mkdir, remove, rmdir;
- rename, link;
- truncate, setattr;
- readlink, symlink;
- statfs, `vfs_statfs_ops`;
- `vfs_sync_all`;
- `vfs_read_all` and `vfs_write`, which keep the ref across the read or
  write that follows the lookup.

A resolution that lands on a draining mount fails (NULL / -1), as if
nothing were there.

`vfs_unmount_ex(mp, busy_cb, ctx, flags)` runs under `vfs_ns_lock`. It takes
the lock itself when the caller does not hold it. The steps are:
1. Under `g_mount_lock`, find the mount and set it draining.
2. Spin until its refs reach 0. The waiter uses `cpu_relax_poll` and keeps
   answering TLB shootdowns. After 5 s it logs
   `[VFS] umount: in-flight operations did not drain` and returns -EBUSY.
3. Take and release `ops->lock`, if the driver set one. This is a barrier
   for calls that reached the driver directly through an open file's
   `ino->ops`.
4. If this is the FS's last mount and `busy_cb(ops, ctx)` returns non-zero,
   return -EBUSY and leave the mount as it was. This is the open-fd and
   file-mapping check, now asked *after* the drain.
5. With `VFS_UMOUNT_SYNC`, call `ops->sync`.
6. Detach and invalidate the page cache.

`vfs_unmount()` is `vfs_unmount_ex(mp, NULL, NULL, 0)`: it drains, does no
busy check and does not sync (the boot, test and shell callers keep their
old behaviour). `vfs_replace_root` drains the old root before swapping it.
`vfs_unmount_all` (shutdown) unmounts the mounts one at a time, deepest
first, each one drained.

`ksys_umount` (`kernel/syscall.c`) now calls
`vfs_unmount_ex(abs, umount_is_busy, NULL, VFS_UMOUNT_SYNC)` under
`vfs_ns_lock`. The busy check (`process_foreach` over fds and VMAs) is the
same as before.

`ksys_mount_opts` is now a wrapper that holds `vfs_ns_lock` around the old
body (`ksys_mount_opts_locked`). That covers the driver's
"already mounted?" check, its probe I/O, `vfs_mount`, and `ksys_remount`.

### Why `vfs_mount*` do not take `vfs_ns_lock`

Drivers call `vfs_mount` and `vfs_mount_root` from inside their mount
routine. Once C3 and C4 land, that routine holds the driver's K30 lock, and
taking K20 there would invert the order. The **caller** of the driver's
mount routine takes the ns lock instead. Today that is the mount syscall.

The boot mounts (`kernel/kernel.c`) and the kernel shell's
`mountdev`/`umount` (`kernel/shell.c`) still call the drivers without it.
They are single-threaded today; see phase D below.

## Atomic O_APPEND

The old code in `ksys_write` looked up the size, then wrote. Two appenders
could pick the same end of file, and a CPU could run between the lookup and
the write.

```c
int vfs_append(vfs_inode_t* ino, const void* buf, size_t len, uint64_t* at);
```

- With **`ops->append`**, the driver resolves the end of file and writes
  there in one critical section under its own instance lock. It returns
  the bytes written and sets `*at`.
- **Without it (the default today, every FS)**, the VFS takes
  `vfs_append_lock`, refreshes the size through `ops->lookup(ino->path)`
  (`ino->path` is mount-relative), writes at that offset and unlocks.

`ksys_write` (append branch only) sets the fd offset to `*at + r`.
`vfs_truncate` and the new `vfs_ftruncate` (used by kiface `ftruncate`,
which also updates `ino->size`) take `vfs_append_lock` when the FS has no
`->append`, so a truncate cannot slip in between an appender's size read
and its write.

This deviates from the plan. The plan wrote `write(ino, VFS_OFF_APPEND, …)`
returning the offset. `->write` returns a byte count, and a driver that has
not been taught the sentinel would write at offset 2^64-1. A separate
optional op with an out-parameter is explicit and safe by default.

**fd table and offsets.** `c->fds[]`, including the offset, belongs to one
process. A process runs on one CPU at a time and has no threads, and fork
copies the table rather than sharing it. These fields need no lock until
threads exist; then a per-file `struct file` with its own lock is needed.

## Contract for the FS drivers (C2–C4) — what the integrator wires

New optional fields at the end of `vfs_fs_ops_t` (`fs/vfs.h`). Every
initializer is designated, so existing drivers compile unchanged:

```c
int (*append)(vfs_inode_t* inode, const void* buf, size_t len, uint64_t* at);
struct klock* lock;    /* the instance's K30 klock (LK_K30_FS) */
```

- **`lock`:** set it to the driver's instance klock. NTFS has per-instance
  ops copies, so each copy gets its instance's lock. The VFS **never holds
  it while calling an op**: every exported op takes it itself. The VFS
  uses it only as the unmount barrier (lock and unlock once after the
  drain). Unset (NULL) is fine; draining by refs still works.
- **`append`:** implement it as
  `lock(); end = <own size>; r = write_locked(ino, end, …); *at = end; unlock();`.
  The FS then appends with its own lock instead of the global K25
  fallback. Unset means the fallback, which is correct but serializes
  every appender of every FS without `->append`, system-wide. When a
  driver sets `append`, the VFS stops taking K25 around its truncates.
  The driver's own lock must then cover truncate against append.
- A driver's mount routine **may** call `vfs_mount*` while holding its
  K30 lock. It must **not** call `vfs_unmount*`, which can take K20.
- **Readdir callbacks are called with no FS lock held** (rule in
  `fs/vfs.h` on `readdir`, agreed with C2). The driver collects the
  entries under its lock and emits them after it unlocks. A callback may
  block (the kernel shell's `vls`/`ls` page and wait for a key) or call
  back into the VFS. The VFS keeps only the mount ref across the listing.
- **`mntinfo` must be lock-free**: no klock, no blocking, stable strings.
  procfs `gen_mounts` calls it under `g_proc_lock` (a spinlock), and
  `vfs_dev_for` calls it during mount.
- **`setattr` fallback.** For an FS without `->setattr` (procfs and sysfs
  today; ramfs and devfs have their own since C2), `vfs_setattr` looks the
  inode up first, then writes mode/uid/gid/times under `ops->lock` when it
  is set.
- **ramfs (C2) integration.** C2 handles appends inside
  `ramfs_vfs_write_locked` with an offset sentinel
  (`offset == VFS_OFF_APPEND → e->size`). This branch does not define
  `VFS_OFF_APPEND`, because `->write` cannot return the offset the data
  landed at. The integrator wires ramfs with:
  - `.lock = <the ramfs klock>`;
  - `.append = ramfs_vfs_append`, written as
    `lock; off = e->size; r = ramfs_vfs_write_locked(ino, off, buf, len); *at = off; unlock`
    (or by passing the sentinel and reading `e->size` before the write,
    still under the lock).

  ramfs is only the root and is never unmounted, so draining is a no-op
  for it.
- **Never clear `ino->ops`** on a shared inode in lookup or readdir. ramfs
  did this (`ino->ops = NULL` in `ramfs_vfs_lookup` and in its readdir).
  The VFS restamps the field after every lookup, but an fd on another CPU
  reading `ino->ops->write` in that window called NULL. The forced test hit
  this as a #PF on a user address (CR2 ≈ 0), which then tried to take the
  BKL under `vfs_append_lock` and was reported by lockdep as
  `bkl vs vfs_append`. It is fixed here in `fs/ramfs_vfs.c` (two lines,
  marked `[M51 C1]`). C2 should keep it that way. `vfs_append` reads
  `ino->ops` once and restamps it after its own lookup.

## `vfs_walk` and mount crossings

`vfs_walk_checked` resolves one component at a time through `vfs_lookup`
and `vfs_readlink`. Each call is a complete VFS operation: resolve, take a
ref, call the driver op (which takes and releases its own lock), drop the
ref. The VFS holds no FS lock between components, so a walk that crosses
from `/` into `/mnt`, or follows a symlink onto another mount, never holds
two K30 locks. The search callback (`kernel/cred.c`) only reads inode
fields. The only nesting the VFS creates is K25 → K30 inside `vfs_append`
and `vfs_truncate`, and that is a single FS.

## Tests

### `-DSECOS_VFS_TEST` (`fs/vfs_test.c`)

The BSP runs it from `kernel_main` after the boot mounts. The APs run it
from their idle loop (hook in `kernel/smp.c`, call in `kernel/kernel.c`),
outside the BKL. The drivers have no locks on this branch, so each phase
keeps a driver to one CPU at a time except through the lock under test.
Output is `[VFST] …` lines ending in `RESULT PASS|FAIL`.

1. **Concurrent O_APPEND, ramfs root.** 4 CPUs each append 300 64-byte
   records (1–3 records per call) to one file through `vfs_append`.
   Expected: exact size, every record well formed (no torn or overlapping
   data), every (cpu, seq) present once and in order. Result: 1200
   records, 0 problems.
2. **Negative control.** Same appends, but "size" and "write" in two
   separate critical sections (the pre-M51 pattern). Result: about 450 of
   1200 records survive and the checker reports more than 120 problems,
   so phase 1 is able to fail.
3. **Concurrent O_APPEND on /mnt.** FAT32 and ext2 runs, 4×60 records.
   Result: 240 records, 0 problems on both.
4. **Mount/umount race.** The BSP runs 40× `vfs_ns_lock` +
   `vfs_unmount_ex(drain, sync)` + the driver's mount routine; every 4th
   unmount is refused by its busy callback. One AP reads `/mnt`: a readdir
   whose callback spins 2 ms inside the FS with a ref held, and
   `vfs_read_all` of a 4 KiB pattern file. Pass criteria: every read either
   returns the exact data or fails cleanly, and no callback runs inside the
   driver after the unmount returned. Results:
   - FAT32: 30 umount, 10 EBUSY, 30 mount, 0 wrong reads, 0
     inside-detached; drains that had to wait for the reader > 0 (checked).
   - ext2: same counts, 40 drains waited.
5. **Negative control for 4.** The drain disabled through the test hook
   `g_vfs_test_nodrain`: the reader is caught inside the detached FS
   (inside-detached = 1).

Each run: lockdep build, `-smp 4`, ROOT=ram plus a virtio FAT32 or ext2
disk. Both runs: `RESULT PASS`, no `[EXC]`, no `[LOCKDEP]`, no
`[LOCK] BUG`, and the boot continues to the root bash.

### Gates and builds

- Gates M26, M41 (+ MNT) and M42 were run from `tools/selftest.sh` in a
  private /tmp; results are in the report.
- `-Wall -Wextra`: 41 warnings in the normal and the `-DSECOS_LOCKDEP`
  builds (the baseline).
- The `tools/{exfat,ntfs}_fuzz` harnesses still build: `vfs.h` only gained
  a `struct klock` forward declaration.

## What phase D must know

- **ns lock callers.** `kernel/kernel.c` (boot `/mnt`, `/dev` and `/proc`
  mounts, the M10 `vfs_unmount("/mnt")`) and `kernel/shell.c`
  (`mountdev`, `umount`) call driver mount routines without
  `vfs_ns_lock`. Wrap them in `vfs_ns_lock()`/`vfs_ns_unlock()` when the
  shell can run concurrently with syscalls. `vfs_unmount` already takes
  the lock when the caller does not hold it.
- **open vs umount.** The open path does a lookup (with a ref) and installs
  the fd later, without the ref. An open that has finished its lookup but
  not installed its fd can therefore slip past the umount busy check. For
  D3 there are two options:
  - hold a mount ref from the lookup to the fd install (export a
    `vfs_lookup_pin`/`vfs_unpin` pair built on `vfs_lookup_ref`/`vfs_mput`);
  - or do the open under `vfs_ns_lock` (cheap: K20 is uncontended outside
    mounts, but it serializes opens).

  The first is preferred.
- **Direct `ino->ops` calls.** `syscall.c` read/write, `pagecache_read`,
  fsync, fstatfs and sysroot take no mount ref. Their safety comes from the
  fd or mapping pinning the mount (the busy check) plus the `ops->lock`
  barrier. Kernel-internal users of a bare `vfs_lookup()` result (sysroot,
  the shell, the demos) must not keep the inode across an unmount; under
  the BKL they cannot.
- **The K25 fallback serializes all appends system-wide** for FSes without
  `->append`. C2–C4 should implement `->append`. ramfs and ext4 matter most
  (logs).
- **`vfs_setattr` fallback.** It takes `ops->lock` only once the drivers
  set `.lock`; until then it is a bare write under a mount ref.
- **Drain timeout.** It is 5 s. A ref leak shows up as every umount of that
  mount returning EBUSY with the `[VFS] umount: …did not drain` line, not as
  a hang.

## Files

- `fs/vfs.{c,h}`: the locks, slots, refs, `vfs_unmount_ex`,
  `vfs_append`, `vfs_ftruncate` and the contract fields.
- `kernel/syscall.c`: the append branch of `ksys_write`, the
  `ksys_mount_opts` wrapper and `ksys_umount`. This is outside the spawn
  region (~87-140) that C6 edits.
- `kernel/kiface.c`: ftruncate goes through `vfs_ftruncate`.
- `fs/ramfs_vfs.c`: the two `ino->ops = NULL` lines removed (overlaps C2).
- `kernel/lock_order.h`: `LK_K25_VFS_APPEND`.
- Test plumbing (small, additive): `fs/vfs_test.c` (new), the `Makefile`
  source list, the AP idle hook in `kernel/smp.c`, and the call in
  `kernel/kernel.c`.
