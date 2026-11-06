# M41 — exFAT read-write

Branch `feat/exfat` (from `main` at `a84af35`). Phase 3 item 3a: read and
write exFAT volumes — USB sticks and SD cards formatted on Windows or macOS.
The driver (`fs/exfat.c`) was written from the public Microsoft *exFAT File
System Specification* (2019); no GPL code was used.

```
bash-5.2# mount -t exfat /dev/vda /x
bash-5.2# cat /x/Relazione*
Il documento è pronto per la revisione.
bash-5.2# df | grep /x
/dev/vda                 63488      3540     59948   6% /x
bash-5.2# umount /x          # VolumeDirty cleared, fsck.exfat-clean on the host
```

## What landed

### Driver (`fs/exfat.{c,h}`)

- **Boot region.** The main boot sector is validated: jump code, `EXFAT   `,
  MustBeZero, signature, revision 1.x, BytesPerSectorShift 9..12 and not
  smaller than the device sector, a cluster size of at most 32 MiB, one or
  two FATs. Geometry must be consistent: the FAT is big enough for
  ClusterCount, FAT and heap don't overlap, the heap fits the volume and the
  volume fits the device. The **boot checksum** over sectors 0..10
  (VolumeFlags and PercentInUse excluded) must match every word of sector 11.
  If the main region fails, the **backup region** (sectors 12..23) is tried
  for every legal sector size; a volume mounted from the backup is
  **read-only**.
- **Critical root entries.** The root directory chain is walked with a bound
  (256 MiB of directory, or ClusterCount), so a cycle is refused. The
  root must contain the **allocation bitmap**, its first cluster valid and
  its length ≥ ⌈ClusterCount/8⌉, and the **up-case table**, whose
  TableChecksum must match. The compressed table (`0xFFFF, n` identity runs)
  is expanded into a 64 Ki-entry map. The free-cluster count is taken from
  the bitmap.
- **Entry sets.** A File entry (0x85) is accepted only if SecondaryCount is
  2..18 and the set fits in the directory. The Stream Extension (0xC0) must
  come first, NameLength must be 1..255 with ⌈n/15⌉ File Name entries (0xC1),
  every other secondary must be in-use and secondary, the **SetChecksum** must
  match, and the name must contain no NUL or `/`. A set that fails is skipped
  (logged at most 32 times per mount), so the rest of the directory stays
  readable.
- **Streams.** FAT chains and **NoFatChain** (contiguous) streams share one
  cluster mapper with a cursor cache. Before the first I/O on a file its
  stream is validated: VDL ≤ DataLength ≤ heap, clusters in range, and a FAT
  chain exactly as long as DataLength requires, ending in EOC. A cycle never
  reaches EOC, so the bounded walk catches it. Clean over-allocation is
  tolerated. A stream that fails returns `-1` from read/write; it never
  loops.
- **Names.** UTF-8 path components ↔ UTF-16 (surrogate pairs; invalid UTF-8
  is refused, lone surrogates read as U+FFFD). Lookups are
  **case-insensitive** through the up-case table, filtered by the stream
  NameHash. New names are refused if they contain the characters the spec
  forbids (`< 0x20 " * / : < > ? \ |`) or are `.`/`..`.
- **Writes.**
  - `create`, `write`/extend, `truncate`, `unlink`, `mkdir`, `rmdir`
    (refused if not empty), `rename` (same directory, case-only, across
    directories, replacing an existing target), and `utimes`. `chmod`
    toggles the ReadOnly attribute; owners are ignored.
  - Allocation is next-fit on the bitmap. A file stays **NoFatChain** while
    the next cluster is free. Otherwise its run is converted to a FAT chain:
    the FAT entries are written, then the new cluster is linked. A failed
    grow rolls back the clusters it took.
  - **ValidDataLength** is honoured on read: bytes past VDL read as zero.
    Writes beyond VDL zero the gap first, and a truncate that grows a file
    zero-fills the new range on disk. Readers that ignore VDL (exfat-fuse
    1.3 does) would otherwise see the stale contents of reused clusters,
    which is a data leak.
  - Unlink marks the set not-in-use *before* freeing the clusters, so a crash
    leaks clusters rather than leaving a name pointing at free space. Rename
    inserts the new set, then deletes the old one.
  - Every changed set gets a recomputed SetChecksum. Directories grow by
    zeroed clusters, and their DataLength/VDL are updated in the parent.
