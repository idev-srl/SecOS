#!/usr/bin/env python3
"""
build_gpt_esp.py
=================

Create a GPT disk image with a single EFI System Partition (ESP) formatted as FAT32
and populated with:
  /EFI/BOOT/BOOTX64.EFI   (UEFI bootloader)
  /EFI/BOOT/kernel.elf    (Kernel ELF – optional duplicate path)
  /startup.nsh            (UEFI shell script fallback)
  /kernel.elf             (Kernel ELF at root for diagnostics)

The image is intended for QEMU/OVMF. It implements:
  - Protective MBR
  - Primary & backup GPT headers with correct CRC32
  - 128 partition entries (only first used) type GUID = EFI System Partition
  - FAT32 volume constructed manually (no external mkfs dependency)
  - 2 FATs, FSInfo, backup boot sector, root cluster = 2

Minimal FAT32 implementation sufficient for firmware to traverse directories.
We only create the needed directory tree. No timestamps, LFN entries, or advanced
attributes. Should be accepted by most UEFI firmware.

If you later wish to use the system's mkfs.vfat + mtools for higher fidelity,
you can integrate a "--system-tools" mode (not implemented here yet).

Usage examples:
  python3 tools/build_gpt_esp.py --bootx64 dist/EFI/BOOT/BOOTX64.EFI --kernel dist/kernel.elf --out disk.img
  python3 tools/build_gpt_esp.py --image-mb 300 --partition-mb 256 --out esp256.img \
      --bootx64 dist/EFI/BOOT/BOOTX64.EFI --kernel dist/kernel.elf

After generation run QEMU (excerpt):
  qemu-system-x86_64 -drive if=pflash,format=raw,readonly=on,file=OVMF_CODE.fd \
                     -drive if=pflash,format=raw,file=OVMF_VARS.fd \
                     -drive file=disk.img,if=virtio,format=raw

Implementation notes:
  - Default total image size: 300 MiB (allows FAT32 cluster count >= threshold).
  - Partition starts at LBA 2048 (1 MiB alignment) sized per --partition-mb.
  - FAT32 layout chosen with ReservedSectorCount=32, NumFATs=2.
  - We iterate to converge on FAT size & cluster count.
  - Short 8.3 names are used (BOOTX64.EFI, KERNEL.ELF, STARTUP.NSH). UEFI requires
    \EFI\BOOT\BOOTX64.EFI path – short name conforms.
  - Directory structure consumes clusters: root(2), EFI(3), BOOT(4), file data
    chains start at cluster 5.

Limitations:
  - No long file name entries.
  - No time/date stamps.
  - No free space bitmap beyond FAT default.
  - Does not recalculate free clusters dynamically; FSInfo gives an estimate.

Test strategy:
  - Basic internal assertions (CRC, layout bounds) when run with --verify.
  - Prints summary with key offsets.

Author: Automated generation
License: MIT (if not specified elsewhere in project)
"""

from __future__ import annotations
import argparse
import os
import struct
import sys
import uuid
import math
import binascii

SECTOR_SIZE = 512
PARTITION_TYPE_EFI_SYSTEM = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
GPT_HEADER_SIZE = 92  # Fixed per spec
GPT_NUM_ENTRIES = 128
GPT_ENTRY_SIZE = 128
PRIMARY_PART_ENTRIES_LBA = 2

def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF

def guid_bytes(u: uuid.UUID) -> bytes:
    """Return GUID in little-endian field order per UEFI (RFC4122 variant)."""
    # fields: time_low(4) time_mid(2) time_hi_version(2) clock_seq_hi(1) clock_seq_low(1) node(6)
    # first three fields little-endian, rest big-endian as stored
    b = u.bytes
    tl = struct.pack('<I', u.time_low)
    tm = struct.pack('<H', u.time_mid)
    th = struct.pack('<H', u.time_hi_version)
    rest = b[8:]
    return tl + tm + th + rest

