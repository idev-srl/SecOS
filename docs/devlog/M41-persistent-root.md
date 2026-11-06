# M41 — Persistent ext4 root by default (Phase 3, item 1)

Before M41 every `make run*` booted on a RAMFS root that the kernel filled from
the programs embedded in its image, so a reboot lost everything: files,
`/root/.ssh`, the SSH host key, `/etc` edits, the keyboard layout. The pieces
for a disk root existed (`ext2_mount_root` adopts an ext2/ext4 volume carrying
`/.secosroot`, M23/M33; JBD2 replay M27a and write-side journaling M27b), but
nothing used them by default, and they had not been exercised on a full-size
volume over many boots.

M41 makes a persistent ext4 system disk the normal way to run SecOS in QEMU.
The no-disk RAMFS boot is unchanged (CI and the smoke tests use it).

## Quick start

```
make run-gui                      # or run-serial, run-ssh, run, run-vnc, run-uefi-disk
make run-gui ROOT=ram             # no disk: RAMFS root, nothing persists
make reset-root                   # DELETE secos-root.img (lists what is on it) and make a new one
```

The first `make run*` creates `secos-root.img` (1 GiB sparse, about 35 MB on
the host at first). Later builds never touch it. `ROOT_IMG=` and `ROOT_SIZE=`
choose another file or size (when it is created).

## Design decisions

### Disk format and journal

