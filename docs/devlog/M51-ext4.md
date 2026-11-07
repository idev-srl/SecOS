# M51 C3 — ext2/ext3/ext4 driver lock (`feat/ext4-lock`)

Phase C, step C3 of the BKL removal (`docs/BKL_REMOVAL_PLAN.md` §4 C). Base:
`42a04cb` (`fix/klock-fair`). The BKL stays on and the default boot behaves
as before: with the BKL serializing every syscall the new lock is never
contended from userland. It makes the driver correct for phase D, when two
CPUs can be inside it at once. Lock API: `docs/devlog/M50-lock-core.md`.

Files: `fs/ext2.c`, `fs/ext2.h`, `fs/ext2ramdev.c`; tests `fs/ext4locktest.c`,
`tools/ext4_lock_test.sh`, `user/fsstress.c`, `tools/fs_stress_drive.py`.

## Lock design

All driver state is global, one ext volume at a time: geometry, the scratch
block buffers (`g_b`, `g_mb`, `g_xb`, `g_eb`, `g_tb`, `g_csb`, `g_jdesc`,
`g_jdata`), the JBD2 transaction (`g_jt[]`, `g_jt_active`), the journal
geometry, the VFS inode pool (`g_vchunk`/`g_vused`), the mount options and
the crash-injection switches. Nearly every helper touches several of them,
so finer locks would buy nothing. One lock:

```c
static klock_t g_e2_lock = KLOCK_INIT(LK_K30_FS, "ext2");
klock_t* ext2_fs_lock(void);                  /* fs/ext2.h */
```

- **Entry points.** Every `vfs_fs_ops` member is a wrapper
  `lock; r = x_locked(...); unlock;` (`e2op_*` → `e2_*_locked` /
  `e2j_*_locked`). The exported functions are wrapped the same way:
  `ext2_mount`, `ext2_mount_root`, `ext2_set_mount_options`,
  `ext2_journal_state`, `ext2_has_csum`, `ext2_last_error`,
  `ext2_test_set_crash`, `ext2_append`. Internal code calls only
  `_locked` functions and helpers. `KLOCK_ASSERT_HELD` sits in `blk_read`,
  `blk_write_direct`, `alloc_block`, `alloc_inode`, `path_to_ino`,
  `vino_alloc`, `ext2_read_super`, `e2_open_volume`, `jbd2_recover`,
  `jt_begin`, `jt_commit` and `e2_remount_locked`. Under lockdep, internal
  re-entry through a wrapper is reported as recursion.
- **JBD2.** Every mutating op is `E2_RO_CHECK; jt_begin; op; jt_finish`
  inside one hold. The transaction is begun, filled and committed (journal
  write, publish, checkpoint, retire) or aborted without the lock ever being
  dropped. `jbd2_recover` runs only from `e2_open_volume` (mount path),
  under the lock. `->sync` and `->remount` take the lock. `e2_remount_locked`
  calls `e2_sync_locked` directly.
- **Held across:** block I/O (K30 → blkdev K40 → xHCI S45), page-cache
  invalidation in `vslot_drop_cache` (S40), `kmalloc` (S70), `kclock` (S64)
  and debugcon. All of these are later levels.
- **Never held across:**
  - a VFS mount-table call. `vfs_mount`/`vfs_mount_root` call back into
    `->mntinfo` (`vfs_dev_for`), and C1 may put `vfs_ns_lock` (K20, lower
    than K30) into `vfs_mount_point_of`.
  - a readdir callback.
  - user memory: the driver only ever sees kernel bounce buffers.
- **`->mntinfo` is lock-free on purpose.** procfs builds `/proc/mounts`
  under its `g_proc_lock` spinlock (S15) and calls `ops->mntinfo` there
  (`fs/procfs.c:342`). Taking a klock there is a lockdep panic, and the
  first boot of this branch found it. `e2_mntinfo_nolock` reads only words
  set at mount time (`g_mounted`, `g_dev`, `g_use_extents`, `g_jhas`) and
  returns static strings.
- **readdir collects, then delivers.** `e2_readdir_locked` packs
  `{type, name length, name}` records into a growing `kmalloc` buffer.
  `e2op_readdir` drops the lock, then builds each child `vfs_inode_t` and
  calls the callback. The callbacks run the shell pager (`cpu_wait_irq`)
  or stat children through the VFS, which re-enters this driver. Neither
  may run under the lock.
