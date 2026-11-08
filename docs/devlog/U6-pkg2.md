# U6 — package installer v2, transfer disk, bigger root (`feat/pkg2`)

Userland plan §3 U6, design §2 D8. Large software (the SDK, Python, the
OpenSSL tools) will not be embedded in the kernel image: it arrives as signed
packages. U6 provides the installer, the host tools that build and carry the
packages, and a root disk big enough to hold them.

## Quick start

```
# host: build a package, put it on a transfer disk, boot with it
python3 tools/secos-pkg create foo.spkg --name foo --version 1.0 \
    build/foo:/usr/bin/foo --tree share/:/usr/share/foo --conf /etc/foo.conf ...
make pkgdisk PKGS="foo.spkg bar.spkg"            # -> pkgdisk.img (exFAT)
make run-serial PKGDISK=pkgdisk.img              # attached as USB storage (usb0)

# SecOS, as root
mkdir -p /media && mount -t exfat /dev/usb0 /media
spkg verify /media/foo.spkg                      # signature + structure only
spkg install /media/foo.spkg                     # install, or upgrade in place
spkg list            spkg list foo               # packages / files of one
spkg check foo                                   # re-hash the files against the database
spkg remove foo
spkg recover                                     # finish/roll back a cut operation (init does it at boot)

# host: grow an existing system disk (SecOS shut down)
make grow-root                                   # secos-root.img -> 8 GiB
make grow-root SIZE=16G ROOT_IMG=other.img BACKUP=0
```

New disks (`make run*` when `secos-root.img` is missing, `make reset-root`)
are 8 GiB sparse: an unused gigabyte costs nothing on the host.
For VMware: `make pkgdisk-vmdk`, attach the `.vmdk` as a second disk.

## Design

### Where the signature is checked (the kernel's role)

The plan (D8) had the installer carry its own Ed25519 verify and public key.
That would have made a copy of the trust root in user space: a second place
to rotate the key, and the kernel's revocation list (M35) and the future
keyring of D9 (a machine key with a capability ceiling) would not apply to
packages. Instead the trust root stays in the kernel:

- `/bin/spkg` computes the digest itself, as a stream: SHA-256 over the whole
  file with the 64 signature bytes zeroed (the ELF/v1 rule, unchanged). A
  package can be many GiB; the kernel never sees its bytes.
- **`SYS_SIGVERIFY` (96)** `(digest32*, sig64*, key_id)` checks the signature
  over the digest against `secos_trusted_pubkey` and refuses a revoked
  `key_id`, with the same function as the ELF loader
  (`secos_sig_verify_digest`, `mm/elf_sign.c`). 0 or `-EBADMSG`.
- It needs no capability and takes no lock: it is a pure function of public
  data. A caller that wanted to lie about the digest could skip the check
  altogether; what the kernel guarantees is that "valid" means "signed by a
  key the kernel trusts and has not revoked", and that only the kernel decides
  which keys those are. spkg itself is a signed program, so the code that
  computes the digest is covered by the same root.
- Executables inside a package are still verified at every exec. The package
  signature protects what exec never checks: headers, `.a` libraries, data,
  configuration, the future Python stdlib.

Syscall number 96: the first free SecOS-specific number (92 is Phase 5's
`SYS_UDRV`, 93–94 reserved for GUI shared memory in TASKS.md; Linux-shaped
calls use 512 + n, D3). It is in the BKL-free list (`kernel/bkl.c`, D1).

### Format v2 (`tools/secos-pkg`)

```
header 256 B   "SECOSPKG", version 2, entry_count, sig_offset (= file size - 64),
               table_offset 256, data_offset, key_id, flags 0, name[64],
               version[32], data_size, reserved (zero)
entries 320 B  path[256], mode (07777), type (file/dir/symlink), data_offset,
               data_size, sha256[32] of the blob, flags (bit 0 = config file)
blobs          back to back in entry order (no gaps: one sequential stream)
signature      64 B, Ed25519 over the digest
```

Same signature format, version field 2. The host tool streams every file
through a 1 MiB buffer (packages of any size in constant memory), sorts
directories parents-first, marks files under `/etc` as configuration, keeps
the source file's executable bit (0755/0644) unless `--mode` says otherwise,
and has `--tree DIR:/prefix`, `--symlink`, `--conf`, `list`. `--v1` still
writes the M32 layout: the kernel's embedded test package is byte-identical.

### Installing (`user/spkg.c`)

1. **Pass 1, verify** — read header + table, stream the rest through SHA-256,
   read the signature, `SYS_SIGVERIFY`. Nothing has been written. Then the
   signed table is validated: layout (offsets match the file size, blobs in
   order with no gaps), paths, types, modes, duplicates, no entry below a
   symlink entry of the same package.
2. **Plan** — load every package database, find the owner of each target.
   Refused: a target owned by another package; an existing file that belongs
   to no package (outside `/etc`); a directory where a file goes; a parent
   that is not a real directory (a symlink: no write through a link); a target
   on another mount than `/var/lib/spkg` (renames must be atomic); not enough
   free space. Configuration files (`/etc`): installed when absent or when the
   existing file is this package's unmodified copy; otherwise the new version
   goes to `<path>.spkg-new` and the user's file is kept (the dpkg rule).
