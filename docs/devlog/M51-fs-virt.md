# M51 C2 — ramfs, devfs, procfs and sysfs locks (`feat/fs-virt-locks`)

Phase C, step C2 of the BKL removal (`docs/BKL_REMOVAL_PLAN.md` §4), on
top of `fix/klock-fair` (42a04cb). The BKL stays on; with default settings
nothing changes. This step gives the in-memory filesystems the locking they
need once the BKL no longer serializes syscalls. The lock API is the one of
`docs/devlog/M50-lock-core.md` ("For phases B–D"); the process rules are
those of `docs/devlog/M50-proc-lifetime.md`.

## ramfs (`fs/ramfs.{c,h}`, `fs/ramfs_vfs.c`)

The M48 heap corruption (sshd.log appended from two CPUs) came from here:
`ramfs_write` allocates a bigger buffer, copies, frees the old one and
swaps the pointer, and two CPUs did that to the same entry at once.

- **One instance klock**, `LK_K30_FS` "ramfs", exported as
  `klock_t* ramfs_fs_lock(void)`. It guards the entry table (`ramfs.c`)
  and the VFS inode cache (`ramfs_vfs.c`) together, because the two change
  together: removal marks the inode dead, lookup rebinds cache slots.
- **Pattern:** every `vfs_fs_ops` entry is `RLOCK(); r = x_locked(); RUNLOCK();`.
  Every exported `ramfs_*` function (kernel.c, the shell's `rf*` commands,
  `ramfs_init`) is a locking wrapper around `ramfs_*_locked`, which is
  exported for code that already holds the lock. The `_locked` helpers
  assert it with `KLOCK_ASSERT_HELD`.
- **Inside the lock:** only the heap (S70) and `pagecache_invalidate`
  (S40, from the removal path). No user copy, no wait, no other FS.
  `ksys_read`/`ksys_write` pass kernel bounce buffers, and page-cache fills
  call `->read` without `pc_lock` since B2, so neither nests wrongly.
- **readdir** runs the callback **unlocked**, on a private copy of each
  child inode. It takes one lock section per child, with a table cursor
  (`ramfs_next_child_locked`). The kernel shell's `ls` callback pages its
  output and can wait for a key; that must never happen under a klock.
  An entry created or removed during the listing may or may not show up,
  as with readdir(3).
- **Write** goes to the entry the inode is bound to
  (`ramfs_write_entry_locked`). It used to look the entry up again by the
  entry's own name, which a concurrent rename could redirect.
- **Rename over an existing target** drops the target and renames in one
  section. It used to be two separate calls that relied on a
  non-preemptible single CPU.
- **Lookup** no longer sets `ino->ops = NULL`. The VFS restamped it after
  every lookup, but another CPU calling through the same inode in that
  window would have jumped through NULL. Cached fields (path, type, size)
  are only rewritten when they changed, so a lock-free reader of an inode
  it already holds sees a path change only across a real rename.
- **chmod/chown/utimes** go through a new `->setattr` op. It does what the
  VFS fallback in `vfs_setattr_at` did (update the cached inode), but
  under the lock that also rebinds and resets cache slots.
- `truncate` keeps the cached inode size current (as `write` has since
  B2). Truncating a directory now fails instead of attaching data to it.

### For C1 (`fs/vfs.h` per-FS lock contract)
- The accessor is `ramfs_fs_lock()` (`fs/ramfs.h`).
- **Unmount draining:** ramfs is only ever the root, and the root is never
  detached (`vfs_unmount("/")` is refused), so there is nothing to drain.
  If C1's contract wants a lock per mount anyway, it is this one.
- **Atomic O_APPEND:** resolve the offset in `ramfs_vfs_write_locked`,
  where `e` is the entry under the lock:
  `if (offset == VFS_OFF_APPEND) offset = e->size;` and return the
  offset in whatever form C1 chooses. The self-test's `log_append` is
  exactly this sequence through the accessor (`ramfs_find_locked` +
  `ramfs_write_entry_locked`), and it never lost or tore a record.