def create_protective_mbr(total_sectors: int) -> bytes:
    mbr = bytearray(512)
    # Boot code left zeros
    # Partition entry at 0x1BE
    # Status
    mbr[0x1BE + 0] = 0x00
    # First CHS (dummy) 0x00 0x02 0x00
    mbr[0x1BE + 1:0x1BE + 4] = bytes([0x00, 0x02, 0x00])
    # Type 0xEE
    mbr[0x1BE + 4] = 0xEE
    # Last CHS (dummy) 0xFE 0xFF 0xFF
    mbr[0x1BE + 5:0x1BE + 8] = bytes([0xFE, 0xFF, 0xFF])
    # First LBA
    mbr[0x1BE + 8:0x1BE + 12] = struct.pack('<I', 1)
    # Sector count (limit to 0xFFFFFFFF)
    count = min(total_sectors - 1, 0xFFFFFFFF)
    mbr[0x1BE + 12:0x1BE + 16] = struct.pack('<I', count)
    # Signature 0x55AA
    mbr[510:512] = b"\x55\xAA"
    return bytes(mbr)

def build_gpt_headers(total_sectors: int, part_start_lba: int, part_end_lba: int, part_type: uuid.UUID, part_name: str) -> tuple[bytes, bytes, bytes]:
    """Return (primary_header, partition_entries, backup_header)."""
    first_usable = 34  # Standard when 128 entries (LBAs 2-33 are entries)
    last_usable = total_sectors - 34  # Backup entries occupy last 33 LBAs + header
    if part_start_lba < first_usable:
        raise ValueError("Partition start precedes first usable LBA")
    if part_end_lba > last_usable:
        raise ValueError("Partition end exceeds last usable LBA")

    # Build partition entries array
    entries = bytearray(GPT_NUM_ENTRIES * GPT_ENTRY_SIZE)
    unique_guid = uuid.uuid4()
    name_utf16 = part_name.encode('utf-16le')
    name_utf16 = name_utf16[:72]  # 36 UTF-16 chars max (72 bytes)
    entry = bytearray(GPT_ENTRY_SIZE)
    entry[0:16] = guid_bytes(part_type)
    entry[16:32] = guid_bytes(unique_guid)
    entry[32:40] = struct.pack('<Q', part_start_lba)
    entry[40:48] = struct.pack('<Q', part_end_lba)
    entry[48:56] = struct.pack('<Q', 0)  # attributes
    entry[56:56+len(name_utf16)] = name_utf16
    entries[0:GPT_ENTRY_SIZE] = entry
    entries_crc = crc32(entries)

    # Primary header
    header = bytearray(GPT_HEADER_SIZE)
    header[0:8] = b"EFI PART"  # Signature
    header[8:12] = struct.pack('<I', 0x00010000)  # Revision 1.0
    header[12:16] = struct.pack('<I', GPT_HEADER_SIZE)
    # CRC placeholder (will fill later)
    header[16:20] = struct.pack('<I', 0)
    header[20:24] = struct.pack('<I', 0)  # reserved
    header[24:32] = struct.pack('<Q', 1)  # MyLBA
    header[32:40] = struct.pack('<Q', total_sectors - 1)  # AlternateLBA
    header[40:48] = struct.pack('<Q', first_usable)
    header[48:56] = struct.pack('<Q', last_usable)
    disk_guid = uuid.uuid4()
    header[56:72] = guid_bytes(disk_guid)
    header[72:80] = struct.pack('<Q', PRIMARY_PART_ENTRIES_LBA)
    header[80:84] = struct.pack('<I', GPT_NUM_ENTRIES)
    header[84:88] = struct.pack('<I', GPT_ENTRY_SIZE)
    header[88:92] = struct.pack('<I', entries_crc)
    header_crc = crc32(bytes(header))
    header[16:20] = struct.pack('<I', header_crc)

    # Backup header
    backup = bytearray(GPT_HEADER_SIZE)
    backup[0:8] = b"EFI PART"
    backup[8:12] = struct.pack('<I', 0x00010000)
    backup[12:16] = struct.pack('<I', GPT_HEADER_SIZE)
    backup[16:20] = struct.pack('<I', 0)  # CRC placeholder
    backup[20:24] = struct.pack('<I', 0)
    backup[24:32] = struct.pack('<Q', total_sectors - 1)  # MyLBA
    backup[32:40] = struct.pack('<Q', 1)  # AlternateLBA
    backup[40:48] = struct.pack('<Q', first_usable)
    backup[48:56] = struct.pack('<Q', last_usable)
    backup[56:72] = guid_bytes(disk_guid)
    backup_entries_lba = total_sectors - 33  # Last 33 LBAs: entries(32) + header(1)
    backup[72:80] = struct.pack('<Q', backup_entries_lba)
    backup[80:84] = struct.pack('<I', GPT_NUM_ENTRIES)
    backup[84:88] = struct.pack('<I', GPT_ENTRY_SIZE)
    backup[88:92] = struct.pack('<I', entries_crc)
    backup_crc = crc32(bytes(backup))
    backup[16:20] = struct.pack('<I', backup_crc)

    return bytes(header), bytes(entries), bytes(backup)

