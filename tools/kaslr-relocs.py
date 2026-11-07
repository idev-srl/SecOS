#!/usr/bin/env python3
"""kaslr-relocs.py — build the kernel KASLR relocation table (M45).

The kernel is linked once with `ld --emit-relocs` (pass 1). This tool walks the
kept relocation sections and records every ABSOLUTE reference (R_X86_64_64,
R_X86_64_32S, R_X86_64_32) whose SYMBOL lies in the slid high-half image: those
are the fields the boot relocator must add the slide to. The symbol, not S+A,
decides: `&buf - KERNEL_VMA` folded into an addend still moves with the image.
PC-relative relocations (PC32/PLT32/...) need no fixup because the whole image
moves as one block; absolute references to LOW (.boot) symbols stay as they are.

The table (format in boot/kaslr.h) is linked back into the kernel as section
`.kaslr`, placed after .data and before .bss, so no section that carries
relocations moves between pass 1 and pass 2. `check` re-extracts the table
from the pass-2 link and requires it to be byte-identical.

Usage:
  kaslr-relocs.py extract <pass1.elf> <out.bin> [--disabled]
  kaslr-relocs.py check   <pass2.elf> <table.bin>
  kaslr-relocs.py boot-only <kaslr_core.o>   (sanity check of the MB2 core)

Deterministic (sorted output, no timestamps): reproducible builds keep working.
Python 3 standard library only.

Copyright (c) 2026 iDev srl
SPDX-License-Identifier: MIT
"""
import struct
import sys

KASLR_MAGIC = 0x4C53414B
KASLR_VERSION = 2
LINK_BASE = 0xFFFFFFFF80000000
ALIGN = 2 << 20
WINDOW = 512 << 20
SLOTS = 768
F_DISABLED = 0x0001
ENT_32 = 0x80000000

R_X86_64_NONE = 0
R_X86_64_64 = 1
R_X86_64_PC32 = 2
R_X86_64_PLT32 = 4
R_X86_64_32 = 10
R_X86_64_32S = 11
R_X86_64_PC64 = 24
R_X86_64_GOTPCREL = 9
R_X86_64_GOTPCRELX = 41
R_X86_64_REX_GOTPCRELX = 42
PC_RELATIVE = {R_X86_64_PC32, R_X86_64_PLT32, R_X86_64_PC64}

SHT_SYMTAB = 2
SHT_RELA = 4
SHF_ALLOC = 0x2
SHN_UNDEF = 0
SHN_ABS = 0xFFF1
PT_LOAD = 1


def die(msg):
    sys.stderr.write("kaslr-relocs: error: %s\n" % msg)
    sys.exit(1)


