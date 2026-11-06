# M41 follow-up — mount hardening

Branch `fix/mount-hardening` (from `feat/exfat` at `5a3cdbe`). The exFAT work
(`docs/devlog/M41-exfat.md`) found three bugs outside its own driver. This
branch fixes them, fixes what the audit turned up around them, and adds a
selftest gate.

## 1. Dangling mount-point pointer (memory safety)

`vfs_mount()` stored the `mount_point` pointer it was given. `ksys_mount`
passes `fat32_mount`/`ext2_mount` the kernel-stack buffer `ktgt`, and the
shell's `mountdev`/`ext2mount` pass their line buffer. So after a userland
`mount -t vfat|ext2 ... /x`, the mount-table entry pointed at dead stack.
Every path lookup, `/proc/mounts` and `umount` then read whatever was there.
On the unfixed kernel the regression run showed it: after the loop, `cat`,
`seq` and `grep` were "command not found". A garbage mount point had
shadowed `/bin`.

- **Fix (VFS-level):** `vfs_mount()` copies the path into a static per-slot
  buffer that the VFS owns (`g_mp_store`), released by `vfs_unmount` and
  `vfs_unmount_all`. Because the buffers are static, a pointer handed out by
  `vfs_mount_info()`/`vfs_mount_point_of()` is always a valid string, even if
  it races an unmount. A trailing `/` is dropped, and `"/"` is refused (the
  root goes through `vfs_mount_root`).
- `ksys_mount` normalizes the target with `proc_resolve_path`, as umount
  already did, so relative paths and `/x/` work.

### Audit

| caller / FS | mount point passed | before |
|---|---|---|
| `SYS_MOUNT` → fat32 / ext2 | stack `ktgt` | **dangling** |
| `SYS_MOUNT` → exfat | copied into `g_mp` by exfat | ok |
| shell `mountdev`, `ext2mount` | shell line buffer | **overwritten by the next command** |
| boot probe (`kernel.c`) | `"/mnt"` literal | ok |
| procfs, devfs, sysfs, ramfs root | literals | ok |

Other instances found by the audit:
- **A second fat32/ext2 mount hijacked the attached volume.** Both drivers
  keep their state in globals, and `ext2_read_super` sets `g_dev` before it
  checks the magic. So even a *refused* `mount -t ext2 /dev/sdX` re-pointed
  the attached ext2 volume, the persistent root included. Both now refuse
  while attached, as exFAT already did.
- **umount with open files.** The next mount resets the driver's inode pool,
  so an fd still held across the unmount would be bound to another file.
  `ksys_umount` now returns `-EBUSY` while any process holds an fd or a
  file-backed mapping on that filesystem, like Linux without `MNT_DETACH`.
- The unmount path frees only the mount-point slot. Driver inode chunks stay
  allocated for reuse by the next mount (intended, as in ext2/exfat).

## 2. Stale cached pages after inode-slot reuse

The page cache is keyed by `vfs_inode_t*`.
- **fat32** kept 32 path-keyed slots, reset them on every mount, and gave its
  last slot to every further file. So after a remount, or once 32 names had
  been looked up, `read()` returned another file's bytes, and an open fd was
  re-pointed at another file. `f9` and `F9`, the same FAT file, also got two
  slots with two caches that disagreed.
- **The fix:** slots are now keyed by directory-entry location and come in
  kmalloc'd chunks, never recycled while mounted (the M40/M41 rule). An
  unlinked or replaced entry's slot is marked dead: reads return EOF and
  writes fail. A rename retargets the slot.
- **The central fix:** `vfs_mount`, `vfs_unmount`, `vfs_mount_root` and
  `vfs_replace_root` call the new `pagecache_invalidate_fs(ops)`, which drops
  every page read through that filesystem. Each page-cache entry now records
  the ops it was read through. No driver that resets its pool on mount can
  serve stale pages again.

Other instances found by the audit:
- **ramfs** recycles dead inodes once its cache is full. **ext2** gives a
  freed inode number's slot to the next file that gets the number. Both now
  drop the pages when the file goes away. That also covers kernel-internal
  `vfs_remove` callers, such as the BusyBox install at boot, which never
  invalidated.
- **devfs, procfs, sysfs** are `VFS_FS_NOCACHE` for `read()`, but `mmap`
  still put pages of generated content into the cache, keyed by procfs's
  recycled pool slots. File-backed `mmap` of a NOCACHE filesystem now fails
  with `-ENODEV`, as on Linux.

## 3. `st_dev` / `st_ino`

`stat_fill` reported `st_dev = 1` for every file.
- Each mount now carries a device id in `vfs_mount_t.dev`.
  - **Block-backed mounts** get the Linux `dev_t` of their device (from
    `->mntinfo`, via `vfs_blkdev_id`): `vdX` → 253, `sdX` → 8 (16 minors per
    disk, partitions included), `nvme<c>n<ns>[p<k>]` → 259, `usbN` → `sd`
    minors after `sdd`. These are stable across boots. Other names hash under
    major 240.
  - **Virtual filesystems** get an anonymous `0:N`, starting at 17 so they
    stay clear of the console/socket pseudo ids that `fstat` uses.
  - The encoding is glibc `makedev`.
- `vfs_inode_t` gains `ino`:
  - ext2/ext4 report the on-disk inode number;
  - fat32 and exFAT report the entry location (root = 1). This is stable
    across remounts until a rename moves the entry;
  - everything else keeps the inode pointer (0 = "derive one").

BusyBox `df /mnt` now names `/dev/sda`, `stat -c %d` differs between `/` and
`/mnt`, and `find -xdev` stops at a nested mount.

## Testing

QEMU only.
- **Gate** at the end of `tools/selftest.sh` (`MNT`, 13 checks): one
  `-DSECOS_HEAP_DEBUG` run on q35 with vfat, ext2 and exFAT disks on AHCI,
  driven over the serial bash. **13/13 PASS.**
  - The 50-round loop mounts and unmounts all three and checks that
    `/proc/mounts` holds exactly the right lines.
  - The fat32 stale-read reproducers: write 40 files, then read them back;
    read `f7`, remount, read `f8`; read `F9`, then write `f9` and read `F9`.
  - Also: `-EBUSY`, the refused second ext2 mount, a trailing-slash mount
    point, `st_dev`/`df`/`find -xdev`, and `heapcheck` (no corruption).
- The same command script on the unfixed kernel (`5a3cdbe`):
  - `st_dev` is 1 everywhere, and `df /mnt` shows `ramfs /`;
  - the loop scores 0/50;
  - `/x` reads come back empty;
  - `/bin` is shadowed.
- `tools/selftest.sh` in full, run with a private `/tmp` (`unshare -rm` plus
  a bind mount): **729 passed, 0 failed** (MNT gate included).
- `tools/smoke.sh --mb2` and `--uefi`: both PASS, with no `[EXC]`.

## Files

`fs/vfs.{c,h}`, `mm/pagecache.{c,h}`, `fs/fat32.c`, `fs/ext2.c`, `fs/exfat.c`,
`fs/ramfs_vfs.c`, `kernel/syscall.c`, `tools/selftest.sh`, `TASKS.md`,
`docs/devlog/M41-exfat.md`.