- The fallback in `vfs_setattr_at` (inode fields written by vfs.c without
  any FS lock) is no longer reached for ramfs or devfs. It is still reached
  for procfs and sysfs, whose inodes are regenerated anyway.

## devfs (`fs/devfs.c`)

- The node table is now **built in `devfs_mount`**, before `/dev` is
  reachable. It used to be built lazily on the first access, which two
  CPUs could do at once. Each node's inode (path, type, size, `fs_data`,
  `ops`, mode, owner) is complete before `g_nnodes` is raised past it
  (release store; readers load it with acquire). Lookups and listings read
  the table without a lock, and the listing callbacks run with nothing
  held.
- **What is still mutable:**
  - **Block devices registered after the mount** (`usbrescan`): they are
    appended by `devfs_sync_blocks()` under `g_devfs_lock`. Lookups and
    listings call it, and it is one `block_count()` when nothing changed.
    The old lazy build never showed such disks at all.
  - **mode/uid/gid** (login chowns `/dev/console`, sshd chmods a pty
    slave): a new `->setattr` op writes them under `g_devfs_lock`. The
    lookup runs before the lock, because `/pts/N` and `/input/eventN`
    belong to `kernel/pty.c` and `drivers/input.c`, whose locks (S10/S15)
    must not nest inside S15.
- `g_devfs_lock` is a spinlock at `LK_S15_SUBSYS`. Only the block registry
  (S64) nests inside it. A K30 klock was not needed: devfs holds no state
  that a long section could touch.

## procfs (`fs/procfs.c`) — the B1 gap

B1 left `snap_cb` reading another process's cwd, its fd table and
`ino->path` under `proc_lock` alone, while the owner rewrites them without
any lock. Lifetime was already safe: `proc_lock` keeps the process, and FS
inode slots are never freed while mounted. Consistency was not. A reader
could copy a half-written cwd, or see a torn fd entry mid-`dup2`, mid-close
or mid-`pipe()`: for example `used=1, is_pipe=0` with `inode` still
pointing at a `pipe_t`, which it then printed as a path.

Reader-side copies alone cannot fix this, so the writers changed too.

- **Writers** (`kernel/syscall.c`, `kernel/process.c`) make every store that
  changes *what an fd is* (`used`/`inode`/`is_pipe`/`pipe_w`/`is_sock`), and
  chdir's cwd copy, under the **owner's `mm_lock`**. The covered paths:
  - `fd_alloc` and a new `fd_release`;
  - pipe;
  - the open install;
  - close;
  - `dup`, `dup2` and `fcntl(F_DUPFD)`;
  - socket and accept;
  - spawn's stdio inheritance;
  - exec's close-on-exec;
  - the exit-time `process_release_pipes`.

  The object references (pipe, socket, pty, evdev) are still taken and
  dropped outside the lock, because their locks rank below S35. So close,
  close-on-exec and exit now *unhook the entry first* and drop the object
  afterwards. Previously a reader could see an entry whose pipe or pty
  inode had just been freed. offset/flags/cloexec are read by nobody else
  and stay unlocked.
- **Reader:** `snap_cb` copies the cwd and the fd table inside the `mm_lock`
  section it already had for the VMAs (`g_proc_lock` S15 → `proc_lock` S30
  → `mm_lock` S35), classifying each fd from one copy of its entry.
  `ino->path` is copied bounded and always terminated. It is the FS's to
  rewrite, under a klock that cannot be taken under a spinlock. ramfs now
  rewrites it only on a rename, so the worst case is a path in mid-rename.
  The procfs inode-pool busy check (`pool_busy_cb`) reads the fd tables
  under `mm_lock` too.
