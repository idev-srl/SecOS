# M42 — NTFS read-only

Branch `feat/ntfs` (from `feat/exfat` at `5a3cdbe`). Phase 3 item 3b, first
step: read files from NTFS volumes, i.e. Windows disks and USB drives.
Read-write is a later step. The driver (`fs/ntfs.c`) was written from public
format documentation (see *References*), and no GPL code was used or ported.

```
bash-5.2# mount -t ntfs /dev/vda /x
bash-5.2# cat /x/Relazione*
Il documento è pronto per la revisione.
bash-5.2# echo hi > /x/new.txt
bash: /x/new.txt: Read-only file system
bash-5.2# ls /x/many | wc -l
2000
bash-5.2# grep ntfs /proc/mounts
/dev/vda /x ntfs ro,relatime 0 0
```

## What landed

### Driver (`fs/ntfs.{c,h}`)

- **Boot sector.** Validation covers:
  - the OEM id and the 0x55AA signature;
  - bytes/sector 512..4096, no smaller than the device sector;
  - the cluster size, including the "negative" encoding for clusters larger
    than 64 KiB, up to 2 MiB;
  - the FAT-BPB fields NTFS requires to be zero;
  - the volume fitting the device;
  - `$MFT`/`$MFTMirr` LCNs inside the volume;
  - MFT record size 1..4 KiB and index block size 512 B..64 KiB.

  If the main sector fails, the **backup boot sector** (the last sector of
  the volume) is tried.
- **`$MFT` bootstrap.** Record 0 is read directly. Its `$DATA` run list is
  decoded first from the base record, which reaches the extension records,
  then through its `$ATTRIBUTE_LIST`, so a fragmented `$MFT` with extension
  records is handled. **`$MFTMirr`:** records 0..3 are read from the mirror
  and validated.
  - A primary record that fails its fixups or structure checks is served from
    the mirror copy.
  - Two valid copies that differ are logged and the `$MFT` copy is used.
  - If record 0 is valid but its `$MFT` run list cannot be used, the mirror's
    record 0 is tried.
- **MFT records.** The update sequence array is applied: the count must equal
  size/512 + 1, the array must lie in the first stride, and every stride must
  end with the USN. Then the header is checked:
  - bytes-allocated equals the record size and bytes-in-use fits;
  - the first attribute comes after the array and is 8-aligned;
  - on NTFS 3.1 the record's own number matches.

  Records are cached (32 per volume).
- **Attributes.** Every header is bounds-checked: length ≥ 0x18, 8-aligned and
  inside bytes-in-use; the name inside the attribute; resident values inside
  the attribute; non-resident headers ≥ 0x40, with the mapping pairs inside;
  and initialized ≤ data ≤ allocated.
- **Run lists.**
  - Each length and offset field is 1..8 bytes, and every run inside the
    attribute.
  - Lengths are positive, offsets sign-extended, and the LCN delta is bounded,
    so there is no overflow.
  - Every LCN lies inside the volume, and the VCN space is limited to 2^62
    bytes.
  - Each extent must start where the previous one ended and end at its
    HighestVcn + 1.
  - At most 2^18 runs per attribute.

  Sparse runs read as zeros, as do bytes past the initialized size.
- **`$ATTRIBUTE_LIST`** (resident or not, ≤ 256 KiB) is followed for every
  attribute:
  - extension records must be in use and point back to the base record, with
    a matching sequence number;
  - the attribute is matched by type, name and instance;
  - extents are chained in VCN order with the same contiguity check.

  The list is never followed recursively, so it cannot loop.