`tools/mkroot.sh` runs a plain `mkfs.ext4` (the host's default ext4 layout:
`has_journal extent 64bit flex_bg metadata_csum dir_index huge_file dir_nlink
extra_isize`, 4 KiB blocks, 8 block groups at 1 GiB), label `SECOSROOT`, a
small FHS skeleton owned by root (`debugfs sif`, because `mkfs -d` copies the
build user's uid) and the `/.secosroot` marker. It then turns on **journal
checksums v3** (`debugfs jo -c`), which Linux enables itself on a
metadata_csum volume at its first mount.

So the journal *is* written (crash safety) *and* metadata checksums are kept.
That needed work in `fs/ext2.c`: before M41 `jt_init` refused any checksummed
journal and silently fell back to unjournalled writes, and the plain writer
had a latent bug: it always wrote 8-byte tags, also into a journal with the
64-bit feature (12-byte tags), which Linux turns on when it mounts a 64-bit
volume. Now the writer produces:

- checksum-v3 tags (`journal_block_tag3_t`, crc32c of the sequence number and
  the block as stored), the descriptor-block tail checksum, the commit-block
  checksum (`h_chksum[0]`) and the commit time, and the journal-superblock
  checksum on every publish/retire;
- 12-byte tags for a 64-bit journal without checksums, 8-byte tags otherwise;
- escaped blocks (a block whose first word is the JBD2 magic is stored with a
  zero first word and `JBD2_FLAG_ESCAPE`).

`EXT4_FEATURE_INCOMPAT_RECOVER` is set before the first transaction after a
mount or a `sync` and cleared by `sync` (and therefore by `poweroff`/`reboot`,
whose final phase syncs every filesystem): e2fsck and Linux only replay a
journal when this flag is set. Recovery (M27a) now refreshes the superblock
and journal-superblock checksums when it marks the journal clean.

Validated against the host tools: a transaction cut after publish
(`-DM27B_CRASH=2`) is replayed by **host `e2fsck -fy`**, which verifies every
v3 checksum (`newf` present, then `e2fsck -fn` clean); SecOS replays the same
journal identically. Cut before the commit block (`=1`): the operation is
absent on both sides.

### Other fixes a long-lived root needed (all in `fs/ext2.c`)

- **Uninitialised block groups.** On a metadata_csum volume of more than one
  group, mke2fs marks the empty groups `BLOCK_UNINIT`/`INODE_UNINIT` and does
  not write their bitmaps. The allocator read the never-written (zero) block
  bitmap as "all free", and `gd_finalize_csum` cleared the UNINIT flag on any
  group-descriptor write, so the first allocation in group 1 could hand out
  the backup superblock. The allocator now builds the bitmap like
  `ext4_init_block_bitmap` (superblock + GDT + reserved-GDT backup, the
  metadata of every group placed in this one, padding past the end of the
  filesystem) and clears the flag itself; `gd_finalize_csum` leaves UNINIT
  groups alone. M33 only ever tested a one-group (64 MB) volume.
- **Recovery reverted the superblock.** `jbd2_recover` cleared
  `NEEDS_RECOVERY` by writing back the superblock it had read *before* the
  replay; when the replayed transaction carried the superblock (free counts),
  that undid it, and host e2fsck reported `Free blocks count wrong` after a
  hard kill. It now re-reads the superblock after the replay. Latent since
  M27a (SecOS never set the flag before); found by the crash runs.
- **Extent trees of depth 1.** New files had a 4-entry inline extent tree and
  a write that needed a fifth extent failed. Fragmentation is normal on a
  root where programs are replaced at every upgrade. The fifth extent now
  moves the entries into a leaf block (340 extents at 4 KiB, with the
  metadata_csum extent tail) under a depth-1 root, up to 4 leaves. Free and
  truncate handle depth 1.
- **Directories made by mkfs** use extents and could not grow past one block
  (`bmap_alloc` refuses extent inodes); `/bin` holds about 160 entries.
  `dir_add` now maps through `block_for_write`. An htree (`dir_index`)
  directory, which only Linux or `e2fsck -D` create, drops its index flag
  before an insert (every htree directory is also a valid linear one).
- **Superblock validation before any write:** a feature whitelist (INCOMPAT
  and RO_COMPAT bits this driver really handles; e.g. `inline_data`,
  `meta_bg`, `bigalloc`, `quota`, crc16 `uninit_bg` are refused), geometry
  sanity, the metadata_csum superblock checksum, the "has errors" state,
  filesystem larger than the device. `ext2_mount_root()` now returns
  -1 (no ext fs), -2 (ext but no marker: a data disk), -3 (refused;
  `ext2_last_error()` says why). A corrupt journal is a refusal too.
- One ext volume at a time (the driver state is global): a second
  `ext2_mount` while one is in the VFS is refused instead of retargeting it.
- The allocator skips full groups and full bitmap bytes.

### Bus

virtio-blk (`vda`). It is the one bus that works unchanged on the MB2/ISO path
(i440fx; the run targets pass `-boot d` so SeaBIOS boots the CD and never
tries the data disk, the CLAUDE.md gotcha) and on the UEFI path (q35/OVMF:
OVMF ignores a disk without an ESP and boots `secos-uefi.img`). With virtio
present the kernel does not probe AHCI/NVMe; the boot disk is not needed after
the loader.

### Population and upgrade policy (`kernel/sysroot.c`)

The install code that was spread through `kernel.c` is now one table,
reconciled at every boot:

| Kind | Examples | Persistent root | RAMFS root |
|---|---|---|---|
| system program | `/bin/bash`, `/bin/busybox`, `/bin/init`, `/bin/secwm`, `/bin/dropbearmulti`, ... (24) | rewritten only when it changed | created |
| alias | BusyBox applets, coreutils names, `reboot`/`halt`/`shutdown`, `ifup`/`ifdown`/`ifconfig`, `startgui`, `script`, `dropbear`/`dropbearkey`/`dbclient` (134) | relative symlink (`ls -> busybox`) | copy (BusyBox: the re-exec stub); the dropbear names are skipped |
| configuration | `/etc/inittab`, `/etc/passwd`, `/etc/group`, `/etc/bash.bashrc`, `/etc/inputrc` | created only when missing | created |
| release stamp | `/etc/secos-release`, `/VERSION` | rewritten when the build changes | created |
| baked SSH keys | `/root/.ssh/authorized_keys` | each missing line appended | created |

Never touched: `/root`, `/home`, `/var` (except `/var/lib/secos/manifest`),
`/tmp`, `/etc/dropbear` (the SSH host key), and any file not in the table.

"Changed" is decided without reading the programs back: the
`/var/lib/secos/manifest` file records, for each program the kernel wrote, the
SHA-256 of what was written plus the file's size and mtime right after. A
program is rewritten when the SHA-256 of the embedded copy differs (an
upgraded kernel), or the file's size/mtime differ from the record (someone
replaced or truncated it). Rewrites go to `/bin/.<name>.sysroot-new` and are
renamed over the old file, one journalled transaction, so a crash leaves the
old or the new program, never half of one. A program listed in the old
manifest but no longer shipped is removed, unless it was modified since. The
manifest itself is saved the same way, only when it changed.