- **Mount claim.** Because `vfs_mount*` runs unlocked, a mount first claims
  the driver under the lock (`g_e2_claimed`) and only then checks
  `e2_busy()` (the VFS table). The flow is:
  1. take the lock;
  2. open the volume (`read_super`, journal replay, `jt_init`, read-only
     flags);
  3. release the lock;
  4. `vfs_mount*`;
  5. unclaim.

  A second `ext2_mount`/`ext2_mount_root` meanwhile fails as "already
  mounted" instead of retargeting the globals under the first.
- **Atomic O_APPEND (C1's `->append`).**
  `int ext2_append(vfs_inode_t* v, const void* buf, size_t len, uint64_t* at)`
  has exactly the signature of C1's new `vfs_fs_ops` member
  (`feat/vfs-locks:fs/vfs.h`):

  ```c
  int (*append)(vfs_inode_t* inode, const void* buf, size_t len, uint64_t* at);
  struct klock* lock;
  ```

  It takes the lock, reads the end of file from the on-disk inode, writes
  there (one JBD2 transaction), sets `*at` to that offset and unlocks.
  There is no write-offset sentinel: C1 chose a separate op. Without
  `.append`, the VFS would serialize every ext4 append behind its global
  K25 `vfs_append_lock`.
- **Unmount draining (C1's `.lock`).** There is no `->umount` op.
  `vfs_unmount` takes and releases `ops->lock` once after detaching the
  mount, as a barrier for calls still in the driver through an open file.
  The VFS never holds it while calling an op, because the lock is not
  recursive. `ext2_mount*` call `vfs_mount*` without holding it (stricter
  than C1 requires).
- **Wiring once C1 and C3 meet** (the integrator; two initializer fields in
  `fs/ext2.c`'s `ext2_ops`, both symbols already exist):

  ```c
      .lock = &g_e2_lock, .append = ext2_append,
  ```
- `fs/ext2ramdev.c`: the dummy device's storage is now `const`. It has no
  `->write` and is never registered (dead code), so it needs no lock.

## Problems found

1. **Pre-existing data corruption (fixed, own commit).** `bmap_alloc()`
   kept the single-indirect block in `g_xb` across `alloc_block()`, which
   reads the block bitmap into `g_xb`. The patched bitmap was written over
   the indirect block, so on a non-extent (ext2) file, every block past the
   12th that a write allocated was mapped to garbage (`0xFFFFFFFF`).
   e2fsck reported "illegal blocks", and later writes failed. The lock
   test's 53 KB append log on a plain ext2 volume with 4 KiB blocks found
   it. This is single-threaded and unrelated to locking. ext4 (extents) was
   not affected.
2. **procfs → `->mntinfo` under a spinlock:** see above. **C2/C4 must know:**
   no FS may take its klock in `->mntinfo`, unless C2 changes procfs to
   collect the mount list first and call `mntinfo` outside `g_proc_lock`.
3. **`vfs_mount` → `->mntinfo`:** a driver cannot hold its own lock across
   `vfs_mount*`. Hence the claim flag.
4. **readdir callbacks** re-enter the VFS and page output. Hence collect,
   then deliver (plan §8 "callbacks run under a lock").
5. **`ext2_set_mount_options` + `ext2_mount`** are two calls (`syscall.c`
   `SYS_MOUNT`). The options are global, so a concurrent mount could
   consume another's options. Each call is locked, but the pair is only
   atomic under the mount syscall's serialization (BKL today, C1's
   `vfs_ns_lock` in phase D).
6. **VFS inode slots are shared, mutable objects.** `->lookup` returns a
   pointer into the slot pool and rewrites the slot's
   `path`/`size`/`mode`/`uid`/`gid`/`times`/`nlink` under the lock. The VFS
   and the syscalls read those fields with no lock. Hard links share one
   slot, and `v->path` holds whichever name was looked up last, so a
   concurrent lookup can tear a path that another CPU is reading (for
   example a path-based op issued from an fd). Word-sized fields are fine.
   `path` is not. Mitigation (cross-branch rule from C2): `->lookup` now
   rewrites the cached path only when it really changes, which happens
   only when a hard link is looked up by its other name. An identical
   lookup no longer writes it. The hard-link case remains, so phase D
   should stop using `ino->path` for fd-based ops or snapshot it under the
   FS lock. `ino->ops` is never cleared on a live slot (the VFS stamps it;
   only a brand-new slot starts zeroed).
7. **No unmount hook.** After `umount`, `g_mounted` stays 1 and an open fd
   on the old volume still reaches the driver. A later mount of another ext
   volume retargets the globals underneath that fd. This predates C3. C1's
   EBUSY on open files closes it.

## Tests

### In-kernel forced concurrency (`-DSECOS_EXT4_LOCKTEST`)

`fs/ext4locktest.c` runs at boot, before init. Every CPU drives the mounted
volume through `ext2_vfs_ops()` with mount-relative paths, outside the BKL:
the BSP from `kernel_main` and each AP from its idle loop (hook in
`kernel/smp.c`, call in `kernel/kernel.c`, next to B3's). Each worker owns
four model-checked slot files, and every iteration does a mix of:

- create with data, grow by write, overwrite in place;
- truncate shrink/grow;
- rename into a shared directory and back, rename over a slot, remove;
- mkdir + nested file + rmdir;
- symlink + readlink, link + remove, setattr;
- readdir of its own directory (exact) and of the shared one;
- statfs, lookup + read (full and partial);
- one record appended to a shared log through `ext2_append()` (the
  returned offset is checked too).

The BSP then checks every model and the log: whole records, per-worker
sequence complete and in order, nothing lost. It also requires that the
lock was really contended, then syncs. `-DSECOS_EXT4_LOCKTEST_TRACE` prints
every operation.

`tools/ext4_lock_test.sh <iso> [smp]` boots it on a fresh 256 MiB default
`mkfs.ext4` volume (journal + metadata_csum: every op is a JBD2
transaction) and on a plain `mkfs.ext2` volume (direct writes). It requires
RESULT PASS, no lockdep/lock/fault marker and a clean host `e2fsck -fn`.

| run | result |
|---|---|
| lockdep build, -smp 4, ext4csum | PASS: 4 workers, 3176 lock acquisitions, 3098 contended, max wait 7.0e8 cycles, log 480 records / 176557 bytes, e2fsck clean |
| lockdep build, -smp 4, ext2 | PASS: same op mix, e2fsck clean |
| final code (`ext2_append`, path rewrite only on change), -smp 4, both volumes | PASS, e2fsck clean |
| **negative control**: same test, lock compiled out (no-op `E2_LOCK`) | FAIL at once on both volumes: lookups/readdir/mkdir failing, log missing, host e2fsck: "Bad magic number in super-block" (the ext4 volume's primary superblock was destroyed) |
| lockdep build, -smp 8, ext4csum and ext2 | PASS: 8 workers, 6367 acquisitions, 6322 contended, max wait 1.5e9 cycles, log 960 records, e2fsck clean on both |

### `/bin/fsstress` + `tools/fs_stress_drive.py` (plan §7 tests 1+2)

`fsstress <dir> [workers] [iters] [seed]`: N forked workers (placed
round-robin on the CPUs by the kernel) on `<dir>/fss.<pid>`. Each worker
model-checks its slot files:

- O_TRUNC rewrite in random chunk sizes, O_APPEND, truncate shrink/grow;
- rename into a shared dir and back, or over another slot; unlink;
- nested mkdir/rmdir (rmdir of a non-empty dir must fail);
- symlink/readlink, chmod + stat, link + nlink;
- getdents (exact against the model), stat;
- read() compared with a MAP_PRIVATE mmap and with the model.

Each iteration also writes one O_APPEND line (32..4000 bytes) to a shared
log in a single write(). The parent then checks the log: every line whole
and exact, each worker's lines numbered 0..iters-1 in order. `-c <base>`
checks the log of a run that was cut off: each worker's lines must form a
gap-free prefix. `-n` skips symlink/link/chmod for filesystems without
them. The target directory is a parameter, so FAT32/exFAT/ramfs can reuse
the program later; `fs_stress_drive.py --target`.

`fs_stress_drive.py --iso ISO --smp 4` boots a CONSOLE=root image on a fresh
`tools/mkroot.sh` root (ext4 + journal + metadata_csum on virtio), runs
fsstress in `/root`, powers off and requires a clean `e2fsck -fn`.

With `--crash K` it repeats K times:

1. start a long fsstress;
2. `kill -9` QEMU at a random 4..25 s;
3. boot again, so the kernel replays the journal;
4. `fsstress -c` must find every leftover log intact;
5. clean up and power off;
6. `e2fsck -fn` must be clean.

Today all of this still serializes under the BKL. It becomes a real race
test in phase D.

Results, all with the `-DSECOS_LOCKDEP` CONSOLE=root image at -smp 4:

| run | result |
|---|---|
| `fs_stress_drive.py --workers 6 --iters 100` | fsstress PASS (6 workers, about 300-350 checked ops each, 600 log lines) in 116 s; no leftovers; clean poweroff; no kernel/lockdep marker; host `e2fsck -fn` clean |
| `--workers 8 --iters 150 --rounds 2` (with the path-rewrite change) | 2× PASS (301 s, 286 s), e2fsck clean |
| `--crash 5` | 5/5 iterations: killed after 6-22 s; the replay boot finds every log intact; e2fsck clean after each. The kernel replayed a committed transaction once (`txns=1`); the other kills fell outside the publish..retire window |
| `--crash 6` (with the host-replay check) | 6/6: host `e2fsck -fy` on a copy fixed nothing beyond the journal; kernel replay once (`txns=1`); log intact; e2fsck clean after each |

### Existing gates and checks

Each section was extracted verbatim from `tools/selftest.sh` and run with
the selftest's `-smp 2` QEMU shim, in a private `/tmp`:

| gate | result |
|---|---|
| M10 ×{fat32, ext2, ext4} | 24/24 PASS |
| M23 persistent ext2 root + M33 metadata_csum ext4 root (e2fsck clean) | 6/6 PASS |
| M27a journal replay | 6/6 PASS |
| M26 (chmod/chown/symlink/umount+remount on ext2) | 7/7 PASS |
| M27b crash atomicity (before commit / after publish) | 9/9 PASS |
| M50 PAGECACHE (`-DSECOS_PAGECACHE_TEST -DSECOS_LOCKDEP`, pcstress on ext4 `/mnt` at -smp 4) | 12/12 PASS |
| M41 persist (`tools/persist_test.sh --crash-runs 3`, CONSOLE=root) | 30/30 after the expected-count update: adding `/bin/fsstress` makes the first boot install 34 programs (was 33). The run showed `written=34 ... failed=0`, and the gate now expects 34 |
| `make lockdep-check` | 18/18 PASS (ssd4, ssd8, nsd4, off4, prd8; no lockdep report) |

Warnings: `-Wall -Wextra` is at 41 in the normal, `-DSECOS_LOCKDEP` and
`-DSECOS_LOCKDEP -DSECOS_EXT4_LOCKTEST` builds, the baseline. The new
files add none.

## For phase D

- One lock per volume, held for a whole operation including the synchronous
  JBD2 commit + checkpoint: about 4 + 2n block writes for an n-block
  transaction, plus the data. Reads (lookup/read/readdir) serialize with
  writes; there is no reader concurrency. Under the 4-CPU lock test, waits
  reached about 7·10⁸ cycles. That is acceptable (plan §8: "long ext4
  commits on USB make others spin"), but `/proc/lockstat` "ext2" is the
  number to watch. The page cache absorbs repeated reads.
- Clear the ext2-related syscalls from `syscall_bkl_mask` only after C1:
  - `vfs_ns_lock` around mount/umount and the `set_mount_options` +
    `ext2_mount` pair;
  - `ksys_write` O_APPEND going through `vfs_append` → `.append = ext2_append`;
  - fd-based ops no longer using the mutable `ino->path`;
  - EBUSY for umount with open files.
- `->mntinfo` must stay lock-free (procfs S15), or procfs must collect
  first.
- Run `tools/ext4_lock_test.sh` and `fs_stress_drive.py` (normal and
  `--crash`) with `bkl=off` once the mask allows it: then fsstress races
  for real.

## Plan corrections

- §4 C3 lists `umount` among the ops to lock: ext2 has no unmount entry
  point; the VFS just drops the mount. The lock is exported for draining
  instead.
- `jbd2_recover` is not an exported op: it runs only inside the mount path,
  which already holds the lock.
- `->mntinfo` cannot follow the "every op takes the FS lock" pattern
  (procfs spinlock). The same holds for every C2/C4 filesystem.
- `fs/ext2ramdev.c` is dead code: `ext2ramdev_register()` has no caller.