- **Names.**
  - `$FILE_NAME` keys are UTF-16, converted to and from UTF-8, with surrogate
    pairs. Unpaired surrogates use a 3-byte encoding so the name still
    round-trips.
  - Lookups are **case-insensitive through the volume's `$UpCase`**: it must
    be 128 KiB and map ASCII as every Windows table does, or the mount is
    refused. A case-exact match is preferred.
  - **DOS 8.3 names** (namespace 2) are neither listed nor looked up.
    Win32+DOS and POSIX names are.
  - Names containing NUL or `/`, and `.`/`..`, are skipped.
  - **System metafiles are hidden:** records below 24 are left out of
    listings and refused by lookup. That covers `$MFT`, `$Volume`, `$Extend`
    and the root's `.` entry, and since `$Extend` is unreachable, its
    children too.
- **Directories.** `$INDEX_ROOT:$I30` must be resident, index `$FILE_NAME`,
  use the FILENAME collation, and have the volume's index block size.
  - Index blocks come from `$INDEX_ALLOCATION:$I30`. Each block's `$BITMAP`
    bit must be set; the block is fixed up (`INDX`) and its VCN field must
    match.
  - Index headers and entries are bounds-checked, and an entry missing its
    LAST terminator is an error.
  - **Lookup** descends the B+ tree on the upcased collation. Equal keys may
    sit in a left subtree, so the search walks it. It is depth-limited (16)
    and reads at most 4096 blocks.
  - **readdir** walks the whole tree in order: depth-limited, with a
    per-call visited bitmap, so a block reached twice (a loop) is an error,
    not a hang. Its buffers are per call, which keeps it safe if the callback
    re-enters the driver.
- **Data.** Unnamed `$DATA` is read resident or non-resident. Aligned spans
  go straight to the caller's buffer (up to 128 sectors per request).
  **Sparse** files read holes as zero. **LZNT1-compressed** files are fully
  supported ([MS-XCA] 2.5):
  - Each 16-cluster unit is classified from its run mapping: all sparse means
    zeros, fully allocated means stored raw, otherwise it is compressed.
    Allocated clusters after a hole are an error.
  - Compressed chunks are bounds-checked (displacement ≤ position, no copy
    across the 4 KiB chunk, no read past the input).
  - Short chunks are zero-padded, and the last decoded unit is cached.
  - Compression with clusters larger than 4 KiB, or a format other than
    LZNT1, gives -EIO.

  Named streams (alternate data streams) are ignored: the unnamed stream is
  what is read.
- **Metadata.** `$STANDARD_INFORMATION` times give mtime, atime and ctime (MFT
  change), converted from NT time to Unix. The size comes from `$DATA`, nlink
  from the non-DOS names in the base record, and the ReadOnly attribute gives
  mode 0444. The type is file, dir or symlink, and statfs counts free
  clusters from `$Bitmap` (counted once, lazily).