- **readdir** collects keys and names under `g_proc_lock` into a per-call
  buffer and runs the callbacks after dropping it. The static key and name
  tables and the shared static `ent` (two CPUs listing `/proc`
  overwrote each other's entry) are gone.
- `gen_mounts` calls `ops->mntinfo` under `g_proc_lock`. **C3/C4: `mntinfo`
  must stay lock-free** (it returns constant strings). A K30 klock taken
  there would be a klock under a spinlock.

## sysfs (`fs/sysfs.c`)

- `g_sysfs_lock` (S15, from S15 review) already guarded the generation
  buffer and the inode pool. The review found the same bug procfs had in
  M50: `mk()` recycled pool slots round-robin even when an open fd held
  one, so after 32 other paths an fd on `/sys/block/vda/size` read as
  another node. It now skips slots an fd holds (`process_foreach` +
  `mm_lock`, as in procfs).
- `readdir` takes no lock at all now. It reads only the block registry
  (which has its own lock) and the CPU count, and builds each entry in the
  caller's scratch inode instead of the shared static `g_ent`. So the
  callbacks no longer run under the spinlock.

## Lock order (`kernel/lock_order.h`)

No new level. The entries were updated:
- K30 names `ramfs_fs_lock()` and what nests inside it.
- S15 adds `g_devfs_lock`.
- S35 `mm_lock` now also publishes `p->cwd` and the fd entries.

The resulting order is:
- ramfs: K30 → {S40 page cache, S70 heap}.
- devfs: S15 → S64.
- procfs/sysfs: S15 → S30 → S35.
- writers: S35 alone (never across a pipe, socket, pty or input unref).

## Tests

- **`fs/fsvirt_test.c`** (`-DSECOS_FSVIRT_TEST`, AP idle hook in
  `kernel/smp.c`, called from `kernel_main` after `/proc` is mounted). Every
  CPU runs the same phase at once, outside the BKL. Output is
  `[FSVT] …: OK|FAIL` and `[FSVT] RESULT PASS|FAIL`.
  - **ramfs**, run 2 passes × 300 iterations per CPU through the ramfs
    `vfs_fs_ops` (`ramfs_vfs_ops()`):
    - Each iteration does create, a write grown in odd-sized chunks with
      byte-exact readback, truncate, chmod/chown, rename (old name gone,
      same inode, contents), an atomic 32-byte append to one shared log,
      a write and readback of the CPU's own stripe of one shared file held
      by everyone, a readdir check (every name well-formed), every 8th
      iteration a subdirectory with a file in it, then unlink.
    - After each pass: the log has exactly one intact record per append,
      in per-CPU order; `heap_check()` passes (headers, canaries, 0x6B
      free poison with lockdep); every ramfs slot was released.
    - MemFree is equal after the two passes.
  - **procfs:** three held (never started) processes. CPU 1 rewrites
    their cwd (two 201-byte strings) and fd 5 (a ramfs file, a fake pipe
    end whose memory reads `ZZZ…` if taken as an inode, closed) under
    `mm_lock`, exactly like the syscalls. The other CPUs read
    `/proc/<pid>/cwd`, `/proc/<pid>/fd/5`, status, stat and maps. Every cwd
    must be one of the two strings, and every fd link `pipe:[…]` or
    `/fsvt/fda`.
  - **Negative control:** the same cwd rewrite without `mm_lock`. The
    readers must see torn strings.
  - **Exits:** the held processes are destroyed
    (`process_detach_unstarted` + `process_destroy`) while every CPU keeps
    reading their `/proc` entries.
- **Negative control for ramfs**, run by hand and not committed:
  `RLOCK`/`RUNLOCK` were made no-ops. The run died within the first
  iterations with `[PANIC] HEAP: double free`, plus wrong sizes and
  contents on every CPU. That is the M48 failure, and it shows that the
  test reaches the race.
- **Results** (lockdep build, `CONSOLE=root`, ramfs root, no disk):
  - -smp 4: PASS. 12239 locked cwd reads, 0 torn; 6228 fd reads, 0 bad.
    The negative control saw 4323 torn reads out of 41050. Reads of the
    exited pids failed cleanly 16927 times. MemFree was 1027456 KiB before
    the test and after both passes.
  - -smp 8: PASS. 4465 cwd reads, 0 torn; 2250 fd reads, 0 bad. The
    negative control saw 1521 torn reads. One ramfs pass took about 10 s
    on the loaded host.
  - -smp 2: PASS. 35853 cwd reads, 0 torn. The negative control saw
    12227 torn reads.
- **Selftest gate "M51 FSVIRT"** (`tools/selftest.sh`, last gate): builds
  the image above and boots it at -smp 4. Every `[FSVT]` check becomes a
  gate check, and the log must contain no `[LOCKDEP]`, panic or `[EXC]`.
  17/17 when run alone.
- **Existing gates** (run on their own, private /tmp):
  - M23: 7/7.
  - KERNEL INTERFACES: 51/51.
  - M50 PAGECACHE: 12/12.
  - PROCRACE at -smp 4 and -smp 8: 18/18.

  That is 91 checks in total, with 0 failures.
- `make lockdep-check`: 18/18.
- `-Wall -Wextra`: 35 warnings in the normal and the lockdep build, down
  from the 41 baseline. The 6 removed were misleading-indentation
  warnings in the old `ramfs_vfs.c`/`ramfs.c` one-liners. None are in the
  touched files except the pre-existing `kernel/process.c:1142`.

## For phase D

- ramfs, devfs, procfs and sysfs no longer need the BKL for their own data.
  Clearing the BKL bit of a VFS syscall still waits for C1 (namespace,
  O_APPEND) and, for files on disk, for C3/C4.
- `ksys_write`'s O_APPEND (`vfs_lookup(ino->path)` then `cur->size`,
  outside any FS lock) stays racy until C1 wires `VFS_OFF_APPEND` into
  ramfs, as described above.