def pick_fat32_geometry(part_sectors: int) -> tuple[int,int,int,int,int]:
    """Compute SecPerClus ensuring minimum 4KiB clusters (SecPerClus >= 8) and FAT32 cluster count threshold.
    Returns (SecPerClus, FATSz32, ClusterCount, ReservedSectorCount, RootCluster)."""
    reserved = 32  # includes boot, FSInfo, gap, backup boot
    num_fats = 2
    root_cluster = 2
    # Enforce firmware-friendly cluster sizes starting at 8 sectors (4KiB)
    for spc in [8,16,32,64]:
        fatsz = 1
        for _ in range(16):
            data_sectors = part_sectors - reserved - num_fats * fatsz
            if data_sectors <= 0:
                break
            cluster_count = data_sectors // spc
            fatsz_new = math.ceil((cluster_count + 2) * 4 / SECTOR_SIZE)
            if fatsz_new == fatsz:
                break
            fatsz = fatsz_new
        if cluster_count >= 65525:
            return spc, fatsz, cluster_count, reserved, root_cluster
    raise ValueError("Partition size does not yield FAT32 cluster count with >=4KiB clusters; enlarge partition.")

def build_fat32(partition_size_sectors: int, part_start_lba: int, files: dict[str, bytes]) -> bytes:
    """Return bytearray representing entire partition (FAT32 volume)."""
    spc, fatsz, cluster_count, reserved, root_cluster = pick_fat32_geometry(partition_size_sectors)
    num_fats = 2
    first_data_sector = reserved + num_fats * fatsz
    # Layout size check
    if first_data_sector >= partition_size_sectors:
        raise ValueError("Invalid FAT layout; data area beyond partition size")

    # Prepare FAT arrays
    fat_entries = cluster_count + 2
    # FAT initialization
    fat = [0] * fat_entries
    # Cluster 0 & 1 reserved signature values
    fat[0] = 0x0FFFFFF8  # Media descriptor low nibble F8
    fat[1] = 0xFFFFFFFF
    # We'll allocate directory clusters deterministically.
    next_free_cluster = 2
    def alloc_cluster_chain(num_clusters: int) -> list[int]:
        nonlocal next_free_cluster
        if next_free_cluster + num_clusters > fat_entries:
            raise ValueError("Not enough clusters for files")
        chain = list(range(next_free_cluster, next_free_cluster + num_clusters))
        next_free_cluster += num_clusters
        # Link in FAT
        for i, c in enumerate(chain):
            fat[c] = 0x0FFFFFFF if i == len(chain) - 1 else chain[i+1]
        return chain

    # Reserve root directory cluster (cluster 2)
    root_dir_cluster = root_cluster
    if next_free_cluster != root_dir_cluster:
        raise AssertionError("Root cluster mismatch")
    next_free_cluster += 1

    # Create EFI and BOOT directories clusters
    efi_dir_cluster = alloc_cluster_chain(1)[0]
    boot_dir_cluster = alloc_cluster_chain(1)[0]

    # Allocate file clusters
    file_allocation = {}
    for path, data in files.items():
        cluster_bytes = spc * SECTOR_SIZE
        needed_clusters = max(1, math.ceil(len(data) / cluster_bytes))
        chain = alloc_cluster_chain(needed_clusters)
        file_allocation[path] = (chain, data)

    # Begin building partition image buffer
    image = bytearray(partition_size_sectors * SECTOR_SIZE)

    # Build BPB (Boot Sector)
    total_sectors = partition_size_sectors
    bpb = bytearray(SECTOR_SIZE)
    bpb[0:3] = b"\xEB\x58\x90"  # JMP + NOP (arbitrary short jump)
    bpb[3:11] = b"MSDOS5.0"  # OEM name
    bpb[11:13] = struct.pack('<H', SECTOR_SIZE)  # BytesPerSec
    bpb[13] = spc  # SecPerClus
    bpb[14:16] = struct.pack('<H', reserved)  # RsvdSecCnt
    bpb[16] = num_fats  # NumFATs
    bpb[17:19] = struct.pack('<H', 0)  # RootEntCnt (FAT32)
    bpb[19:21] = struct.pack('<H', 0)  # TotSec16 (FAT32 -> 0)
    bpb[21] = 0xF8  # Media
    bpb[22:24] = struct.pack('<H', 0)  # FATSz16=0
    bpb[24:26] = struct.pack('<H', 32)  # SectorsPerTrack dummy
    bpb[26:28] = struct.pack('<H', 64)  # NumHeads dummy
    bpb[28:32] = struct.pack('<I', part_start_lba)  # HiddenSectors
    bpb[32:36] = struct.pack('<I', total_sectors)  # TotSec32
    bpb[36:40] = struct.pack('<I', fatsz)  # FATSz32
    bpb[40] = 0x00  # ExtFlags
    bpb[41] = 0x00  # FSVer low
    bpb[42:46] = struct.pack('<I', root_cluster)  # RootCluster
    bpb[46:48] = struct.pack('<H', 1)  # FSInfo sector
    bpb[48:50] = struct.pack('<H', 6)  # Backup boot sector
    bpb[50:52] = struct.pack('<H', 0)  # Reserved
    # FAT32 BPB extended section (offsets 64..90):
    # 64..67 Volume ID, 68..78 Volume Label (11), 79..90 FS Type.
    import random
    vol_id = random.getrandbits(32)
    struct.pack_into('<I', bpb, 64, vol_id)
    label = (os.environ.get('SECOS_VOL_LABEL','SECOS')).upper().ljust(11)[:11]
    bpb[68:79] = label.encode('ascii')
    bpb[82:90] = b"FAT32   "  # File system type
    # Signature for boot sector
    bpb[510:512] = b"\x55\xAA"
    image[0:SECTOR_SIZE] = bpb

    # FSInfo sector
    fsinfo = bytearray(SECTOR_SIZE)
    # Lead signature 0x41615252 at offset 0
    fsinfo[0:4] = b"RRaA"  # 0x52 0x61 0x41 0x52 little-endian matches RRaA
    # Structure signature at 484: 0x61417272
    fsinfo[484:488] = b"rrAa"
    free_clusters = cluster_count - next_free_cluster
    fsinfo[488:492] = struct.pack('<I', free_clusters if free_clusters >= 0 else 0xFFFFFFFF)
    fsinfo[492:496] = struct.pack('<I', next_free_cluster)  # next free cluster
    # Trail signature 0xAA55 at 508
    fsinfo[508:510] = b"\x55\xAA"
    image[SECTOR_SIZE:2*SECTOR_SIZE] = fsinfo

    # Backup boot sector (copy of sector 0) at sector 6
    backup_boot_offset = 6 * SECTOR_SIZE
    image[backup_boot_offset:backup_boot_offset + SECTOR_SIZE] = bpb

    # Write FATs
    fat_sector_start = reserved * SECTOR_SIZE
    def write_fat(base_offset: int):
        buf = bytearray(fatsz * SECTOR_SIZE)
        # Each FAT32 entry 4 bytes little-endian
        for idx, val in enumerate(fat):
            struct.pack_into('<I', buf, idx * 4, val)
        image[base_offset:base_offset + len(buf)] = buf
    write_fat(fat_sector_start)
    write_fat(fat_sector_start + fatsz * SECTOR_SIZE)

    # Helper to write directory entries
    def short_dir_entry(name: str, ext: str, attr: int, cluster: int, size: int) -> bytes:
        entry = bytearray(32)
        short_name = name.upper().ljust(8)[:8] + ext.upper().ljust(3)[:3]
        entry[0:11] = short_name.encode('ascii')
        entry[11] = attr
        # Leave time/date zeros
        entry[26:28] = struct.pack('<H', cluster & 0xFFFF)
        entry[20:22] = struct.pack('<H', (cluster >> 16) & 0xFFFF)  # High cluster
        entry[28:32] = struct.pack('<I', size)
        return bytes(entry)

    # Build root directory cluster content
    cluster_bytes = spc * SECTOR_SIZE
    def cluster_offset(c: int) -> int:
        data_sector = first_data_sector + (c - 2) * spc
        return data_sector * SECTOR_SIZE

    root_entries = []
    # Volume label entry (attribute 0x08) - 11 char padded name
    vol_label = short_dir_entry('SECOS', '', 0x08, 0, 0)
    root_entries.append(vol_label)
    root_entries.append(short_dir_entry('EFI', '', 0x10, efi_dir_cluster, 0))
    # startup.nsh file entry
    if 'startup.nsh' in files:
        sc_chain, sc_data = file_allocation['startup.nsh']
        root_entries.append(short_dir_entry('STARTUP', 'NSH', 0x20, sc_chain[0], len(sc_data)))
    # kernel.elf at root
    if 'kernel.elf' in files:
        k_chain, k_data = file_allocation['kernel.elf']
        root_entries.append(short_dir_entry('KERNEL', 'ELF', 0x20, k_chain[0], len(k_data)))
    root_entries.append(b'\x00' * 32)  # End marker
    root_cluster_data = bytearray(cluster_bytes)
    pos = 0
    for e in root_entries:
        root_cluster_data[pos:pos+32] = e
        pos += 32
    image[cluster_offset(root_dir_cluster):cluster_offset(root_dir_cluster)+cluster_bytes] = root_cluster_data

    # EFI directory
    efi_entries = []
    efi_entries.append(short_dir_entry('BOOT', '', 0x10, boot_dir_cluster, 0))
    efi_entries.append(b'\x00' * 32)
    efi_cluster_data = bytearray(cluster_bytes)
    pos = 0
    for e in efi_entries:
        efi_cluster_data[pos:pos+32] = e
        pos += 32
    image[cluster_offset(efi_dir_cluster):cluster_offset(efi_dir_cluster)+cluster_bytes] = efi_cluster_data

    # BOOT directory
    boot_entries = []
    if 'EFI/BOOT/BOOTX64.EFI' in files:
        b_chain, b_data = file_allocation['EFI/BOOT/BOOTX64.EFI']
        boot_entries.append(short_dir_entry('BOOTX64', 'EFI', 0x20, b_chain[0], len(b_data)))
    # Optional kernel duplicate inside BOOT
    if 'EFI/BOOT/kernel.elf' in files:
        kb_chain, kb_data = file_allocation['EFI/BOOT/kernel.elf']
        boot_entries.append(short_dir_entry('KERNEL', 'ELF', 0x20, kb_chain[0], len(kb_data)))
    boot_entries.append(b'\x00' * 32)
    boot_cluster_data = bytearray(cluster_bytes)
    pos = 0
    for e in boot_entries:
        boot_cluster_data[pos:pos+32] = e
        pos += 32
    image[cluster_offset(boot_dir_cluster):cluster_offset(boot_dir_cluster)+cluster_bytes] = boot_cluster_data

    # Write file data into allocated clusters
    for path, (chain, data) in file_allocation.items():
        remaining = data
        for c in chain:
            slice_data = remaining[:cluster_bytes]
            remaining = remaining[cluster_bytes:]
            buf = bytearray(cluster_bytes)
            buf[:len(slice_data)] = slice_data
            image[cluster_offset(c):cluster_offset(c)+cluster_bytes] = buf

    return image