- **VolumeDirty** is set in the main boot sector before the first change. It
  is cleared, and PercentInUse refreshed, by `->sync`, which runs on
  `umount`, `sync` and shutdown. A volume that was *already* dirty at mount
  stays dirty, so a checker still gets to look at it. Both fields are
  outside the boot checksum.
- **Timestamps** come from the RTC (`kclock_real_sec`), stored as UTC with a
  valid UTC-offset byte. Create, modify and access times plus the 10 ms
  fields are converted back with the offset when read.
- **VFS inodes.** One slot per entry set, keyed by location (directory first
  cluster + slot). Slots come in kmalloc'd chunks and are never recycled
  while mounted, the same rule `fs/ext2.c` adopted in M40. An open fd keeps
  working across a rename, and an fd on an unlinked file gets errors. A new
  slot drops any page-cache pages left under its pointer by an earlier
  mount.
- **I/O.** A 64-entry write-through metadata sector cache (FAT, bitmap,
  directories) sits next to direct multi-sector data transfers, up to 64
  sectors per request. Filesystem sectors larger than the device sector are
  supported, e.g. 4 KiB on 512 B.

### Integration

- `SYS_MOUNT` (`kernel/syscall.c`): fstype `exfat`, tried last for
  `auto`/empty. The device may be named `/dev/vda` or `vda`, so BusyBox
  `mount -t exfat /dev/vda /x` works.
- `SYS_UMOUNT` now **syncs the filesystem before detaching it**, which is
  what clears VolumeDirty. ext2/ext4 benefit too: any open transaction is
  closed.
- Boot (`kernel/kernel.c`): the data-disk probe for `/mnt` tries exFAT after
  FAT32 and ext2/ext4, on whole disks and partitions.
- `/proc/mounts` shows `/dev/vda /x exfat ...` (via `->mntinfo`).
  `/proc/filesystems` lists `exfat`. `statfs` reports `EXFAT_SUPER_MAGIC`
  0x2011BAB0, so `df` lists the mount.
- Kernel shell: `mountdev <dev> [mp]` also tries exFAT.

## Host tooling (no sudo)

`tools/exfat-host.sh` drives the real exFAT tools, independent of the SecOS
driver:

- `setup` (run automatically on first use) does `apt-get download exfatprogs
  exfat-fuse libfuse2` and unpacks them into `~/.local/secos-exfat`
  (`$EXFAT_TOOLS`). No root is needed.
- Files are put on and read off the image with **exfat-fuse inside an
  unprivileged user+mount namespace** (`unshare -rm`), where Linux allows
  FUSE mounts. The packaged `mount.exfat-fuse` hardcodes two root-only mount
  options: `allow_other`/`blkdev`, and `blksize=` (`fuseblk` only). `setup`
  writes a copy with same-length byte patches that turn them into harmless
  `fsname=`/`subtype=`.
- `mkgood IMG` builds the 64 MiB test disk (`make disk-exfat`):
  `hello.txt`, `Relazione è già pronta.txt`, `emoji_😀.txt`, a long-named
  subdirectory, a 3 MiB file (NoFatChain), `trunc.bin`, `delete_me.txt`,
  `rename_me.txt`, and two files appended one cluster at a time alternately
  so both fragment (40-run FAT chains).