Aliases are symlinks on disk, so upgrading BusyBox upgrades every applet at
once, and a hard link would keep pointing at the old inode after the rename.
`SYS_SPAWN` now follows symlinks (it used `vfs_read_all`, which does not;
`execve` already did); `argv[0]` keeps the name used, which is what the
multi-call programs dispatch on. The RAMFS keeps the copies/stubs: it has no
symlinks.

Measured under TCG: first boot on a fresh disk writes 24 programs and 134
symlinks in about 6 s; an unchanged boot checks all 158 entries in about
0.45 s and writes nothing; an upgraded kernel with one changed program writes
exactly that one.

`/etc/secos-release` (`NAME`, `VERSION`, `BUILD_TS`, `GIT_HASH`) is the version
stamp. The default inittab now always contains `gui:once:/bin/startgui
--auto` (see below), so it has 4 entries.

### Settings

- **Keyboard layout:** `loadkeys it` saves `KEYMAP=it` in
  `/etc/vconsole.conf` (other lines kept; new file + rename); `loadkeys -n it`
  does not save. The kernel applies the file at boot
  (`sysroot_apply_settings`, `[KEYMAP] /etc/vconsole.conf: KEYMAP=it
  (applied)`). `make KEYMAP=it` is only the default while the file is missing.
  The kernel shell's own `loadkeys` stays runtime-only (it is the recovery
  shell).
- **GUI at boot:** `/etc/gui-autostart` holds `1` or `0` (yes/no, on/off;
  empty = on). Without the file, `startgui --auto` follows the build default,
  which the kernel publishes on `/proc/cmdline` as `secos.gui_autostart=0|1`
  (`make GUI_AUTOSTART=1`). `/proc/cmdline` also shows `root=` (`ramfs` or
  `/dev/vda`) and `secos.keymap=` when a build default is set.

### Health at boot

```
[ROOT] persistent root: ext4 on vda (journal on, metadata_csum), 918 MiB free of 1024 MiB
```

(debugcon: `[ROOT] persistent root fs=ext4 dev=vda journal=on csum=on
free_mib=... total_mib=...`). A root whose journal cannot be written gets
`[ROOT] WARNING: writes to this root are not crash-safe`. On RAMFS:
`[ROOT] root: RAMFS (no system disk: changes are lost at reboot)`.

A disk that is present but unusable is refused before anything is written:
bad superblock checksum, unsupported features, the errors flag, a corrupt
journal, or no primary superblock while a backup superblock labelled
`SECOSROOT` exists (a damaged system disk rather than a blank one). The boot
then prints a framed warning on the console, logs `[ROOT] WARNING: system disk
vda refused: <reason> -> RAMFS fallback, disk untouched`, does not mount the
disk at `/mnt` either, and continues on RAMFS. Nothing ever formats a disk:
only `tools/mkroot.sh` creates one, and only when the file does not exist.

## Tests

