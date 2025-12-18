#!/usr/bin/env python3
"""
mkgptfat.py - Create a minimal GPT disk image with one FAT32 EFI System Partition
containing \EFI\BOOT\BOOTX64.EFI and optional kernel.elf.
No external tools or root required.
Simplifications: single partition starting at LBA 2048, size fixed to fit files.
"""
import sys, struct, os

BYTES_PER_SECTOR = 512
PARTITION_START_LBA = 2048  # Typical alignment
EFI_SYSTEM_GUID = bytes.fromhex('28732ac11ff8d211ba4b00a0c93ec93b')  # EFI System Partition GUID (little endian in GPT)
DISK_GUID = bytes.fromhex('112233445566778899aabbccddeeff00')
PART_GUID = bytes.fromhex('00ffeeddccbbaa998877665544332211')
SECTORS_PER_CLUSTER = 8
RESERVED_SECTORS = 32
NUM_FATS = 2
MEDIA_BYTE = 0xF8
FSINFO_SECTOR = 1
BACKUP_BOOT_SECTOR = 6
ROOT_CLUSTER = 2
EOC = 0x0FFFFFFF

def le16(x): return struct.pack('<H', x)
def le32(x): return struct.pack('<I', x)
def le64(x): return struct.pack('<Q', x)

def build_fat(volume_sectors, boot_data, kernel_data):
    # cluster calculations similar to previous script but inside partition
    cluster_bytes = SECTORS_PER_CLUSTER * BYTES_PER_SECTOR
    boot_clusters = (len(boot_data) + cluster_bytes - 1)//cluster_bytes
    kernel_clusters = 0
    if kernel_data:
        kernel_clusters = (len(kernel_data) + cluster_bytes -1)//cluster_bytes
    first_boot_cluster = 5
    first_kernel_cluster = first_boot_cluster + boot_clusters if kernel_clusters else None
    used_last_cluster = first_boot_cluster + boot_clusters -1
    if kernel_clusters:
        used_last_cluster = first_kernel_cluster + kernel_clusters -1
    used_clusters_required = used_last_cluster + 1
    cluster_count = max(used_clusters_required + 128, 1024)
    while True:
        fat_entries = cluster_count + 2
        fat_size_bytes = fat_entries*4
        fat_size_sectors = (fat_size_bytes + BYTES_PER_SECTOR -1)//BYTES_PER_SECTOR
        data_sectors = cluster_count * SECTORS_PER_CLUSTER
        total_sectors = RESERVED_SECTORS + NUM_FATS*fat_size_sectors + data_sectors
        if total_sectors <= volume_sectors:
            break
        cluster_count -= 64  # shrink if overshoot
    fat_entries = cluster_count + 2
    fat_size_bytes = fat_entries*4
    fat_size_sectors = (fat_size_bytes + BYTES_PER_SECTOR -1)//BYTES_PER_SECTOR
    data_sectors = cluster_count * SECTORS_PER_CLUSTER
    total_sectors = RESERVED_SECTORS + NUM_FATS*fat_size_sectors + data_sectors
    # Build boot sector
    bs = bytearray(BYTES_PER_SECTOR)
    bs[0:3] = b'\xEB\x58\x90'
    bs[3:11] = b'MSWIN4.1'
    bs[11:13] = le16(BYTES_PER_SECTOR)
    bs[13] = SECTORS_PER_CLUSTER
    bs[14:16] = le16(RESERVED_SECTORS)
    bs[16] = NUM_FATS
    bs[17:19] = b'\x00\x00'
    bs[19:21] = b'\x00\x00'
    bs[21] = MEDIA_BYTE
    bs[22:24] = b'\x00\x00'
    bs[24:26] = le16(63)
    bs[26:28] = le16(255)
    bs[28:32] = b'\x00\x00\x00\x00'
    bs[32:36] = le32(total_sectors)
    bs[36:40] = le32(fat_size_sectors)
    bs[40:42] = b'\x00\x00'
    bs[42:44] = b'\x00\x00'
    bs[44:48] = le32(ROOT_CLUSTER)
    bs[48:50] = le16(FSINFO_SECTOR)
    bs[50:52] = le16(BACKUP_BOOT_SECTOR)
    bs[64] = 0x80
    bs[66] = 0x29
    bs[67:71] = le32(0x12345678)
    bs[71:82] = b'SECOS      '
    bs[82:90] = b'FAT32   '
    bs[510:512] = b'\x55\xAA'

    fsinfo = bytearray(BYTES_PER_SECTOR)
    fsinfo[0:4] = b'RRaA'
    fsinfo[484:488] = b'rrAa'
    fsinfo[488:492] = le32(0xFFFFFFFF)
    fsinfo[492:496] = le32(0xFFFFFFFF)
    fsinfo[508:512] = b'\x55\xAA'

    fat = bytearray(fat_size_sectors*BYTES_PER_SECTOR)
    def setfat(idx,val): struct.pack_into('<I', fat, idx*4, val)
    setfat(0,0x0FFFFFF8); setfat(1,0xFFFFFFFF); setfat(ROOT_CLUSTER,EOC); setfat(3,EOC); setfat(4,EOC)
    # file clusters
    c=first_boot_cluster
    for i in range(boot_clusters):
        setfat(c, EOC if i==boot_clusters-1 else c+1)
        c+=1
    if kernel_clusters:
        c=first_kernel_cluster
        for i in range(kernel_clusters):
            setfat(c, EOC if i==kernel_clusters-1 else c+1)
            c+=1

    def cluster_offset(cluster):
        data_start = RESERVED_SECTORS + NUM_FATS*fat_size_sectors
        sector = data_start + (cluster-2)*SECTORS_PER_CLUSTER
        return sector*BYTES_PER_SECTOR

    def mk_entry(name8, ext3, attr, first_cluster, size):
        e=bytearray(32)
        e[0:8]=name8; e[8:11]=ext3; e[11]=attr
        struct.pack_into('<H', e, 20, (first_cluster>>16)&0xFFFF)
        struct.pack_into('<H', e, 26, first_cluster & 0xFFFF)
        struct.pack_into('<I', e, 28, size)
        return e
    ATTR_DIR=0x10; ATTR_ARCH=0x20; ATTR_VOL=0x08

    root = bytearray(SECTORS_PER_CLUSTER*BYTES_PER_SECTOR)
    root[0:32] = mk_entry(b'SECOS     ', b'   ', ATTR_VOL, 0, 0)
    root[32:64] = mk_entry(b'EFI     ', b'   ', ATTR_DIR, 3,0)
    efi = bytearray(SECTORS_PER_CLUSTER*BYTES_PER_SECTOR)
    efi[0:32] = mk_entry(b'BOOT    ', b'   ', ATTR_DIR,4,0)
    bootdir = bytearray(SECTORS_PER_CLUSTER*BYTES_PER_SECTOR)
    bootdir[0:32] = mk_entry(b'BOOTX64 ', b'EFI', ATTR_ARCH, first_boot_cluster, len(boot_data))
    if kernel_data:
        bootdir[32:64] = mk_entry(b'KERNEL  ', b'ELF', ATTR_ARCH, first_kernel_cluster, len(kernel_data))

    part_bytes = volume_sectors*BYTES_PER_SECTOR
    part = bytearray(part_bytes)
    part[0:BYTES_PER_SECTOR] = bs
    part[FSINFO_SECTOR*BYTES_PER_SECTOR:(FSINFO_SECTOR+1)*BYTES_PER_SECTOR] = fsinfo
    part[BACKUP_BOOT_SECTOR*BYTES_PER_SECTOR:(BACKUP_BOOT_SECTOR+1)*BYTES_PER_SECTOR] = bs
    fat_start = RESERVED_SECTORS*BYTES_PER_SECTOR
    part[fat_start:fat_start+len(fat)] = fat
    part[fat_start+len(fat):fat_start+2*len(fat)] = fat
    part[cluster_offset(ROOT_CLUSTER):cluster_offset(ROOT_CLUSTER)+len(root)] = root
    part[cluster_offset(3):cluster_offset(3)+len(efi)] = efi
    part[cluster_offset(4):cluster_offset(4)+len(bootdir)] = bootdir

    # file data
    def write_file(buf, first_cluster):
        cluster=first_cluster; remaining=buf
        while remaining:
            chunk=remaining[:SECTORS_PER_CLUSTER*BYTES_PER_SECTOR]
            off=cluster_offset(cluster)
            part[off:off+len(chunk)] = chunk
            remaining=remaining[len(chunk):]
            cluster+=1
    write_file(boot_data, first_boot_cluster)
    if kernel_data:
        write_file(kernel_data, first_kernel_cluster)
    return part, total_sectors