- `mkcorrupt DIR` (`make disk-exfat-corrupt`) builds an 8 MiB base volume
  with exfat-fuse and derives six damaged images with `tools/exfat_img.py`:

  | image       | damage                                                                 | expected                         |
  |-------------|------------------------------------------------------------------------|----------------------------------|
  | `files`     | `oob.txt` FirstCluster out of range, `cycle.bin` FAT cycle, `badsum.txt` SetChecksum, `badcnt.txt` SecondaryCount=30, `loopdir/` cyclic chain | mounts; per-object errors/skip |
  | `geo`       | ClusterCount 0xFFFFFF00 (boot checksums recomputed)                   | refused (geometry)               |
  | `csum2`     | main + backup boot checksum wrong                                      | refused                          |
  | `csum1`     | main boot checksum wrong only                                          | mounted from backup, read-only   |
  | `upcase`    | up-case table byte flipped                                             | refused (TableChecksum)          |
  | `rootcycle` | root FAT entry points to itself                                        | refused                          |

- `verify IMG` runs after the SecOS run:
  - `fsck.exfat -n`, plus `exfat_img.py deepcheck`, which checks more than
    fsck.exfat 1.1.3 does. That fsck does **not** report a cluster marked
    used but owned by nothing. deepcheck requires bitmap == owned clusters
    exactly, no cross-links, every SetChecksum and NameHash correct, and
    chain lengths equal to DataLength.
  - VolumeDirty must be clear.
  - Every file is compared, mounted through exfat-fuse.

### Host fuzz harness (`tools/exfat_fuzz/`)

`fs/exfat.c` is compiled **unchanged** for the host against stub block/VFS
layers (`stub/debugcon.h`, `stub/heap.h`), with ASan + UBSan
(`-fno-sanitize-recover`). Each iteration:

- mutates 1–24 bytes in one region: boot fields (checksum recomputed 3 times
  out of 4, mirrored into the backup), the FAT head, the root directory,
  entry type/fields, or the first heap clusters;
- mounts, walks every directory, reads every file;
- runs a write workload: create, write past VDL, mkdir, rename, truncate
  shrink/grow, unlink of host files, rmdir, sync;
- mounts again and repeats.

A hang (SIGALRM, 20 s) or any sanitizer report fails with the seed. Mode `0`
runs the workload on the unmodified image and dumps it for
fsck/deepcheck.

## Testing

All in QEMU, plus the host harness.

- **In-kernel suite** (`fs/exfat_test.c`, gate `M41_EXFAT_DEMO`), good image
  on virtio: **38/38 checks OK**. It covers long-name, case-insensitive and
  emoji reads, the 3 MiB and fragmented host files, create/mkdir/rmdir
  (non-empty refused), unlink, the four rename forms, truncate shrink+grow,
  a 2 MiB write, interleaved fragmenting writes, a write past VDL, an
  in-place overwrite, directory growth (80 creates, 40 unlinks),
  `/proc/mounts`, `statfs`, VolumeDirty set and then cleared on unmount, and
  remount persistence.