`tools/persist_test.sh` (run by `tools/selftest.sh` as the "M41 PERSISTENT
ROOT GATE"; private `mktemp` directory, host port 24000-24999):

1. image A on a fresh disk: write `/root/notes.txt` and `/home/alice/f.txt`,
   `loadkeys it`, append a hand-made SSH key, edit `/etc/inittab`, fetch the
   host key with `ssh-keyscan`, `poweroff`: QEMU exits by itself, e2fsck clean;
2. image A again: `written=0`, files, keymap (applied from vconsole.conf),
   both keys and the inittab edit are there; host key file byte-identical and
   the same fingerprint;
3. image B (`-DSECOS_SYSROOT_TEST=2` changes `/bin/sysroot_probe` from `hello`
   to `m16_child`, and bakes one more key): exactly `/bin/sysroot_probe` is
   rewritten and runs, the new key is appended (3 lines), user key, files,
   `/etc` edit, keymap and host key unchanged;
4. hard kills: a bash loop rewrites small files and copies `/bin/lua`
   (230 KB) over and over; QEMU is killed with SIGKILL 3–10 s in. After each
   kill the host runs `e2fsck -fy` on a copy (it replays the journal, checking
   the v3 checksums) and then `e2fsck -fn` must be clean; SecOS then mounts
   the disk, replays when the journal holds a transaction, and shuts down
   e2fsck-clean;
5. a disk with a broken superblock checksum and one with a zeroed primary
   superblock magic: RAMFS fallback with the warning, a working shell, and the
   image byte-identical afterwards (md5).

Results (QEMU TCG, 2026-11-06): the gate passed 27/27 in a dev run with 6
hard kills (2 of them landed inside a published transaction and were replayed
by SecOS; host e2fsck found nothing to fix after any kill) and 27/27 inside the
full `tools/selftest.sh` (3 kills). Full selftest: **698 passed, 0 failed**.
`tools/smoke.sh --mb2` and `--uefi` (RAMFS path) pass. The first dev run found
the stale-superblock recovery bug above (one kill: `Free blocks count wrong`)
and a wrong backup-superblock probe sector; both fixed and the checks made
stricter (any e2fsck message besides the pass headers fails).

Also validated by hand: the `-DM27B_CRASH=1|2` fault-injection images on a
mkroot-style 4 KiB / 64-bit / checksum-v3 volume (cut before commit: absent;
cut after publish: host `e2fsck -fy` and SecOS both replay it, `newf`
present, e2fsck clean), and the UEFI path (q35 + OVMF, root disk on virtio):
first boot provisions, second writes nothing, poweroff leaves the disk clean.

## Known gaps

- No block-layer flush/FUA: virtio-blk writes are synchronous and QEMU keeps
  them across a `kill -9`, which is what the tests exercise. A real power cut
  with a volatile disk cache could reorder the journal writes; issuing
  `VIRTIO_BLK_T_FLUSH` (and the AHCI/NVMe equivalents) at commit is the next
  step for real hardware.
- File data is not journalled (ordered mode, as ext4's default): a crash
  during an in-place overwrite can leave mixed old/new data in that file.
  System programs are always replaced by rename, so they are never torn.
- A transaction holds at most 64 metadata blocks (fewer on 1 KiB volumes);
  beyond that the extra blocks are written unjournalled and logged
  (`[EXT2] WARN: transaction full`). SecOS operations stay far below it.
- Extent trees deeper than one level are read but not extended (a file would
  need more than 1360 extents).
- Dropping an htree index on a metadata_csum volume leaves the old index
  blocks without a dirent checksum tail, which e2fsck reports. Only
  directories Linux indexed are affected; SecOS never creates one.
- Unsupported journal formats (v1/v2 checksums, async commit, fast commits)
  mount with direct writes and a warning instead of being refused.
- The ISO on real hardware still boots on RAMFS unless a disk carrying
  `/.secosroot` is present; there is no installer that partitions a real
  disk.
- `tools/mkroot.sh` uses the host's mkfs.ext4 defaults; a host whose
  `mke2fs.conf` adds an unsupported feature gets a disk the kernel refuses
  (loudly) rather than a corrupted one.
