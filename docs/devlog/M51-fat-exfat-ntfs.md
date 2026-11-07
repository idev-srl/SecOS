# M51 C4 — FAT32, exFAT and NTFS driver locks (`feat/fat-exfat-ntfs-lock`)

Plan: `docs/BKL_REMOVAL_PLAN.md` §4 Phase C, step C4. Base: `42a04cb`
(`fix/klock-fair`). The BKL stays on; with default settings nothing changes.
This step makes the three non-ext filesystem drivers correct without it.

## Lock design

| Driver | Lock(s) | Level | Covers |
|---|---|---|---|
| `fs/fat32.c` | `g_fat_lock` "fat32" | K30 | the single instance: geometry, `g_sec`, the FSInfo flag, the new allocation hint, the inode pool, the mount options |
| `fs/exfat.c` | `g_ex_lock` "exfat" | K30 | the single instance: `V`, the 64-sector cache `g_cbuf`, `g_bounce`/`g_zero`, the up-case table, the bitmap state, the inode pool, the function-static scratch entry sets |
| `fs/ntfs.c` | `g_vol_lock[i]` "ntfs0".."ntfs3" | K30 | **one per volume**: everything in that `nt_vol_t` (record/index caches, inode pool + hash, run lists, compression buffers, lookup buffers) |

Pattern (plan §4 C): every `vfs_fs_ops` entry and every exported function is
`lock(); r = x_locked(); unlock();`; internal code calls only `_locked`
functions; `KLOCK_ASSERT_HELD` sits in the central helpers (sector I/O,
inode-pool get/kill/move, `node_get`, `dev_read`, mount). Exported and
wrapped: lookup, readdir, read, write, create, mkdir, remove, rename,
truncate, setattr, sync, statfs, remount (FAT32), readlink (NTFS),
`*_mount`, `*_set_mount_options`, `exfat_probe`, `exfat_is_mounted`,
`exfat_sync`, `ntfs_mounted_count`.

**Deliberately lock-free: `mntinfo`.** procfs `gen_mounts` calls
`ops->mntinfo` with `g_proc_lock` (a spinlock) held, and no klock may be taken
inside a spinlock (cross-branch rule from C2). It only reads the mounted flag
and the device-name string, which do not change while the mount is in the
table.

**Cached inode paths.** procfs and the VFS copy `ino->path` without the
driver lock, so a lookup no longer rewrites the same path string on every hit:
`fs_set_path()` (`fs/fs_dirbuf.h`) writes only when the path really changes
(a rename, or a slot looked up under another spelling of a case-insensitive
name). `ino->ops` is never left NULL on a live inode (the VFS stamps the same
value after each lookup; NTFS `ops_init` no longer zeroes, see below).

**NTFS: per volume, not per driver.** The locks are `g_vol_lock[i]` for
`g_vol[i]` (kept outside `nt_vol_t` so the big volume array stays in .bss).
The only driver-wide mutable state was
the bounce sector `g_bounce` (now `nt_vol_t.bounce`), the boot-sector buffer
of `mount_inner` (now the volume's bounce too), `ntfs_probe`'s use of
`g_bounce` (now a private `kmalloc` buffer: probe has no volume) and the
mount-generation counter (now an atomic add). With those gone every byte of
mutable state belongs to one volume, so per-volume locks are correct and let
readers of two NTFS volumes (two disks) run in parallel. A VFS inode belongs to
one volume for its whole life (slots never move between volumes, chunks are
never freed), so `nt_read` finds the lock through `node->v` holding nothing.
The `-EROFS` stubs and `sync` (a no-op on a read-only driver) are stateless
and take no lock.

**Lock order.** Only levels already in `kernel/lock_order.h` are used; no new
level, no change to the file. Inside K30: block I/O (K40, `blkdev->lock`),
`kmalloc` (S70), `pagecache_invalidate` (S40, FAT/exFAT inode pool),
`vfs_mount_point_of` (S20, the "attached" checks). Never inside:
- `vfs_mount()`: every `*_mount` builds the volume under the lock, drops it
  and then calls `vfs_mount`, because C1's `vfs_ns_lock` (K20) comes before
  K30. A `g_mounting` flag (FAT32, exFAT) / `nt_vol_t.claimed` (NTFS) keeps a
  concurrent mount off the instance until the table has it.
- readdir callbacks: see below.
- user copies: the VFS hands the drivers kernel bounce buffers already
  (`ksys_read`/`ksys_write`/`ksys_getdents`).
- two volume locks at once: NTFS mount claims a slot under that slot's lock
  only; `ntfs_mounted_count` takes them one after the other.

**Readdir: collect first, then call back.** A readdir callback is arbitrary
caller code: the kernel shell's `ls`/`vls` page their output and wait for a key
(`cpu_wait_irq` — forbidden with a non-BKL klock held), tests and the VFS call
`vfs_lookup` on each child (re-entering the same FS = taking the lock twice).
New `fs/fs_dirbuf.h` (header-only, so the fuzz harnesses need nothing new):
the wrapper runs the `_locked` walk with `fs_dirbuf_cb`, which packs every
field a driver sets on a child (path, type, size, mode, owner, times, nlink,
ino) into a growing buffer, unlocks, and replays the records to the real
callback. An allocation failure returns -1 instead of a silently truncated
listing. ext2 (C3) and the virtual filesystems (C2) can use the same header.

**Mount options.** `*_set_mount_options` + `*_mount` is one request staged in a
driver global (`g_fopt_new`, `g_xopt_new`, `g_nopt_new`). The pair is only
correct if the caller serializes it — the BKL today, `vfs_ns_lock` after C1
(the mount syscall and the kernel shell are the only callers). The driver lock
only keeps the struct copy whole (FAT32/exFAT; NTFS has no driver lock).

## Wiring into C1 (`feat/vfs-locks`) — for the integrator

C1's `fs/vfs.h` adds two optional `vfs_fs_ops_t` fields: `.lock` (the
instance's K30 klock: `vfs_unmount` takes and releases it once as a drain
barrier, the in-memory setattr fallback runs under it) and
`.append(ino, buf, len, uint64_t* at)` (atomic O_APPEND). This branch does not
contain C1, so the fields are not set here; everything they need is exported
with C1's exact signatures. After merging both, add:

