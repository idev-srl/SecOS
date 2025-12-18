#!/usr/bin/env python3
"""
inspect_esp.py
==============

Parse a GPT disk image created by build_gpt_esp.py (or other tool), locate the EFI System
Partition (ESP), parse its FAT32 structures, and enumerate directory entries:
  - Root directory
  - EFI directory
  - EFI/BOOT directory

Outputs a human-readable listing including short names, attributes, size, starting cluster, and cluster chain length.

Usage:
  python3 tools/inspect_esp.py dist/esp_gpt.img

Optional flags:
  --dump-fat N    Dump first N FAT32 entries (default 64)
  --limit-chain M Limit cluster chain traversal per file (safety)

Limitations:
  - FAT32 only (no FAT16/FAT12)
  - Short 8.3 names only (no LFN parsing)
  - Assumes partition alignment at 1MiB (start LBA 2048) if only one partition.
  - Minimal validation; not a full filesystem checker.
"""
from __future__ import annotations
import sys, struct, argparse, uuid

SECTOR_SIZE = 512

def read(path: str) -> bytes:
    with open(path, 'rb') as f:
        return f.read()

def parse_gpt(disk: bytes):
    # Primary header at LBA 1
    hdr = disk[SECTOR_SIZE:2*SECTOR_SIZE]
    if hdr[0:8] != b'EFI PART':
        raise SystemExit('No GPT signature at LBA1')
    rev = struct.unpack('<I', hdr[8:12])[0]
    ents_lba = struct.unpack('<Q', hdr[72:80])[0]
    num_ents = struct.unpack('<I', hdr[80:84])[0]
    ent_size = struct.unpack('<I', hdr[84:88])[0]
    if ent_size != 128:
        raise SystemExit(f'Unexpected GPT entry size {ent_size}')
    entries_off = ents_lba * SECTOR_SIZE
    entries_len = num_ents * ent_size
    entries = disk[entries_off:entries_off+entries_len]
    esp = None
    for i in range(num_ents):
        e = entries[i*128:(i+1)*128]
        if e == b'\x00' * 128:
            continue
        part_type_raw = e[0:16]
        # Convert GUID little-endian fields back
        def guid_from_le(raw: bytes) -> uuid.UUID:
            tl, tm, th = struct.unpack('<I', raw[0:4]), struct.unpack('<H', raw[4:6]), struct.unpack('<H', raw[6:8])
            rest = raw[8:]
            b = struct.pack('>I', tl[0]) + struct.pack('>H', tm[0]) + struct.pack('>H', th[0]) + rest
            return uuid.UUID(bytes=b)
        part_type = guid_from_le(part_type_raw)
        start_lba = struct.unpack('<Q', e[32:40])[0]
        end_lba = struct.unpack('<Q', e[40:48])[0]
        name_utf16 = e[56:128]
        # Trim zeros and decode
        name = name_utf16.decode('utf-16le', errors='ignore').rstrip('\x00')
        if str(part_type).upper() == 'C12A7328-F81F-11D2-BA4B-00A0C93EC93B':
            esp = (start_lba, end_lba, name)
            break
    if not esp:
        raise SystemExit('EFI System Partition not found in GPT entries')
    return esp

def parse_fat32(disk: bytes, part_start_lba: int):
    boot_off = part_start_lba * SECTOR_SIZE
    bpb = disk[boot_off:boot_off+SECTOR_SIZE]
    if bpb[0:3] not in (b'\xEB\x58\x90', b'\xEB\x3C\x90', b'\xEB\x76\x90'):
        raise SystemExit('Unexpected jump instruction in boot sector')
    bytes_per_sec = struct.unpack('<H', bpb[11:13])[0]
    spc = bpb[13]
    rsvd = struct.unpack('<H', bpb[14:16])[0]
    fats = bpb[16]
    fatsz32 = struct.unpack('<I', bpb[36:40])[0]
    root_cluster = struct.unpack('<I', bpb[42:46])[0]
    fsinfo_sector = struct.unpack('<H', bpb[46:48])[0]
    backup_boot_sector = struct.unpack('<H', bpb[48:50])[0]
    tot_sec32 = struct.unpack('<I', bpb[32:36])[0]
    if bytes_per_sec != SECTOR_SIZE:
        raise SystemExit(f'BytesPerSec {bytes_per_sec} != 512')
    first_fat_sector = rsvd
    first_data_sector = rsvd + fats * fatsz32
    return {
        'spc': spc,
        'reserved': rsvd,
        'fats': fats,
        'fatsz32': fatsz32,
        'root_cluster': root_cluster,
        'fsinfo_sector': fsinfo_sector,
        'backup_boot_sector': backup_boot_sector,
        'total_sectors': tot_sec32,
        'first_data_sector': first_data_sector,
        'first_fat_sector': first_fat_sector,
        'boot_off': boot_off
    }

def load_fat(disk: bytes, fat_info: dict, part_start_lba: int, dump_count: int):
    fat_sector = part_start_lba + fat_info['first_fat_sector']
    fat_bytes = fat_info['fatsz32'] * SECTOR_SIZE
    fat_off = fat_sector * SECTOR_SIZE
    fat = disk[fat_off:fat_off+fat_bytes]
    entries = []
    for i in range(0, len(fat), 4):
        entries.append(struct.unpack('<I', fat[i:i+4])[0] & 0x0FFFFFFF)
    print(f"FAT[0..{dump_count-1}]:", ' '.join(f"{entries[i]:08X}" for i in range(min(dump_count, len(entries)))))
    return entries