3. **Pass 2, stage** — read the file again into
   `/var/lib/spkg/staging/<name>.<pid>/f<i>` (0700, files 0600 until moved),
   hashing again: the digest must equal the verified one, so a package
   changed between check and use installs nothing. Then the new database
   (`staging/.../db`).
4. **Commit** — write the transaction (`/var/lib/spkg/txn.new`, fsync) and
   rename it to `/var/lib/spkg/txn`: that rename is the commit point.
5. **Apply** — new directories (parents first), renames of the staged files
   over their targets, removal of the old version's files that are not in the
   new one (a config file only if unmodified), removal of directories the old
   version created and the new one does not have (deepest first, only if
   empty), the database rename. Then the staging directory and the txn go.

Every action of the txn is idempotent (a rename whose source is gone was
done, an unlink of a missing file is done, ...), so **recovery** is "replay
the txn if there is one, then delete every staging directory": before the
commit point a crash leaves the old version (the staging files are dropped),
after it the replay completes the new version. Each single step is atomic on
the SecOS ext4 root (one journalled transaction per VFS operation, M27b/M41),
including across a power cut. Every spkg command that changes state recovers
first (under its lock); **init** runs `spkg recover` before the sysinit
entries when a txn or staging files exist (`user/init.c`), so after a crash
nothing else starts while a package is half there.

Remove is the same machinery with no staging: a txn of unlinks/rmdirs and
the database unlink.

Restrictions: paths only below `/usr`, `/opt`, `/etc` (so `/usr/share`), no
`.`/`..`/empty components, no spaces or control bytes, < 256 bytes; no
setuid/setgid bits (directories keep the sticky bit); every file root:root.
A lock file (`/var/lib/spkg/lock`, owner pid, stale when that pid is gone)
serializes spkg runs. `install`, `remove`, `recover` need root; `list`,
`verify`, `check` do not.

**Owned-file database**: `/var/lib/spkg/db/<name>.list`, text, one object
per line with the path last: `d <mode> <created> <path>`, `f <mode> <size>
<sha256> <path>`, `c ...` (configuration), `l <sha256-of-target> <path>`;
plus name, version, package digest and install time. `created` says whether
the package made the directory (only those are removed with it).