```c
/* fs/fat32.c, the fat32_ops initializer */
    .lock = &g_fat_lock, .append = fat32_append,
/* fs/exfat.c, the exfat_ops initializer */
    .lock = &g_ex_lock, .append = exfat_append,
/* fs/ntfs.c, ops_init(): per instance, no .append (read-only: the VFS
 * fallback reaches ->write, which returns -EROFS) */
    o->lock = &g_vol_lock[v->idx];
```

(`fat32_fs_lock()`, `exfat_fs_lock()` and `ntfs_fs_lock(ops)` return the
same pointers for code that only has the ops.) `ops_init` sets fields one by
one, so add the line next to the others, without zeroing.

- **Append.** `fat32_append` / `exfat_append` take the driver lock, read the
  file's current size from the inode slot, write there, set `*at` to that
  offset and unlock — one critical section. The slot size is authoritative in
  both drivers (one slot per directory entry; write, truncate, rename and
  unlink update it — FAT32's truncate did not, fixed here). There is no
  `VFS_OFF_APPEND` sentinel in `->write` (C1 dropped it).
- **Drain.** The drivers never hold their lock when calling `vfs_mount`
  (C1: "must not be held by the caller of vfs_mount*/vfs_unmount*"); a
  `g_mounting` flag (FAT32, exFAT) / `claimed` flag (NTFS) keeps a concurrent
  mount off the instance while the lock is dropped.
- **C1's other rules**, all met: readdir callbacks run with no FS lock held
  (`fs_dirbuf.h`); `mntinfo` is lock-free; a cached `ino->path` is rewritten
  only when it changes (`fs_set_path`); `ino->ops` is never NULL on a live
  inode.

## Fixes found on the way

- **FAT32 allocation held the lock for seconds.** `fat_alloc` was first-fit
  from cluster 2 with one FAT sector read per cluster. On a 512-byte-cluster
  volume (`mkfs.fat -F 32` on 64 MiB) a 64 KiB write did hundreds of thousands
  of sector reads: the lockdep stuck-lock watchdog (2 s) fired on the first
  C4 run (`stuck on klock fat32 ... held_ms=0x7CD`, three waiters). Now
  next-fit from a hint (lowered by every free, reset at mount), scanning one
  FAT sector per 128 entries. This was just as slow under the BKL before, it
  was only never measured.
- **FAT32 truncate left an open fd on a freed chain.** `fat32_truncate` updated
  the directory entry but not the inode slot, so an fd kept writing through
  the old `first_cluster` (freed by a shrink to 0) until the next lookup. It
  now refreshes the slot's chain head and size.
- **NTFS `ops_init` zeroed live function pointers.** A remount of a slot
  `mzero`ed `v->ops` before refilling it, while an fd left over from the
  previous mount of that slot could call through `ino->ops` (harmless under
  the BKL, a NULL call without it). It now writes the fields one by one, never
  zeroing (they depend only on the slot index).
- **NTFS shared buffers**: `g_bounce` and the `static b0` of `mount_inner`
  were shared by all volumes (see above).

None of the three drivers re-enters itself through an exported op except
exFAT's rename, which replaced a target through `ex_remove` — now
`ex_remove_locked`.

## Tests

**Forced concurrency: `fs/c4_fsstress.c`** (`-DSECOS_C4_FSSTRESS`), driven by
`tools/c4_fs_drive.py`. The BSP (from `kernel_main`, after the boot mounts)
and every AP (from its idle loop, hook in `kernel/smp.c`) pull 42 jobs from one
queue and run them at the same time outside the BKL. q35 AHCI: sda = fresh
64 MiB FAT32, sdb = fresh 64 MiB exFAT, sdc = the M42 NTFS image (mounted twice,
`/c4n` and `/c4n2`, two instances). Jobs, on FAT32 and exFAT at once:
FILES ×8 (own directory: 6 files written in odd-sized chunks with page-cache
invalidation, renamed, half truncated, a third unlinked, mkdir/rmdir, every
byte verified via `ops->read` and `pagecache_read`), APPEND ×4 (600 32-byte
lines into one shared `LOG.TXT` through `fat32_append`/`exfat_append`, each
returned offset line-aligned and growing),
SHARED ×4 (create/write/rename/unlink in one shared directory), SCAN ×2
(readdir of the shared directory and the root while they churn); on NTFS 6
readers hash all 3520 files, alternating between the two instances file by
file. Afterwards the BSP re-verifies every surviving file, checks the log
(every line once, whole, in each writer's order), checks that all NTFS readers
agree, prints the NTFS listing, syncs and unmounts. The host then runs
`fsck.vfat -n`, `fsck.exfat -n`, `exfat_img.py deepcheck` + VolumeDirty clear,
reads every file back (mtools / exfat-fuse) against the same formulas, and
compares the NTFS listing with the ntfs-3g reference.

Result, `-DSECOS_LOCKDEP`, `-smp 4`: **12/12 OK**, `[C4] RESULT PASS`, no
`[LOCKDEP]`/`[LOCK] BUG`/heap/`[EXC]`; ~48 s for the run. The locks really
were fought over:

```
[C4] workers=4 done=4 jobs=42 scans=16000 ms=47664
[C4] lock fat32 acquisitions=34569 contended=9111
[C4] lock exfat acquisitions=34557 contended=3071
[C4] lock ntfs /c4n acquisitions=34924 contended=6843
[C4] lock ntfs /c4n2 acquisitions=85031 contended=27105
[C4] ntfs readers=6 files=3520 errors=1 disagreements=0
```
(the one error is `/secret.efs`, EACCES by design; 3519 files match ntfs-3g.
The Interix symlink, which readdir reports as a file, is skipped.)

**Negative control**: the same image with `-DSECOS_C4_NOLOCK` (the three
drivers' lock macros compiled out) fails at once — 20 `[C4] FAIL` lines
(`lookup after create`, `append`, `shared rename`, `write`, `create` on both
FAT32 and exFAT) within the first seconds, then the kernel wedges and never
prints a result. So the test does see the races the locks prevent.

**Host fuzz harnesses** (drivers compiled unchanged against `stub/`): the
`stub/klock.h` of both harnesses now implements `KLOCK_INIT`, `klock_lock`,
`klock_unlock`, `klock_held` and `KLOCK_ASSERT_HELD` as checks (a recursive
lock, an unlock of a free lock or a helper reached without the lock aborts).
`exfat_fuzz` clean workload + 3000 mutated images (4362 mounts) and
`ntfs_fuzz` `list` (compared with ntfs-3g: 3537 objects OK) + 2000 mutated
images (3946 mounts): no crash, hang, UB or lock-discipline abort.

**Gates** (extracted from `tools/selftest.sh`, run in a private `/tmp`): see
the results section below.

## For phase D

- D3 can clear the file-syscall bits for FAT32/exFAT/NTFS files once C1's
  VFS locks exist: every driver op is self-locking. What is still unlocked is
  the VFS side: `ino->size`/`ino->path` of a slot are written under the driver
  lock but read by the VFS without it (aligned word reads, x86-64: no tearing,
  but `path` can change under a reader during a rename — C1's concern).
- A stale fd of an unmounted FAT32/exFAT volume is still served by the
  driver (FAT32 checks only `g_mounted`), and a new mount resets the inode pool
  those fds point into. Pre-existing, unchanged; C1's unmount draining
  (EBUSY on open files, or the drain barrier above) is the fix.
- FAT32 and exFAT remain one instance each (driver-global state). A second
  instance needs per-instance state like NTFS's `nt_vol_t`.
- The NTFS readdir reports an Interix symlink as a file (lookup says
  symlink): harmless, noted.

## Shared files touched

- `kernel/smp.c`: one `#ifdef SECOS_C4_FSSTRESS` call in the AP idle loop.
- `kernel/kernel.c`: one `#ifdef SECOS_C4_FSSTRESS` call after the B3 hook.
- `Makefile`: `fs/c4_fsstress.c` added to the sources (empty unless defined).
- `tools/{exfat,ntfs}_fuzz/stub/klock.h`: the checking stub above.
- Not touched: `kernel/lock_order.h` (no new level), `fs/vfs.{c,h}`,
  `tools/selftest.sh`.