- **Reparse points.** Choice: links are **listed and resolved as VFS
  symlinks**. Other reparse-backed files are listed but fail to read.
  - `IO_REPARSE_TAG_SYMLINK`: a relative target keeps its path (`\` → `/`). An
    absolute `X:\path` target is mapped under the mount point
    (`<mp>/path`), on the assumption that the target is on the same volume.
  - Junctions (`IO_REPARSE_TAG_MOUNT_POINT`, `\??\C:\…`) are mapped the same
    way.
  - WSL symlinks (`IO_REPARSE_TAG_LX_SYMLINK`, UTF-8 target) and Interix
    symlinks (system file holding `IntxLNK\1`, as older ntfs-3g writes them)
    become symlinks too.
  - Any other tag on a file (WOF/compact OS, dedup, OneDrive placeholders, WSL
    special files) is listed but read returns **-EIO**: its unnamed `$DATA`
    is only a stub, and returning it would silently give wrong content.
    Directories with other tags list normally.
- **EFS.** A file whose `$STANDARD_INFORMATION` or `$DATA` carries the
  encrypted flag returns **-EACCES** from read.
- **Read-only.**
  - Every mutating op returns **-EROFS**: write, create, mkdir, remove,
    rename, truncate, setattr, symlink and link.
  - `ntfs_mount_opts(dev, mp, want_rw=1)` is refused with -EROFS.
  - statfs reports `ST_RDONLY`, and the ops set the new `VFS_FS_RDONLY` flag.
- **Dirty / hibernated volumes.** The `$Volume` dirty flag, and a
  `hiberfil.sys` starting with `HIBR` (Windows hibernation or Fast Startup),
  are reported as warnings. The volume is still mounted, read-only as always.
- **Instances.** Up to **four NTFS volumes at once**. The VFS ops carry no
  context pointer, so each instance has its own ops table built from
  per-instance wrappers.
- **VFS inodes.** One slot per MFT base record, found through a hash. Slots
  are never recycled while mounted (as in ext2/exFAT) and carry a mount
  generation, so an inode kept across an unmount gets errors, not another
  file's data. The ops set `VFS_FS_NOCACHE`: the unified page cache
  zero-fills when `->read` fails, which would turn -EIO/-EACCES into silent
  zeros. The driver keeps its own record, index-block and compression-unit
  caches instead.

### Kernel / VFS changes outside the driver

- `fs/vfs.h`: `VFS_FS_RDONLY`.
- `kernel/syscall.c`, `open()` on such a filesystem:
  - write access or `O_TRUNC` gives **-EROFS**;
  - `O_CREAT` of a missing file returns the create's -EROFS instead of
    ENOENT.
- `fs/vfs.c`: `rmdir`/`rename`/`link` pass an -EROFS from the filesystem
  through instead of mapping it to -EIO.
- `fs/procfs.c`: `/proc/mounts` shows `ro` for such mounts, and
  `/proc/filesystems` lists `ntfs`.
- `kernel/syscall.c` `ksys_mount`: fstype `ntfs` (also `ntfs3`, `ntfs-3g`),
  and `auto` tries NTFS after exFAT. Only fstype dispatch lines were added;
  the stack-buffer mount point is left to `fix/mount-hardening`, and the
  driver copies the mount point itself.
- `kernel/syscall.c` `ksys_getdents`: the kernel buffer now follows the
  caller's size up to 1 MiB (it was a static 64 KiB, which silently cut every
  directory at 256 entries). BusyBox's `opendir` already grows its buffer, so
  `ls` of the 2000-entry directory (or a Windows `System32`) lists all of
  them.
- `kernel/kernel.c`: the boot `/mnt` probe tries NTFS last, plus the
  `M42_NTFS_DEMO` hook. `kernel/shell.c`: `mountdev` tries NTFS.

## Host tooling (no sudo)

`tools/ntfs-host.sh` drives the real NTFS tools, independent of the SecOS
driver:

- `mkntfs` and `ntfs-3g` come from PATH, or `apt-get download ntfs-3g
  libntfs-3g89` into `~/.local/secos-ntfs` when they are missing.
- Files go onto the image through **ntfs-3g inside `unshare -rm`**. ntfs-3g
  drops supplementary groups with `setgroups()`, which a user namespace
  forbids (`priv drop: setgroups failed`). A two-line `LD_PRELOAD` shim
  (`nosetgroups.so`, built by `setup`) makes that call a no-op, and the mount
  works unprivileged.
- `mkgood IMG` (`make disk-ntfs`) builds a 96 MiB image containing:
  - long Unicode, CJK, emoji and 176-character names;
  - 7-level nested dirs;
  - a Win32+DOS name pair (`system.ntfs_dos_name`) and a hard link;
  - a 48 MiB sparse file with three data islands;
  - a directory with the compressed attribute holding text, mixed
    (text / random / zeros) and tiny files, all LZNT1;
  - a file with an alternate data stream;
  - a Windows relative symlink, an absolute symlink, a junction and a WSL
    symlink (all through `system.ntfs_reparse_data`), plus an Interix symlink
    (`ln -s`);
  - an EFS-flagged file, patched in place;
  - a **2000-entry directory** whose index has internal nodes (checked);
  - a **5 MiB file in ~1300 runs**: the volume is filled with 4 KiB files and
    ballast, then every other 4 KiB file is deleted, so the file goes into
    the holes. That is too many runs for one record, so it gets an
    **`$ATTRIBUTE_LIST`** and extension records (checked);
  - a 5 MiB contiguous file.

  ntfs-3g's listing of the image (type, size, FNV-1a of the content, symlink
  target) is saved as `disk.img.list`, the reference.
- `mkcorrupt DIR` (`make disk-ntfs-corrupt`) builds a 16 MiB base and derives
  eight damaged images with `tools/ntfs_img.py` (record edits are fixup-aware):

  | image      | damage | expected |
  |------------|--------|----------|
  | `files`    | `runbad.bin` LCN delta far outside the volume; `attrbad.bin` `$DATA` length 0x7FF8; `c.txt` broken LZNT1 chunk header; `secret.efs` EFS flag; `loopdir` index entry whose child is its own block; `indxbad` torn INDX sector; `$Volume` dirty; `hiberfil.sys` = `HIBR` | mounts (warnings); per-object EIO / refused / EACCES / readdir error |
  | `bootmain` | main boot sector OEM destroyed | mounted from the backup boot sector |
  | `bootboth` | main + backup destroyed | refused |
  | `mft0`     | `$MFT` record 0 torn (USN) | mounted using the `$MFTMirr` copy |
  | `mftboth`  | record 0 torn in `$MFT` and `$MFTMirr` | refused |
  | `upcase`   | `$UpCase['a']` = 'a' | refused |
  | `rootbad`  | root `$INDEX_ROOT` collation rule changed | refused |
  | `geo`      | sectors/cluster = 3 in both boot sectors | refused |

- `mkfuzz IMG` builds a 32 MiB full-featured image as a faster fuzz base.

### Host harness (`tools/ntfs_fuzz/`)

`fs/ntfs.c` is compiled **unchanged** against stub block/VFS layers, with
ASan + UBSan (`-fno-sanitize-recover`).

- `ntfs_fuzz IMG list` prints the listing in the reference format. That gives
  a host-side diff against ntfs-3g before QEMU is even involved.
- Fuzz mode pre-scans the base for FILE records, INDX blocks and
  "interesting" records: attribute lists, extension records, index
  allocations, reparse points, compressed or sparse data. It also collects the
  compressed clusters. Each iteration mutates 1–16 bytes (random, flipped
  bit, ±1, 0x00/0xFF) in one of eleven regions:
  - boot fields;
  - one record;
  - one INDX block;
  - attribute headers and run lists;
  - the metafiles;
  - index headers;
  - interesting records and their runs;
  - LZNT1 data;
  - anywhere.

  It then mounts twice, walks everything, reads every file (also at odd
  offsets and past EOF), follows symlinks, calls statfs and tries every write
  op. A hang (SIGALRM, 20 s) or a sanitizer report fails with the seed.

## Testing

QEMU plus the host harness only.

- **Host listing:** the driver built for the host lists the 96 MiB image
  **identically to ntfs-3g**: 3537 objects, 3518 files with matching sizes
  and content hashes, and the five symlink kinds resolving to the expected
  targets.
- **In-kernel suite** (`fs/ntfs_test.c`, gate `M42_NTFS_DEMO`, virtio): the
  kernel walks the whole volume and prints an `[M42L]` line per object. Its
  listing **matches ntfs-3g's** (3537 objects), and **37/37 targeted checks
  pass**:
  - names: Unicode/CJK names, case-insensitive ASCII and non-ASCII lookups,
    DOS alias and metafiles hidden, all 2000 big-directory entries plus
    lookups at both ends;
  - data: the attribute-list file and the big file against the pattern, the
    sparse file (holes are zero), LZNT1 reads at odd offsets, resident and
    empty files, the ADS file's main stream, EFS → -13;
  - metadata: plausible times, nlink = 2, the four symlink kinds, `stat`
    following a symlink;
  - read-only: **all ten write ops → -30** with the content unchanged, and an
    rw mount request → -30;
  - integration: `/proc/mounts` `ro`, `/proc/filesystems`, statfs, a
    **second simultaneous instance**, unmount + a stale inode erroring
    cleanly, remount.
- **Corrupt suite** (q35: AHCI ×4 + NVMe + USB mass storage): all six attached
  images behave as in the table above, and `[M42C] DONE (kernel alive)` is
  printed with no `[EXC]`/`[PANIC]`. `bootboth` and `geo` are checked by the
  host harness, because SecOS registers at most four AHCI disks and skips
  AHCI/NVMe when virtio is present.
- **Userland** over the serial bash (default image), checked in the selftest:
  - BusyBox `mount -t ntfs /dev/vda /x`;
  - `/proc/mounts` shows `ntfs ro`, and `df` lists the mount;
  - `cat` of a long Unicode name works;
  - `echo > /x/new.txt`, `ln -s` and `rm` all fail with "Read-only file
    system"; `cat secret.efs` fails with "Permission denied";
  - `ls /x/many | wc -l` = 2000;
  - `md5sum` of the fragmented and the compressed file equals the host's;
  - `ls -l` shows the Windows symlink's target, `cat` follows it, and a path
    through the junction resolves;
  - `umount` drops the mount.
- **Fuzzing (ASan + UBSan), no crash, sanitizer report or hang:**
  - 20 000 images from the 96 MiB test image;
  - 20 000 from the 32 MiB fuzz base;
  - 20 000 from the 16 MiB corrupt base;
  - 3 000 + 2 × 50 + 100 + 10 during development.

  That is about 63 000 mutated images.
- `tools/selftest.sh` gained the **M42 gate: 58 checks**: the good suite,
  the listing comparison with ntfs-3g, the corrupt suite, the host harness on
  two more images, a 3400-image fuzz and the userland mount.
- `tools/smoke.sh --mb2` and `--uefi` pass.

## References

All public documentation; no source code was copied or ported.

- Linux-NTFS project, *NTFS Documentation* (Richard Russon, Yuval Fledel), the
  "flatcap" pages (`flatcap.github.io/linux-ntfs/ntfs/`). Used for the boot
  sector, MFT record header and update sequence array, attribute header
  layouts, run lists (mapping pairs), `$ATTRIBUTE_LIST`, `$STANDARD_INFORMATION`,
  `$FILE_NAME` and namespaces, `$INDEX_ROOT`/`$INDEX_ALLOCATION`/`$BITMAP`,
  index entries/INDX records, `$UpCase`, `$Volume` flags, `$MFTMirr`.
- Microsoft, *[MS-XCA]: Xpress Compression Algorithm*, section 2.5 "LZNT1
  Algorithm Details": chunk header, flag groups, the position-dependent
  displacement/length split.
- Microsoft, *[MS-FSCC]: File System Control Codes*, sections 2.1.2.1
  (reparse tags), 2.1.2.4 (symbolic link reparse data buffer, relative flag)
  and 2.1.2.5 (mount point reparse data buffer).
- The behaviour of the host's ntfs-3g/mkntfs was used as a black-box oracle
  (the reference listing), not as source.

## Ownership and modes (Phase 3 integration with multi-user)

NTFS (whose ACLs are not mapped) stores no Unix owner or mode, and since `feat/multiuser-base` the VFS
enforces permissions, so the mount supplies them, as Linux ntfs3 does
(`fs/mntopts.h`, shared by FAT32, exFAT and NTFS):

- default: every object is `root:root`, directories `0755`, files `0644`
  (no exec bits: programs are not run from these volumes). Any user can read a
  mounted USB stick or data disk; only root writes it.
- mount(2) data `uid=N,gid=N,umask=OOO,dmask=OOO,fmask=OOO` change that per
  mount (`umask` sets both masks, so `umask=027` gives `0750` files and
  directories, as on Linux), e.g. `mount -t ntfs -o uid=1000,gid=100 /dev/usb0 /media`.
  A malformed or unknown option refuses the mount with `EINVAL`; options never
  leak into the next mount.
- a file with the FS read-only attribute (`FILE_ATTRIBUTE_READONLY`) loses its write bits.
- options are per volume (up to 4 NTFS volumes); an explicit `rw` fails with
  `EROFS` like any read-write request; symlinks/junctions stay `0777`.
- writes to a read-only mount fail with `EROFS` before any permission
  check, root included (`kernel/cred.c`), as Linux `inode_permission()`.
- BusyBox `mount -o ...` forwards its data string (and `-r` as `ro`) to
  `SYS_MOUNT` (`user/busybox_compat.c`); this takes effect with the next
  `make port-busybox`.

## Known gaps

- **Read-only.** Write support is the next step (see `TASKS.md`).
- `SYS_MOUNT` has no flags argument, so every NTFS mount is read-only and an
  explicit `-o rw` cannot be refused from userland. The kernel API
  (`ntfs_mount_opts`) refuses it.
- Named data streams (ADS) are not exposed; only the unnamed stream is read.
- Symlink/junction targets that are absolute are assumed to be on the same
  volume (mapped under the mount point). `\\server\share` and
  `Volume{GUID}` targets come out as unresolvable paths.
- WOF ("compact OS") files, dedup and cloud placeholders, and WSL
  FIFOs/devices read as -EIO. A Windows 10/11 system volume has many WOF
  files under `Windows\`.
- The `$LogFile` is not inspected. Whether a volume is clean is judged only
  from the `$Volume` dirty flag and `hiberfil.sys`. Read-only access does not
  replay a log; it reads the on-disk state, which may be stale for a
  hibernated or crashed Windows.
- A fragmented `$MFT` (an attribute list on record 0) is handled by the code
  but not covered by an image: mkntfs + ntfs-3g never produced one.
- 4 KiB-sector (4Kn) volumes are handled by the code (bytes/sector up to
  4096) but untested.
- Index blocks are required to have the volume's index block size, as every
  Windows and mkntfs volume does.
- Inode slots are per instance (65 536 per mount) and are reused by a remount
  of the same instance, the same convention as ext2/exFAT. The generation
  stamp rejects I/O through an inode from an older mount, but the slot memory
  is shared.
- `vfs_unmount` does not notify the driver, so an unmounted instance's caches
  and run lists stay allocated until the instance is mounted again.
- Userland compat quirks seen along the way, not NTFS-specific and not fixed
  (they live in the prebuilt BusyBox/bash images):
  - BusyBox's `mkdir` and `touch` report EROFS as "Permission denied",
    because their `busybox_compat.c` wrappers map every failure to EACCES;
  - `user/libc.c` `opendir` (bash globbing) still reads at most 256 entries.

## Files

- New: `fs/ntfs.c`, `fs/ntfs.h`, `fs/ntfs_test.c`, `tools/ntfs-host.sh`,
  `tools/ntfs_img.py`, `tools/ntfs_fuzz/{ntfs_fuzz.c,Makefile,stub/*}`,
  `docs/devlog/M42-ntfs.md`.
- Touched:
  - `fs/vfs.h` (`VFS_FS_RDONLY`) and `fs/vfs.c` (EROFS pass-through);
  - `fs/procfs.c` (`ro`, `ntfs`);
  - `kernel/syscall.c` (open EROFS, `ksys_mount` fstype, getdents buffer);
  - `kernel/kernel.c` (boot probe + gate) and `kernel/shell.c` (`mountdev`);
  - `Makefile` (sources, `disk-ntfs`, `disk-ntfs-corrupt`) and `.gitignore`;
  - `tools/selftest.sh` (M42 gate), `CLAUDE.md`, `TASKS.md`.
