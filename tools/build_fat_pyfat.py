#!/usr/bin/env python3
"""
Create a FAT32 image using pyfatfs so that UEFI firmware/Ovmf can parse it.
Populates /EFI/BOOT/BOOTX64.EFI and optional kernel.elf.
"""
import sys, os, io
from pyfatfs import PyFat

IMAGE_SIZE_MB = int(os.environ.get('FAT_MB', '32'))
BYTES_PER_SECTOR = 512

def main():
    if len(sys.argv) < 3:
        print("Usage: build_fat_pyfat.py <bootx64.efi> <out.img> [kernel.elf]", file=sys.stderr)
        sys.exit(1)
    boot = sys.argv[1]; outimg = sys.argv[2]; kernel = sys.argv[3] if len(sys.argv)>3 else None
    with open(boot,'rb') as f: boot_data=f.read()
    kernel_data=b''
    if kernel and os.path.exists(kernel):
        with open(kernel,'rb') as kf: kernel_data=kf.read()
    size_bytes = IMAGE_SIZE_MB*1024*1024
    # Create empty image file
    with open(outimg,'wb') as img:
        img.truncate(size_bytes)
    # Initialize FAT32 filesystem
    pf = PyFat.FAT32()
    with open(outimg,'r+b') as img:
        pf.mkfs(img, volume_label='SECOS', size=size_bytes, fat_count=2)
        # Create directories
        pf.mkdir('/EFI')
        pf.mkdir('/EFI/BOOT')
        pf.write('/EFI/BOOT/BOOTX64.EFI', boot_data)
        if kernel_data:
            pf.write('/kernel.elf', kernel_data)
    print(f"Created FAT32 image {outimg} size={IMAGE_SIZE_MB}MB with BOOTX64.EFI (pyfatfs)")

if __name__=='__main__':
    main()