def build_disk(bootfile, outimg, kernelfile=None, disk_mb=64):
    with open(bootfile,'rb') as f: boot_data=f.read()
    kernel_data=b''
    if kernelfile and os.path.exists(kernelfile):
        with open(kernelfile,'rb') as kf: kernel_data=kf.read()
    disk_bytes = disk_mb*1024*1024
    # Reserve first 1MB for GPT + alignment even if not used fully
    min_partition_bytes = disk_bytes - PARTITION_START_LBA*BYTES_PER_SECTOR
    # Choose partition size as min_partition_bytes rounded down to sector
    partition_sectors = min_partition_bytes // BYTES_PER_SECTOR
    part, part_total_sectors = build_fat(partition_sectors, boot_data, kernel_data)
    # Adjust partition_sectors to actual used part size (avoid trailing unused FAT region confusion)
    partition_sectors = part_total_sectors
    disk_total_sectors = PARTITION_START_LBA + partition_sectors

    # Protective MBR
    mbr = bytearray(BYTES_PER_SECTOR)
    mbr[446:446+16] = b'\x00' + b'\x00\x02\x00' + b'\xEE' + b'\x00\xFF\xFF' + struct.pack('<I',1) + struct.pack('<I',disk_total_sectors-1)
    mbr[510:512] = b'\x55\xAA'

    # GPT Header + Partition Entry Array
    gpt_header = bytearray(BYTES_PER_SECTOR)
    gpt_header[0:8] = b'EFI PART'
    gpt_header[8:12] = le32(0x00010000)
    gpt_header[12:16] = le32(92)
    gpt_header[20:28] = le64(disk_total_sectors -1)  # last LBA
    gpt_header[24:32] = le64(1)  # current LBA
    gpt_header[32:40] = le64(2)  # backup LBA
    gpt_header[40:48] = le64(34) # first usable LBA
    gpt_header[48:56] = le64(disk_total_sectors - 34)
    gpt_header[56:72] = DISK_GUID
    part_entry_lba = 2
    gpt_header[72:80] = le64(part_entry_lba)
    gpt_header[80:84] = le32(128)  # size of entry
    gpt_header[84:88] = le32(128)  # number of entries
    # We skip CRC computations (some firmware tolerate missing/zero CRC)

    part_entry_array = bytearray(BYTES_PER_SECTOR*32)  # space for entries
    # Single partition entry
    first_lba = PARTITION_START_LBA
    last_lba  = PARTITION_START_LBA + partition_sectors -1
    entry = bytearray(128)
    entry[0:16] = EFI_SYSTEM_GUID
    entry[16:32] = PART_GUID
    entry[32:40] = le64(first_lba)
    entry[40:48] = le64(last_lba)
    entry[48:56] = le64(0)  # attributes
    name = 'SECOS EFI'.encode('utf-16le')
    entry[56:56+len(name)] = name
    part_entry_array[0:128] = entry

    # Backup GPT header
    backup_header = bytearray(BYTES_PER_SECTOR)
    backup_header[0:8] = b'EFI PART'
    backup_header[8:12] = le32(0x00010000)
    backup_header[12:16] = le32(92)
    backup_header[20:28] = le64(disk_total_sectors -1)
    backup_header[24:32] = le64(disk_total_sectors -2)
    backup_header[32:40] = le64(2)  # backup header points to primary header LBA
    backup_header[40:48] = le64(34)
    backup_header[48:56] = le64(disk_total_sectors - 34)
    backup_header[56:72] = DISK_GUID
    backup_header[72:80] = le64(disk_total_sectors - 33)
    backup_header[80:84] = le32(128)
    backup_header[84:88] = le32(128)

    disk = bytearray(disk_total_sectors*BYTES_PER_SECTOR)
    disk[0:BYTES_PER_SECTOR] = mbr
    disk[1*BYTES_PER_SECTOR:(1+1)*BYTES_PER_SECTOR] = gpt_header
    disk[2*BYTES_PER_SECTOR:(2+len(part_entry_array)//BYTES_PER_SECTOR)*BYTES_PER_SECTOR] = part_entry_array
    # Place partition
    part_offset = PARTITION_START_LBA * BYTES_PER_SECTOR
    disk[part_offset:part_offset+len(part)] = part
    # Backup entries and header at end
    backup_entries_offset = (disk_total_sectors - 33) * BYTES_PER_SECTOR
    disk[backup_entries_offset:backup_entries_offset+len(part_entry_array)] = part_entry_array
    backup_header_offset = (disk_total_sectors -2) * BYTES_PER_SECTOR
    disk[backup_header_offset:backup_header_offset+BYTES_PER_SECTOR] = backup_header

    with open(outimg,'wb') as out:
        out.write(disk)
    print(f"Created GPT FAT disk {outimg} size={len(disk)//1024}KB sectors={disk_total_sectors} part_sectors={partition_sectors}")

if __name__=='__main__':
    if len(sys.argv) < 3:
        print("Usage: mkgptfat.py <bootx64.efi> <disk.img> [kernel.elf]", file=sys.stderr)
        sys.exit(1)
    bootfile=sys.argv[1]; diskimg=sys.argv[2]; kern=None
    if len(sys.argv) >3: kern=sys.argv[3]
    build_disk(bootfile, diskimg, kern)