- The kernel shell's `rf*` debug commands and `rftree` read
  `ramfs_find`/`ramfs_list_path` pointers after the lock is dropped. They
  are BKL-only debug code: take `ramfs_fs_lock()` around them, or keep
  them under the BKL.
- `kernel/shell.c`'s pipeline setup writes a child's fds without `mm_lock`
  (the child is held and the shell runs under the BKL). The same applies
  if it ever runs without the BKL.
- Pre-existing and unchanged: an fd held open on a **removed** ramfs file
  keeps a dead inode, and once the 260-slot cache is full a later lookup
  may recycle that inode for another file. The fd then reads the new
  file. Recycling happens as late as possible, but it is not refcounted.
  A real fix needs inode references (C1's area).
- `vfs_setattr_at`'s fallback still writes inode fields from vfs.c without
  an FS lock. procfs and sysfs use it; their inodes are regenerated.
- procfs `ino->path` copies of *other* filesystems' inodes (ext4, exfat,
  ...) are only as stable as those drivers make them. C3/C4 should, like
  ramfs now, rewrite a cached inode's path only when it actually changes.

## What the plan got wrong

- "procfs/sysfs: review — they already have their locks". The procfs
  `snap_cb` gap could not be closed in procfs alone: the fd-table and cwd
  writers in `kernel/syscall.c` and `kernel/process.c` had to publish under
  `mm_lock`. sysfs had a real bug (recycled open slots), not just a review
  item.
- "devfs … then lock what's mutable": after the table is built at mount,
  the only mutable state is late block registration and chmod/chown. A
  small S15 spinlock covers both; devfs needs no K30 klock.
- The readdir callback contract was never written down: three of the four
  filesystems called back under their lock. The kernel shell's `ls`
  callback can wait for a key, so **no FS may call a readdir callback with
  its lock held**. C3/C4 must follow the same rule.