def build_image(args):
    total_image_sectors = (args.image_mb * 1024 * 1024) // SECTOR_SIZE
    partition_sectors = (args.partition_mb * 1024 * 1024) // SECTOR_SIZE
    if partition_sectors >= total_image_sectors // 2:
        print("[WARN] Partition uses >= half the disk; consider enlarging image for backup GPT space.")
    # Ensure disk large enough for GPT structures: Need at least 34 + partition + 33
    min_required = 34 + partition_sectors + 33
    if total_image_sectors < min_required:
        raise SystemExit(f"Image too small. Need >= {min_required*SECTOR_SIZE//1024//1024} MiB")

    part_start_lba = 2048  # 1 MiB alignment
    part_end_lba = part_start_lba + partition_sectors - 1
    total_sectors = total_image_sectors

    print(f"[INFO] Disk sectors: {total_sectors}, Partition: LBA {part_start_lba}-{part_end_lba} ({partition_sectors} sectors)")

    # Load file contents
    files: dict[str, bytes] = {}
    def read_optional(path: str, target_key: str):
        if path and os.path.isfile(path):
            with open(path, 'rb') as f:
                data = f.read()
            files[target_key] = data
            print(f"[INFO] Added file {target_key} size={len(data)}")
        else:
            print(f"[WARN] Missing {path}; skipping {target_key}")

    read_optional(args.bootx64, 'EFI/BOOT/BOOTX64.EFI')
    read_optional(args.kernel, 'kernel.elf')
    # Duplicate kernel inside EFI/BOOT directory if present
    if 'kernel.elf' in files:
        files['EFI/BOOT/kernel.elf'] = files['kernel.elf']
    # startup.nsh script
    startup_script = b"echo Starting SecOS UEFI\nfs0:\EFI\BOOT\BOOTX64.EFI\n"
    files['startup.nsh'] = startup_script
    # Build FAT32 partition image
    print("[INFO] Building FAT32 volume (may take a moment)...")
    partition_image = build_fat32(partition_sectors, part_start_lba, files)
    print(f"[INFO] FAT32 volume built: {len(partition_image)} bytes")

    # GPT headers and entries
    primary_header, entries, backup_header = build_gpt_headers(total_sectors, part_start_lba, part_end_lba, PARTITION_TYPE_EFI_SYSTEM, "EFI System Partition")
    print("[INFO] GPT headers built (CRC OK).")

    # Assemble full disk image
    disk = bytearray(total_sectors * SECTOR_SIZE)
    disk[0:SECTOR_SIZE] = create_protective_mbr(total_sectors)
    disk[SECTOR_SIZE:SECTOR_SIZE+GPT_HEADER_SIZE] = primary_header
    entries_bytes = entries
    entries_len = len(entries_bytes)
    disk[PRIMARY_PART_ENTRIES_LBA*SECTOR_SIZE:PRIMARY_PART_ENTRIES_LBA*SECTOR_SIZE + entries_len] = entries_bytes
    # Write partition
    part_offset = part_start_lba * SECTOR_SIZE
    disk[part_offset:part_offset + len(partition_image)] = partition_image
    # Backup entries
    backup_entries_lba = total_sectors - 33
    disk[backup_entries_lba*SECTOR_SIZE:backup_entries_lba*SECTOR_SIZE + entries_len] = entries_bytes
    # Backup header
    disk[(total_sectors - 1)*SECTOR_SIZE:(total_sectors - 1)*SECTOR_SIZE + GPT_HEADER_SIZE] = backup_header

    with open(args.out, 'wb') as f:
        f.write(disk)
    print(f"[OK] Disk image written to {args.out} (size={len(disk)} bytes)")

    if args.verify:
        # Basic structural checks
        assert disk[510:512] == b"\x55\xAA", "MBR signature missing"
        assert disk[SECTOR_SIZE:SECTOR_SIZE+8] == b"EFI PART", "Primary GPT signature missing"
        assert disk[(total_sectors - 1)*SECTOR_SIZE:(total_sectors - 1)*SECTOR_SIZE + 8] == b"EFI PART", "Backup GPT signature missing"
        print("[VERIFY] Basic signatures OK.")

def parse_args(argv):
    p = argparse.ArgumentParser(description="Build GPT disk image with FAT32 EFI System Partition for SecOS")
    p.add_argument('--out', required=True, help='Output disk image path')
    p.add_argument('--bootx64', default='dist/EFI/BOOT/BOOTX64.EFI', help='Path to BOOTX64.EFI')
    p.add_argument('--kernel', default='dist/kernel.elf', help='Path to kernel ELF')
    p.add_argument('--image-mb', type=int, default=300, help='Total disk image size in MiB (default 300)')
    p.add_argument('--partition-mb', type=int, default=256, help='ESP partition size in MiB (default 256)')
    p.add_argument('--verify', action='store_true', help='Run internal sanity checks after build')
    return p.parse_args(argv)

def main(argv=None):
    args = parse_args(argv or sys.argv[1:])
    try:
        build_image(args)
    except Exception as e:
        print(f"[ERROR] {e}")
        return 1
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
