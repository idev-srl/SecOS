#!/usr/bin/env python3
"""
Fix PE32+ relocation table for UEFI bootloaders.
Adds a minimal but valid .reloc section.
"""
import struct
import sys

def add_reloc_section(filename):
    with open(filename, 'rb') as f:
        data = bytearray(f.read())
    
    # Parse PE header
    e_lfanew = struct.unpack('<I', data[0x3c:0x40])[0]
    print(f"PE header at: 0x{e_lfanew:x}")
    
    # PE signature
    if data[e_lfanew:e_lfanew+4] != b'PE\x00\x00':
        print("Not a valid PE file!")
        return False
    
    # COFF header
    machine = struct.unpack('<H', data[e_lfanew+4:e_lfanew+6])[0]
    num_sections = struct.unpack('<H', data[e_lfanew+6:e_lfanew+8])[0]
    print(f"Machine: 0x{machine:x}, Sections: {num_sections}")
    
    # Optional header size
    opt_size = struct.unpack('<H', data[e_lfanew+20:e_lfanew+22])[0]
    opt_start = e_lfanew + 24
    
    # Get number of RVA and sizes
    num_rva = struct.unpack('<I', data[opt_start+92:opt_start+96])[0]
    print(f"Number of RVA entries: {num_rva}")
    
    # Data directories start at opt_start + 96
    # Each entry is 8 bytes (RVA, Size)
    
    # Find sections
    section_start = opt_start + opt_size
    sections = []
    for i in range(num_sections):
        sec_offset = section_start + i * 40
        name = data[sec_offset:sec_offset+8].rstrip(b'\x00').decode('ascii', errors='ignore')
        vsize = struct.unpack('<I', data[sec_offset+8:sec_offset+12])[0]
        vaddr = struct.unpack('<I', data[sec_offset+12:sec_offset+16])[0]
        psize = struct.unpack('<I', data[sec_offset+16:sec_offset+20])[0]
        paddr = struct.unpack('<I', data[sec_offset+20:sec_offset+24])[0]
        sections.append((name, vsize, vaddr, psize, paddr, sec_offset))
        print(f"  {name}: VA=0x{vaddr:x} Size=0x{vsize:x} PA=0x{paddr:x} PSize=0x{psize:x}")
    
    # Find last section
    if not sections:
        print("No sections found!")
        return False
    
    last_sec_name, last_vsize, last_vaddr, last_psize, last_paddr, last_sec_offset = sections[-1]
    
    # Calculate where .reloc should go
    reloc_vaddr = last_vaddr + last_vsize
    reloc_vaddr = (reloc_vaddr + 0xfff) & ~0xfff  # Align to 4KB
    reloc_paddr = last_paddr + last_psize
    
    # Create minimal reloc section (8 bytes = reloc base page + entry)
    reloc_data = struct.pack('<I', 0x1000) + struct.pack('<I', 0)  # Dummy reloc entry
    reloc_size = len(reloc_data)
    
    print(f"\nAdding .reloc section:")
    print(f"  VA: 0x{reloc_vaddr:x}")
    print(f"  PA: 0x{reloc_paddr:x}")  
    print(f"  Size: 0x{reloc_size:x}")
    
    # Append reloc data
    data.extend(reloc_data)
    
    # Modify last section header to make room for new section
    # For now, just try to update existing structure
    print("\nWARNING: Relocation support requires full PE rewrite")
    print("Falling back to existing approach...")
    
    return False

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: fix_pe_reloc.py <bootloader.efi>")
        sys.exit(1)
    
    filename = sys.argv[1]
    if not add_reloc_section(filename):
        print("Could not fix PE file")
        sys.exit(1)
    
    print("PE file fixed!")