class Elf:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.d = f.read()
        d = self.d
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
            die("%s: not an ELF64 little-endian file" % path)
        (self.e_type, self.e_machine, _, self.e_entry, self.e_phoff, self.e_shoff, _,
         _, self.e_phentsize, self.e_phnum, self.e_shentsize, self.e_shnum,
         self.e_shstrndx) = struct.unpack_from("<HHIQQQIHHHHHH", d, 16)
        if self.e_machine != 0x3E:
            die("%s: not x86-64" % path)
        self.sh = []
        for i in range(self.e_shnum):
            o = self.e_shoff + i * self.e_shentsize
            (name, typ, flags, addr, off, size, link, info, align, entsize) = \
                struct.unpack_from("<IIQQQQIIQQ", d, o)
            self.sh.append(dict(name=name, type=typ, flags=flags, addr=addr, off=off,
                                size=size, link=link, info=info, entsize=entsize))
        shstr = self.sh[self.e_shstrndx]
        for s in self.sh:
            s["sname"] = self.cstr(shstr["off"] + s["name"])
        self.ph = []
        for i in range(self.e_phnum):
            o = self.e_phoff + i * self.e_phentsize
            (typ, flags, off, vaddr, paddr, filesz, memsz, align) = \
                struct.unpack_from("<IIQQQQQQ", d, o)
            self.ph.append(dict(type=typ, off=off, vaddr=vaddr, paddr=paddr,
                                filesz=filesz, memsz=memsz))

    def cstr(self, off):
        end = self.d.index(b"\0", off)
        return self.d[off:end].decode("ascii", "replace")

    def va_to_phys(self, va, width):
        for p in self.ph:
            if p["type"] != PT_LOAD:
                continue
            if p["vaddr"] <= va and va + width <= p["vaddr"] + p["filesz"]:
                return p["paddr"] + (va - p["vaddr"])
        die("relocated field at VA 0x%x is not in a loaded file-backed segment" % va)

    def symbols(self, idx):
        s = self.sh[idx]
        out = []
        for k in range(s["size"] // 24):
            (name, info, other, shndx, value, size) = \
                struct.unpack_from("<IBBHQQ", self.d, s["off"] + k * 24)
            out.append((shndx, value, name))
        return out


def extract(path):
    elf = Elf(path)
    symcache = {}
    entries = {}
    n_abs_high = 0
    for rs in elf.sh:
        if rs["type"] != SHT_RELA:
            continue
        target = elf.sh[rs["info"]]
        if not (target["flags"] & SHF_ALLOC):
            continue
        if rs["link"] not in symcache:
            symcache[rs["link"]] = elf.symbols(rs["link"])
        syms = symcache[rs["link"]]
        for k in range(rs["size"] // 24):
            r_offset, r_info, r_addend = struct.unpack_from("<QQq", elf.d, rs["off"] + k * 24)
            rtype = r_info & 0xFFFFFFFF
            symi = r_info >> 32
            if rtype == R_X86_64_NONE:
                continue
            if rtype in PC_RELATIVE:
                # S+A-P is slide-invariant only if S and P move together.
                shndx, svalue, _ = syms[symi]
                if shndx != SHN_ABS and shndx != SHN_UNDEF and \
                        (svalue >= LINK_BASE) != (r_offset >= LINK_BASE):
                    die("PC-relative reference across the low/high boundary at 0x%x" % r_offset)
                continue
            if rtype in (R_X86_64_GOTPCREL, R_X86_64_GOTPCRELX, R_X86_64_REX_GOTPCRELX):
                die("GOT relocation in a -fno-pic kernel at 0x%x" % r_offset)
            if rtype not in (R_X86_64_64, R_X86_64_32S, R_X86_64_32):
                die("unsupported relocation type %d at 0x%x (section %s)"
                    % (rtype, r_offset, target["sname"]))
            shndx, svalue, _ = syms[symi]
            value = (svalue + r_addend) & 0xFFFFFFFFFFFFFFFF
            width = 8 if rtype == R_X86_64_64 else 4
            # Cross-check against the linked bytes (validates the parse).
            foff = target["off"] + (r_offset - target["addr"])
            if rtype == R_X86_64_64:
                stored = struct.unpack_from("<Q", elf.d, foff)[0]
            elif rtype == R_X86_64_32S:
                stored = struct.unpack_from("<i", elf.d, foff)[0] & 0xFFFFFFFFFFFFFFFF
            else:
                stored = struct.unpack_from("<I", elf.d, foff)[0]
            if stored != value:
                die("field at 0x%x holds 0x%x, expected S+A=0x%x" % (r_offset, stored, value))
            # Classify by the SYMBOL, not by S+A: the compiler folds expressions
            # such as `(uint64_t)&buf - KERNEL_VMA_BASE` (or a physmap offset)
            # into the addend, so S+A can be a small or wrapped value while it
            # still moves with the image (it is S+slide+A at run time).
            if shndx == SHN_ABS:
                if svalue >= LINK_BASE:
                    n_abs_high += 1            # an absolute constant, not an address
                continue
            if svalue < LINK_BASE:
                continue                       # low (.boot) symbol: does not move
            if svalue > LINK_BASE + WINDOW:
                die("reference to 0x%x at 0x%x is outside the 512 MiB window"
                    % (svalue, r_offset))
            phys = elf.va_to_phys(r_offset, width)
            if phys > 0x7FFFFFFF - 8:
                die("field physical address 0x%x does not fit the table" % phys)
            if phys in entries:
                die("duplicate relocation at 0x%x" % r_offset)
            entries[phys] = ENT_32 if width == 4 else 0
    out = sorted(entries.items())
    for (a, fa), (b, _) in zip(out, out[1:]):
        if a + (4 if fa else 8) > b:
            die("overlapping relocated fields at phys 0x%x / 0x%x" % (a, b))
    return [a | f for a, f in out], n_abs_high, guard_phys(elf)


def guard_phys(elf):
    """Physical address of __stack_chk_guard (the boot relocator randomizes it),
    or 0 if the kernel has none. It must sit in a file-backed section that does
    not move between the passes (.data)."""
    for idx, s in enumerate(elf.sh):
        if s["type"] != SHT_SYMTAB:
            continue
        strtab = elf.sh[s["link"]]
        for shndx, value, name in elf.symbols(idx):
            if shndx in (SHN_UNDEF, SHN_ABS) or shndx >= len(elf.sh):
                continue
            if elf.cstr(strtab["off"] + name) != "__stack_chk_guard":
                continue
            if elf.sh[shndx]["sname"] != ".data":
                die("__stack_chk_guard is in %s, expected .data" % elf.sh[shndx]["sname"])
            return elf.va_to_phys(value, 8)
    return 0


def build_table(entries, disabled, guard):
    flags = F_DISABLED if disabled else 0
    hdr = struct.pack("<IHHIIQQQIIII", KASLR_MAGIC, KASLR_VERSION, flags, len(entries),
                      SLOTS, LINK_BASE, ALIGN, 0, 0, 0, guard, 0)
    assert len(hdr) == 56
    return hdr + b"".join(struct.pack("<I", e) for e in entries)


def main(argv):
    if len(argv) >= 3 and argv[0] == "extract":
        disabled = "--disabled" in argv[3:]
        entries, n_abs, guard = extract(argv[1])
        table = build_table(entries, disabled, guard)
        with open(argv[2], "wb") as f:
            f.write(table)
        n32 = sum(1 for e in entries if e & ENT_32)
        print("[kaslr] %d relocations (%d x 64-bit, %d x 32S), table %d bytes, guard %s%s"
              % (len(entries), len(entries) - n32, n32, len(table),
                 ("@0x%x" % guard) if guard else "none",
                 ", KASLR disabled at build time" if disabled else ""))
        return 0
    if len(argv) == 3 and argv[0] == "check":
        with open(argv[2], "rb") as f:
            want = f.read()
        disabled = bool(struct.unpack_from("<H", want, 6)[0] & F_DISABLED)
        entries, _, guard = extract(argv[1])
        if build_table(entries, disabled, guard) != want:
            die("pass-2 relocations differ from the embedded table (layout moved)")
        elf = Elf(argv[1])
        ks = [s for s in elf.sh if s["sname"] == ".kaslr"]
        if len(ks) != 1 or elf.d[ks[0]["off"]:ks[0]["off"] + len(want)] != want:
            die("section .kaslr does not hold the table")
        return 0
    if len(argv) == 2 and argv[0] == "boot-only":
        # The MB2 relocator object runs from the low .boot section before the
        # high half exists: every allocated section must be a .boot one and
        # nothing may be left undefined (no call into the high-half kernel).
        elf = Elf(argv[1])
        bad = [s["sname"] for s in elf.sh
               if (s["flags"] & SHF_ALLOC) and not s["sname"].startswith(".boot")]
        if bad:
            die("%s: allocated non-.boot sections: %s" % (argv[1], ", ".join(bad)))
        for s in elf.sh:
            if s["type"] == SHT_SYMTAB:
                for shndx, _, name in elf.symbols(elf.sh.index(s))[1:]:
                    if shndx == SHN_UNDEF:
                        die("%s: undefined symbol %s"
                            % (argv[1], elf.cstr(elf.sh[s["link"]]["off"] + name)))
        return 0
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
