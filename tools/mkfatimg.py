#!/usr/bin/env python3
"""
mkfatimg.py - Minimal FAT32 image builder for UEFI testing.
Creates a small FAT32 volume containing \EFI\BOOT\BOOTX64.EFI (and optionally kernel.elf).
No external dependencies; not a full implementation, just enough for firmware to read.
WARNING: Simplified and not robust. For development only.
"""
import sys, struct, os

BYTES_PER_SECTOR = 512
SECTORS_PER_CLUSTER = 8  # 4KB clusters
RESERVED_SECTORS = 32    # includes boot + FSInfo + backup later
NUM_FATS = 2
ROOT_CLUSTER = 2
FSINFO_SECTOR = 1
BACKUP_BOOT_SECTOR = 6
MEDIA_BYTE = 0xF8

# FAT32 end of cluster chain marker
EOC = 0x0FFFFFFF

def le32(x):
    return struct.pack('<I', x)

def build_image(bootfile, outimg, kernelfile=None, min_size_mb=32):
    with open(bootfile, 'rb') as f:
        boot_data = f.read()
    kernel_data = b''
    if kernelfile and os.path.exists(kernelfile):
        with open(kernelfile, 'rb') as kf:
            kernel_data = kf.read()
    # Directory structure clusters needed: root, EFI dir, BOOT dir
    boot_clusters = (len(boot_data) + (SECTORS_PER_CLUSTER*BYTES_PER_SECTOR - 1)) // (SECTORS_PER_CLUSTER*BYTES_PER_SECTOR)
    boot_clusters = (len(boot_data) + (SECTORS_PER_CLUSTER*BYTES_PER_SECTOR - 1)) // (SECTORS_PER_CLUSTER*BYTES_PER_SECTOR)
    kernel_clusters = 0
    if kernel_data:
        kernel_clusters = (len(kernel_data) + (SECTORS_PER_CLUSTER*BYTES_PER_SECTOR - 1)) // (SECTORS_PER_CLUSTER*BYTES_PER_SECTOR)
    # Cluster assignments fixed for early entries
    # 2 root, 3 EFI, 4 BOOT, 5.. file BOOTX64.EFI, then maybe kernel
    first_boot_cluster = 5
    first_kernel_cluster = first_boot_cluster + boot_clusters if kernel_clusters else None
    used_last_cluster = first_boot_cluster + boot_clusters - 1
    if kernel_clusters:
        used_last_cluster = first_kernel_cluster + kernel_clusters - 1
    used_clusters_required = used_last_cluster + 1  # count of clusters (starting from 0 conceptually, but we use >=2)

    # Target total sectors based on min_size_mb
    desired_total_sectors = max(min_size_mb * 1024 * 1024 // BYTES_PER_SECTOR, 4096)

    # Iteratively pick cluster_count so that computed total_sectors >= desired_total_sectors
    cluster_count = max(used_clusters_required + 128, 1024)
    while True:
        fat_entries = cluster_count + 2  # include reserved entries
        fat_size_bytes = fat_entries * 4
        fat_size_sectors = (fat_size_bytes + BYTES_PER_SECTOR - 1) // BYTES_PER_SECTOR
        data_sectors = cluster_count * SECTORS_PER_CLUSTER
        total_sectors = RESERVED_SECTORS + (NUM_FATS * fat_size_sectors) + data_sectors
        if total_sectors >= desired_total_sectors:
            break
        cluster_count += 128
    # Final image size from BPB total sectors
    img_size = total_sectors * BYTES_PER_SECTOR

    # Allocate full image
    img = bytearray(img_size)
    # Boot sector (BPB)
    bs = bytearray(BYTES_PER_SECTOR)
    bs[0:3] = b'\xEB\x58\x90'  # JMP
    bs[3:11] = b'MSWIN4.1'
    bs[11:13] = struct.pack('<H', BYTES_PER_SECTOR)
    bs[13] = SECTORS_PER_CLUSTER
    bs[14:16] = struct.pack('<H', RESERVED_SECTORS)
    bs[16] = NUM_FATS
    bs[17:19] = b'\x00\x00'  # RootEntCnt (0 for FAT32)
    bs[19:21] = b'\x00\x00'  # TotSec16
    bs[21] = MEDIA_BYTE
    bs[22:24] = b'\x00\x00'  # FATSz16
    bs[24:26] = struct.pack('<H', 63)  # SecPerTrk
    bs[26:28] = struct.pack('<H', 255) # NumHeads
    bs[28:32] = b'\x00\x00\x00\x00' # HiddenSec
    bs[32:36] = le32(total_sectors)      # Total sectors 32-bit
    bs[36:40] = le32(fat_size_sectors)   # FAT size sectors
    bs[40:42] = b'\x00\x00'  # ExtFlags
    bs[42:44] = b'\x00\x00'  # FSVer
    bs[44:48] = le32(ROOT_CLUSTER)
    bs[48:50] = struct.pack('<H', FSINFO_SECTOR)
    bs[50:52] = struct.pack('<H', BACKUP_BOOT_SECTOR)
    bs[64] = 0x80  # DriveNum
    bs[66] = 0x29  # BootSig
    bs[67:71] = le32(0x12345678)  # VolumeID
    vol_label = b'SECOS      '  # 11 bytes
    bs[71:82] = vol_label
    bs[82:90] = b'FAT32   '
    bs[510:512] = b'\x55\xAA'
    img[0:BYTES_PER_SECTOR] = bs
    # FSInfo sector
    fsinfo = bytearray(BYTES_PER_SECTOR)
    fsinfo[0:4] = b'RRaA'
    fsinfo[484:488] = b'rrAa'
    fsinfo[488:492] = le32(0xFFFFFFFF)  # free cluster count unknown
    fsinfo[492:496] = le32(0xFFFFFFFF)  # next free cluster unknown
    fsinfo[508:512] = b'\x55\xAA'
    img[FSINFO_SECTOR*BYTES_PER_SECTOR:(FSINFO_SECTOR+1)*BYTES_PER_SECTOR] = fsinfo
    # Backup boot sector
    img[BACKUP_BOOT_SECTOR*BYTES_PER_SECTOR:(BACKUP_BOOT_SECTOR+1)*BYTES_PER_SECTOR] = bs

    # FAT tables start
    fat_start = RESERVED_SECTORS * BYTES_PER_SECTOR
    def write_fat_entry(fat_bytes, index, value):
        struct.pack_into('<I', fat_bytes, index*4, value)
    fat = bytearray(fat_size_sectors * BYTES_PER_SECTOR)
    # First two reserved entries
    write_fat_entry(fat, 0, 0x0FFFFFF8)
    write_fat_entry(fat, 1, 0xFFFFFFFF)
    # Root cluster 2 EOC
    write_fat_entry(fat, ROOT_CLUSTER, EOC)
    # EFI cluster 3
    write_fat_entry(fat, 3, EOC)
    # BOOT cluster 4
    write_fat_entry(fat, 4, EOC)
    # File clusters chain
    c = first_boot_cluster
    for i in range(boot_clusters):
        if i == boot_clusters - 1:
            write_fat_entry(fat, c, EOC)
        else:
            write_fat_entry(fat, c, c+1)
        c += 1
    if kernel_clusters:
        c = first_kernel_cluster
        for i in range(kernel_clusters):
            if i == kernel_clusters - 1:
                write_fat_entry(fat, c, EOC)
            else:
                write_fat_entry(fat, c, c+1)
            c += 1
    # Copy FAT to both
    img[fat_start:fat_start+len(fat)] = fat
    second_fat_offset = fat_start + len(fat)
    img[second_fat_offset:second_fat_offset+len(fat)] = fat

    # Data region start
    data_start_sector = RESERVED_SECTORS + NUM_FATS * fat_size_sectors
    def cluster_offset(cluster):
        return (data_start_sector + (cluster - 2) * SECTORS_PER_CLUSTER) * BYTES_PER_SECTOR

    # Helper to make 8.3 name entry
    def mk_entry(name8, ext3, attr, first_cluster, size):
        e = bytearray(32)
        e[0:8] = name8
        e[8:11] = ext3
        e[11] = attr
        # write cluster (FAT32 uses high+low)
        struct.pack_into('<H', e, 20, (first_cluster >> 16) & 0xFFFF)
        struct.pack_into('<H', e, 26, first_cluster & 0xFFFF)
        struct.pack_into('<I', e, 28, size)
        return e

    ATTR_DIR = 0x10
    ATTR_ARCH = 0x20

    # Root directory cluster: entry for EFI
    root_cluster_data = bytearray(SECTORS_PER_CLUSTER * BYTES_PER_SECTOR)
    root_cluster_data[0:32] = mk_entry(b'EFI     ', b'   ', ATTR_DIR, 3, 0)
    img[cluster_offset(ROOT_CLUSTER):cluster_offset(ROOT_CLUSTER)+len(root_cluster_data)] = root_cluster_data

    # EFI directory cluster: entry for BOOT
    efi_cluster_data = bytearray(SECTORS_PER_CLUSTER * BYTES_PER_SECTOR)
    efi_cluster_data[0:32] = mk_entry(b'BOOT    ', b'   ', ATTR_DIR, 4, 0)
    img[cluster_offset(3):cluster_offset(3)+len(efi_cluster_data)] = efi_cluster_data

    # BOOT directory cluster: entry for BOOTX64.EFI (and optional KERNEL  ELF)
    boot_dir = bytearray(SECTORS_PER_CLUSTER * BYTES_PER_SECTOR)
    boot_dir[0:32] = mk_entry(b'BOOTX64 ', b'EFI', ATTR_ARCH, first_boot_cluster, len(boot_data))
    if kernel_clusters:
        # Name kernel.elf -> KERNEL  ELF
        boot_dir[32:64] = mk_entry(b'KERNEL  ', b'ELF', ATTR_ARCH, first_kernel_cluster, len(kernel_data))
    img[cluster_offset(4):cluster_offset(4)+len(boot_dir)] = boot_dir

    # Write file data clusters
    def write_file(data, first_cluster):
        remaining = data
        cluster = first_cluster
        while remaining:
            chunk = remaining[:SECTORS_PER_CLUSTER*BYTES_PER_SECTOR]
            img[cluster_offset(cluster):cluster_offset(cluster)+len(chunk)] = chunk
            remaining = remaining[len(chunk):]
            cluster += 1
    write_file(boot_data, first_boot_cluster)
    if kernel_clusters:
        write_file(kernel_data, first_kernel_cluster)

    with open(outimg, 'wb') as out:
        out.write(img)
    print(f"Created FAT32 image {outimg} size={img_size//1024}KB total_sectors={total_sectors} cluster_count={cluster_count} used_clusters={used_clusters_required} boot_clusters={boot_clusters}")

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: mkfatimg.py <bootx64.efi> <output.img> [kernel.elf]", file=sys.stderr)
        sys.exit(1)
    bootfile = sys.argv[1]
    outimg = sys.argv[2]
    kernelfile = sys.argv[3] if len(sys.argv) > 3 else None
    build_image(bootfile, outimg, kernelfile, min_size_mb=32)