spkg's manifest (`user/note_spkg.S`) is confined: `CAP_ENFORCE |
FS_READ | FS_WRITE | SIGNAL | TIME` (no spawn, sockets, IPC; no_new_privs).
Fetching packages from a URL needs `CAP_NET` and waits for wget (U11). It is
built on the current libc (`make pkg-progs`); U7 moves it to musl.

### Transfer disk

`make pkgdisk PKGS="..."` (`tools/mkpkgdisk.sh`) verifies each package on the
host, then builds an exFAT image sized to them (+25 % + 32 MiB) with the real
exFAT tools (`tools/exfat-host.sh`, no sudo). `PKGDISK=<image>` on any run
target attaches it as USB mass storage: the kernel probes AHCI/NVMe only
when there is no virtio disk (M41), but USB always, so it works next to the
virtio root. Reading it costs about 20 MB/s under TCG.

### Bigger root, `make grow-root`

`tools/growroot.sh <image> [size]`: refuses a missing marker, a file in use
(QEMU's image lock via `qemu-img info`, `fuser`), shrinking, `meta_bg`, and a
size whose group-descriptor table would not fit in the existing GDT plus the
reserved GDT blocks (the `resize_inode` feature): SecOS has no `meta_bg`, so
growing past that would make the disk unmountable. A 1 GiB `mkroot` disk has
127 reserved GDT blocks, enough for 1024x. Then: a sparse backup copy of the
image as it was (`BACKUP=0` skips it), `e2fsck -f -p` (replays a journal left
by an unclean shutdown; anything preen cannot fix stops the script),
`truncate` + `resize2fs`, `e2fsck -fn` must be clean and the feature set
unchanged; any failure puts the backup back.

The SecOS ext4 driver needed no change for a resized volume: resize2fs keeps
the layout `ext2_mount_root` already validates (no new feature flags), adds
groups with `INODE_UNINIT`/`BLOCK_UNINIT` whose bitmaps the M41 code builds
like `ext4_init_block_bitmap` (the loop over every group descriptor finds the
bitmaps and inode tables resize2fs placed in the flex-group heads 16/32/48),
and the backup superblocks + reserved GDT blocks in groups 25, 27, 49
(sparse_super) are found by `e2_group_has_super`. Checked by the gates below:
the writes of whole test runs are forced into the added groups (the old
1 GiB is filled first) and e2fsck stays clean.

### ext4 write speed (`fs/ext2.c`)

Installing 200 MB showed the root's write path was slow: `dd bs=1M` wrote
0.43 MB/s under TCG. Three changes, all inside one write call:

- crc32c by table (one 1 KiB table, byte at a time) instead of bit by bit:
  every block allocation on a metadata_csum volume checksums the block bitmap,
  inode bitmap, group descriptor and superblock, and every commit the journal
  tags;
- a block that the write covers whole is no longer zero-filled first (the
  data reaches the disk before the transaction commits, as before; a failed
  write aborts the transaction, so the allocation is rolled back);
- whole data blocks on consecutive physical blocks go out in one device
  request (up to 64 KiB) instead of one request per 4 KiB.

16 MiB with `dd bs=1M`: 37 s → 13 s. 4 KiB writes are still bound by one
journal commit per `write()` (about 20 I/Os): group commit is a later item.

## Bugs found

- **ext4 write path cost** (above): the zero-fill doubled every data write and
  the bit-loop crc32c dominated block allocation.
- `tools/persist_test.sh` counted 34 system programs; `/bin/spkg` makes 35.

## Tests

**PKG2 gate** (`tools/pkg2_test.sh`, selftest section "PKG2 GATE"; ~10 min
under TCG; `--big-mb`, `--kills`):

1. a fresh 1 GiB root, first boot (system install) + user files written in
   the guest, poweroff, e2fsck clean;
2. `make grow-root SIZE=8G` on it: e2fsck -fn clean, 2097152 blocks; a
   shrink request is refused; the old 1 GiB is then filled with an unwritten
   extent (`debugfs fallocate`) so later writes can only use the added groups;
3. boot: `df` 8388608 KiB, `md5sum -c` of the pre-grow files OK; the exFAT
   transfer disk (USB) mounts; refused with nothing installed: a payload byte
   flipped, a table byte flipped, signed packages with `/root/evil` and
   `/usr/../root/x`; `spkg verify` of the 200 MB package; install **76 s**
   (both passes + 200 MB written); `spkg check`; its program runs through
   the `/usr/bin` symlink, modes 0755/0644 root; md5 of every blob equals
   the host file; poweroff, e2fsck clean, **197 MiB of it in the added
   groups**;
4. reboot: still installed and intact; remove: no file, directory, database,
   staging entry or txn left (`find` over /opt /usr /etc /var/lib/spkg), the
   200 MiB come back;
5. upgrade/downgrade between two 24-file versions with kill -9 at random
   moments (0.3 s .. one install time) and at fixed points inside the commit
   (`SPKG_TEST_KILL_AT`, 17 and 22 actions): after each, `spkg recover`
   leaves exactly one version, all 24 files of it, its own extra file only,
   `spkg check` clean; 6/6 (1 old, 5 new: the random kills that land during
   staging keep the old one, the commit kills complete the new one);
6. power cut (QEMU killed) 5.7 s into an upgrade: the host e2fsck replays
   the journal and finds nothing else; the next boot runs `spkg recover`
   from init and one whole version is installed; remove, poweroff, clean;
   no `[EXC]`/panic/lockdep line in any boot.

Result: **36 passed, 0 failed**. (A first run with a 20 MB package found the
grow tests' "fill the old groups" arithmetic wrong: part of the grown
volume's metadata lives in the new flex-group heads; and the transfer disk
had no room for the hand-added tampered copies. Both test fixes.)

Also run by hand:
- the user's own 1 GiB disk (a **copy** of
  `.claude/worktrees/phase4-test/secos-root.img`, 235 files, journal dirty):
  `growroot.sh copy.img 8G` (journal replayed by `e2fsck -p`, resize, clean),
  booted (console entry switched to bash on the copy to log in), wrote
  files, poweroff, e2fsck clean; every file in /root /home /etc /var equal
  to the original (`debugfs rdump` + md5) except the expected ones (release
  stamp, manifest, logs, the edited inittab, the new files);
- `growroot.sh` on an image held by a running QEMU: refused, image
  unchanged; `2T` on a 1 GiB disk: refused (256 GDT blocks needed, 128 there,
  "largest size without meta_bg: 1024 GiB");
- `secos-pkg --v1` reproduces `crypto/user_testpkg_spkg.h` byte for byte.

Builds: `-Wall -Wextra` warnings 0 → 0 for the kernel, normal and
`-DSECOS_LOCKDEP`; `user/spkg.c`, `user/init.c` clean with `-Wextra`.

## For later steps

- U7 (musl): `user/spkg.c` uses only POSIX file calls, `getenv`, `kill`,
  `statfs`, `fdopen` + the raw `SYS_SIGVERIFY` (keep it in `libsecos.a`).
- U11: `spkg install <url>` = fetch with wget into a root-only file, then the
  same path (the two-pass digest check already covers a file that changes).
  The manifest then needs `CAP_NET`, or spkg runs wget (needs `CAP_PROC`).
- D9 (local signing): packages signed with the machine key go through the
  same `SYS_SIGVERIFY`; the kernel decides per `key_id`. A ceiling for
  locally signed *packages* (e.g. no files in `/usr/bin`) would be an spkg
  policy keyed on the key id the kernel reports.
- File size: the ext2/ext4 driver stores only the low 32 bits of `i_size`
  (`inode_size_lo`): files over 4 GiB are not supported yet. Large SDKs must
  split blobs (spkg itself handles 64-bit sizes).
- One ext volume at a time (M41) still holds: an RO SDK volume (D8 "Q6"
  alternative) would need multi-instance ext4.