def cluster_to_offset(cluster: int, fat_info: dict, part_start_lba: int):
    spc = fat_info['spc']
    first_data_sector = fat_info['first_data_sector']
    sector = part_start_lba + first_data_sector + (cluster - 2) * spc
    return sector * SECTOR_SIZE

def read_cluster(disk: bytes, cluster: int, fat_info: dict, part_start_lba: int):
    off = cluster_to_offset(cluster, fat_info, part_start_lba)
    size = fat_info['spc'] * SECTOR_SIZE
    return disk[off:off+size]

def parse_dir(disk: bytes, start_cluster: int, fat_entries: list[int], fat_info: dict, part_start_lba: int, limit_chain: int):
    clusters = []
    c = start_cluster
    steps = 0
    while c < 0x0FFFFFF8 and steps < limit_chain:
        clusters.append(c)
        nxt = fat_entries[c]
        if nxt >= 0x0FFFFFF8:
            break
        c = nxt
        steps += 1
    entries = []
    for cl in clusters:
        data = read_cluster(disk, cl, fat_info, part_start_lba)
        for i in range(0, len(data), 32):
            slot = data[i:i+32]
            if slot[0] == 0x00:
                return entries
            if slot[0] == 0xE5:  # deleted
                continue
            attr = slot[11]
            name = slot[0:11].decode('ascii', errors='replace')
            start_cluster = (struct.unpack('<H', slot[20:22])[0] << 16) | struct.unpack('<H', slot[26:28])[0]
            size = struct.unpack('<I', slot[28:32])[0]
            entries.append({
                'name': name,
                'attr': attr,
                'cluster': start_cluster,
                'size': size
            })
    return entries

def attr_str(a: int) -> str:
    flags = []
    if a & 0x10: flags.append('DIR')
    if a & 0x20: flags.append('ARCH')
    if a & 0x01: flags.append('RO')
    if a & 0x02: flags.append('HID')
    if a & 0x04: flags.append('SYS')
    if a & 0x08: flags.append('VOL')
    return ','.join(flags) if flags else 'FILE'

def main(argv=None):
    ap = argparse.ArgumentParser(description='Inspect GPT + FAT32 ESP image')
    ap.add_argument('image', help='Disk image path')
    ap.add_argument('--dump-fat', type=int, default=64, help='How many FAT entries to dump')
    ap.add_argument('--limit-chain', type=int, default=1024, help='Max clusters to traverse per directory chain')
    args = ap.parse_args(argv)
    disk = read(args.image)
    start_lba, end_lba, name = parse_gpt(disk)
    print(f"ESP Partition: start LBA={start_lba} end LBA={end_lba} name='{name}' sectors={end_lba - start_lba + 1}")
    fat_info = parse_fat32(disk, start_lba)
    print("FAT32 BPB:")
    for k in ['spc','reserved','fats','fatsz32','root_cluster','fsinfo_sector','backup_boot_sector','total_sectors','first_data_sector','first_fat_sector']:
        print(f"  {k}: {fat_info[k]}")
    # Verify FSInfo signatures
    fsinfo_off = (start_lba + fat_info['fsinfo_sector']) * SECTOR_SIZE
    fsinfo = disk[fsinfo_off:fsinfo_off+SECTOR_SIZE]
    lead = fsinfo[0:4]
    struct_sig = fsinfo[484:488]
    trail = fsinfo[508:510]
    print(f"FSInfo lead={lead.hex()} struct={struct_sig.hex()} trail={trail.hex()}")
    fat_entries = load_fat(disk, fat_info, start_lba, args.dump_fat)
    # Root
    root_entries = parse_dir(disk, fat_info['root_cluster'], fat_entries, fat_info, start_lba, args.limit_chain)
    print("Root directory entries:")
    for e in root_entries:
        print(f"  {e['name']} attr={attr_str(e['attr'])} cluster={e['cluster']} size={e['size']}")
    # Find EFI dir
    efi_entry = next((x for x in root_entries if x['attr'] & 0x10 and x['name'].startswith('EFI')), None)
    if efi_entry:
        efi_entries = parse_dir(disk, efi_entry['cluster'], fat_entries, fat_info, start_lba, args.limit_chain)
        print("EFI directory entries:")
        for e in efi_entries:
            print(f"  {e['name']} attr={attr_str(e['attr'])} cluster={e['cluster']} size={e['size']}")
        boot_entry = next((x for x in efi_entries if x['attr'] & 0x10 and x['name'].startswith('BOOT')), None)
        if boot_entry:
            boot_entries = parse_dir(disk, boot_entry['cluster'], fat_entries, fat_info, start_lba, args.limit_chain)
            print("EFI/BOOT directory entries:")
            for e in boot_entries:
                print(f"  {e['name']} attr={attr_str(e['attr'])} cluster={e['cluster']} size={e['size']}")
        else:
            print("[WARN] BOOT directory not found under EFI")
    else:
        print("[WARN] EFI directory not found in root")
    return 0

if __name__ == '__main__':
    sys.exit(main())