- **Host verify** after that run: fsck.exfat clean, deepcheck clean (1505
  owned clusters, equal to the kernel's free count), VolumeDirty clear, all
  18 tree checks pass.
- The same suite on a **512-byte-cluster** volume (126 976 clusters, a
  31-cluster bitmap): 38/38 OK, host verify OK.
- **Corrupt suite** (q35: AHCI ×4 + NVMe + USB mass storage): all six images
  behave as in the table above; `[M41C] DONE (kernel alive)`, no `[EXC]`.
- **Userland** over the serial bash (default image), all checked from the
  host afterwards (fsck/deepcheck clean, VolumeDirty clear):
  - BusyBox `mount -t exfat /dev/vda /x`, `/proc/mounts`, `df`, `cat` of a
    long Unicode name;
  - write, `mkdir`, `mv`, `umount` (the exfat line leaves `/proc/mounts`),
    then remount and read it back.
- **Fuzzing**, with no crash, sanitizer report or hang:
  - 20 000 images from the 8 MiB base;
  - 30 000 from a 16 MiB image with the full long-name tree;
  - 1 500 each at 512 B / 32 KiB / 128 KiB / 1 MiB clusters.
  
  Clean-workload dumps are fsck- and deepcheck-clean at every cluster size.
- `tools/selftest.sh` gained the **M41 gate: 45 checks** (good suite, host
  verify, corrupt suite, 3 000-image fuzz, userland mount).
- Selftest log paths are fixed (`/tmp/secos_selftest*`). To run alongside
  another agent's selftest I ran it with a private `/tmp`: `unshare -rm`,
  then `mount --bind <dir> /tmp`, then `tools/selftest.sh`. Nested user
  namespaces are fine for the exfat-fuse step.

## Ownership and modes (Phase 3 integration with multi-user)

exFAT stores no Unix owner or mode, and since `feat/multiuser-base` the VFS
enforces permissions, so the mount supplies them, as Linux exfat does
(`fs/mntopts.h`, shared by FAT32, exFAT and NTFS):

- default: every object is `root:root`, directories `0755`, files `0644`
  (no exec bits: programs are not run from these volumes). Any user can read a
  mounted USB stick or data disk; only root writes it.
- mount(2) data `uid=N,gid=N,umask=OOO,dmask=OOO,fmask=OOO` change that per
  mount (`umask` sets both masks, so `umask=027` gives `0750` files and
  directories, as on Linux), e.g. `mount -t exfat -o uid=1000,gid=100 /dev/usb0 /media`.
  A malformed or unknown option refuses the mount with `EINVAL`; options never
  leak into the next mount.
- a file with the FS read-only attribute (`ATTR_READ_ONLY`) loses its write bits.
- `ro` mounts the volume read-only (`VFS_FS_RDONLY`, `ST_RDONLY`); chmod still maps
  the write bits onto `ATTR_READ_ONLY`, chown is accepted and ignored.
- writes to a read-only mount fail with `EROFS` before any permission
  check, root included (`kernel/cred.c`), as Linux `inode_permission()`.
- BusyBox `mount -o ...` forwards its data string (and `-r` as `ro`) to
  `SYS_MOUNT` (`user/busybox_compat.c`); this takes effect with the next
  `make port-busybox`.

## Known gaps

- **One exFAT volume at a time.** A second `mount -t exfat` fails while one
  is attached.
- **Read-only cases:** TexFAT (NumberOfFats = 2) and volumes recovered from
  the backup boot region.
- **4 KiB-sector volumes** are handled by the code (fs sector > device
  sector) but untested: mkfs.exfat 1.1.3 cannot set the sector size of an
  image file.
- **No crash journal** (exFAT has none). The orderings above limit the damage
  and VolumeDirty tells the next checker. A crash in the middle of a rename
  can leave two names for one cluster run.
- Vendor extension/allocation secondaries are kept on in-place updates but
  **dropped by rename**.
- Directories never shrink. Freed clusters' FAT entries are left as they
  were; the bitmap is the authority.
- No locking beyond the block layer (like `fs/fat32.c`); SMP is off by
  default.
- No POSIX owners or modes on exFAT: ReadOnly ↔ mode 0444, uid/gid ignored.
- Found outside this milestone and fixed on `fix/mount-hardening`
  (`docs/devlog/M41-mount-hardening.md`): `df <path>` reported `/` for every
  mount (one constant `st_dev`); `ksys_mount` handed fat32/ext2 a stack
  mount-point pointer that the VFS stored; `fs/fat32.c` served stale
  page-cache data after inode-slot reuse.

## Files

- New: `fs/exfat.c`, `fs/exfat.h`, `fs/exfat_test.c`, `tools/exfat-host.sh`,
  `tools/exfat_img.py`, `tools/exfat_fuzz/{exfat_fuzz.c,Makefile,stub/*}`.
- Touched: `Makefile` (sources, `disk-exfat`, `disk-exfat-corrupt`),
  `.gitignore`, `kernel/syscall.c` (mount/umount), `kernel/kernel.c` (boot
  probe + gate hook), `kernel/shell.c` (`mountdev`), `fs/procfs.c`
  (`/proc/filesystems`), `tools/selftest.sh` (M41 gate), `CLAUDE.md`,
  `TASKS.md`.
