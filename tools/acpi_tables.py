#!/usr/bin/env python3
"""
tools/acpi_tables.py - synthesize ACPI DMAR / IVRS tables for the M53b parser

    tools/acpi_tables.py gen OUTDIR            write the seed tables (*.bin)
    tools/acpi_tables.py check HARNESS OUTDIR  gen, then run `HARNESS dump`
                                               on each table and compare the
                                               parsed summary with what was
                                               encoded here

The tables follow the Intel VT-d spec (chapter 8, DMAR) and the AMD IOMMU
spec (section 5, IVRS). They are seeds for tools/acpi_fuzz and a known-answer
test for arch/x86/acpi_dmar.c. The `S ...` summary lines printed by
`acpi_fuzz dump` are matched exactly (order included).

The PCI topology the harness answers config reads with (see acpi_fuzz.c):
    00:00.0 host bridge           00:02.0 display (class 03)
    00:14.0 USB controller        00:1c.0 root port, buses 01-02
    01:00.0 bridge, buses 02-02   02:00.0 NIC
    00:1f.0 ISA bridge            (everything else absent)

Copyright (c) 2026 iDev srl
Author: Luigi De Astis <l.deastis@idev-srl.com>
SPDX-License-Identifier: MIT
"""
import os
import struct
import subprocess
import sys


def acpi_table(sig, body, rev=1):
    """36-byte SDT header + body, checksum fixed."""
    length = 36 + len(body)
    hdr = struct.pack("<4sIBB6s8sI4sI", sig, length, rev, 0, b"SECOS ",
                      b"SECOSTBL", 1, b"SCOS", 1)
    t = bytearray(hdr + body)
    t[9] = (-sum(t)) & 0xFF
    return bytes(t)


# ---------------------------------------------------------------- DMAR
def scope(stype, path, enum_id=0, start_bus=0):
    b = struct.pack("<BBHBB", stype, 6 + 2 * len(path), 0, enum_id, start_bus)
    for d, f in path:
        b += struct.pack("<BB", d, f)
    return b


EP, BR, IOAPIC, HPET, NS = 1, 2, 3, 4, 5


def drhd(base, flags=0, seg=0, scopes=b"", size=0):
    return struct.pack("<HHBBHQ", 0, 16 + len(scopes), flags, size, seg, base) + scopes


def rmrr(base, limit, seg=0, scopes=b""):
    return struct.pack("<HHHHQQ", 1, 24 + len(scopes), 0, seg, base, limit) + scopes


def atsr(flags=0, seg=0, scopes=b""):
    return struct.pack("<HHBBH", 2, 8 + len(scopes), flags, 0, seg) + scopes


def rhsa(base, prox):
    return struct.pack("<HHIQI", 3, 20, 0, base, prox)


def andd(devnum, name):
    n = name.encode() + b"\0"
    return struct.pack("<HH3sB", 4, 8 + len(n), b"\0\0\0", devnum) + n


def satc(flags=0, seg=0, scopes=b""):
    return struct.pack("<HHBBH", 5, 8 + len(scopes), flags, 0, seg) + scopes


def dmar(haw, flags, structs):
    body = struct.pack("<BB10s", haw - 1, flags, b"\0" * 10) + b"".join(structs)
    return acpi_table(b"DMAR", body)


# ---------------------------------------------------------------- IVRS
def bdf(b, d, f):
    return (b << 8) | (d << 3) | f


def dev4(t, devid, dte=0):
    return struct.pack("<BHB", t, devid, dte)


def alias(t, devid, al, dte=0):
    return struct.pack("<BHBBHB", t, devid, dte, 0, al, 0)


def ext(t, devid, data, dte=0):
    return struct.pack("<BHBI", t, devid, dte, data)


def special(handle, devid, variety, dte=0):
    return struct.pack("<BHBBHB", 0x48, 0, dte, handle, devid, variety)


def acpihid(devid, hid, cid, uid):
    return struct.pack("<BHB8s8sBB", 0xF0, devid, 0, hid, cid, 2 if uid else 0,
                       len(uid)) + uid


def ivhd(t, devid, base, entries, flags=0, seg=0, info=0, attr=0, efr=0, efr2=0):
    e = b"".join(entries)
    if t == 0x10:
        hdr = struct.pack("<BBHHHQHHI", t, flags, 24 + len(e), devid, 0x40,
                          base, seg, info, attr)
    else:
        hdr = struct.pack("<BBHHHQHHIQQ", t, flags, 40 + len(e), devid, 0x40,
                          base, seg, info, attr, efr, efr2)
    return hdr + e


def ivmd(t, devid, aux, base, length, flags=0):
    return struct.pack("<BBHHHQQQ", t, flags, 32, devid, aux, 0, base, length)


def ivrs(ivinfo, structs):
    body = struct.pack("<I8s", ivinfo, b"\0" * 8) + b"".join(structs)
    return acpi_table(b"IVRS", body, rev=2)


# ------------------------------------------------------------- the seeds
# Each entry: name -> (bytes, expected summary lines from `acpi_fuzz dump`).
def tables():
    t = {}
    # QEMU 6.2/9.2 q35 + intel-iommu: one DRHD with explicit scopes.
    t["dmar_qemu"] = (dmar(39, 1, [drhd(0xFED90000, 0, 0, b"".join([
        scope(IOAPIC, [(0, 0)], enum_id=0, start_bus=0xFF),
        scope(EP, [(0, 0)]), scope(EP, [(2, 0)]), scope(EP, [(0x1F, 0)]),
        scope(BR, [(0x1C, 0)])]))]),
        ["S DMAR haw=39 flags=0x1 drhd=1 rmrr=0 atsr=0 rhsa=0 satc=0 andd=0 inv=0 drop=0 trunc=0 csum=0",
         "S DRHD 0 base=0xfed90000 seg=0 all=0 gfx=0 scopes=5 inc=0",
         "S SC ioapic id=0 bdf=ff:00.0 res=1",
         "S SC dev id=0 bdf=00:00.0 res=1",
         "S SC dev id=0 bdf=00:02.0 res=1 disp",
         "S SC dev id=0 bdf=00:1f.0 res=1",
         "S SC bridge id=0 bdf=00:1c.0 res=1 bus=01-02",
         "S FOR 02:00.0 -> 0", "S FOR 00:03.0 -> none", "S IOAPIC0 ff:00.0"])
    # Typical client platform: iGPU unit, catch-all unit, RMRRs, everything.
    t["dmar_laptop"] = (dmar(39, 7, [
        drhd(0xFED90000, 0, 0, scope(EP, [(2, 0)])),
        drhd(0xFED91000, 1, 0, scope(IOAPIC, [(0x1E, 7)], enum_id=2) +
             scope(HPET, [(0x1E, 6)], enum_id=0) + scope(NS, [(0x15, 0)], enum_id=1)),
        rmrr(0x7C000000, 0x7C01FFFF, 0, scope(EP, [(0x14, 0)])),
        rmrr(0x80000000, 0x87FFFFFF, 0, scope(EP, [(2, 0)])),
        atsr(1, 0),
        rhsa(0xFED91000, 3),
        andd(1, "\\_SB.PCI0.I2C0"),
        satc(1, 0, scope(EP, [(2, 0)])),
        ]),
        ["S DMAR haw=39 flags=0x7 drhd=2 rmrr=2 atsr=1 rhsa=1 satc=1 andd=1 inv=0 drop=0 trunc=0 csum=0",
         "S DRHD 0 base=0xfed90000 seg=0 all=0 gfx=1 scopes=1 inc=0",
         "S SC dev id=0 bdf=00:02.0 res=1 disp",
         "S DRHD 1 base=0xfed91000 seg=0 all=1 gfx=0 scopes=3 inc=0 node=3",
         "S SC ioapic id=2 bdf=00:1e.7 res=1",
         "S SC hpet id=0 bdf=00:1e.6 res=1",
         "S SC acpi id=1 bdf=00:15.0 res=1",
         "S RMRR 0 0x7c000000-0x7c01ffff seg=0 scopes=1 inc=0",
         "S SC dev id=0 bdf=00:14.0 res=1",
         "S RMRR 1 0x80000000-0x87ffffff seg=0 scopes=1 inc=0",
         "S SC dev id=0 bdf=00:02.0 res=1 disp",
         "S FOR 02:00.0 -> 1", "S FOR 00:02.0 -> 0", "S RMRRCOV 00:02.0 -> 1",
         "S RMRRCOV 00:14.0 -> 0", "S HPET0 00:1e.6", "S IOAPIC2 00:1e.7"])
    # Scopes through bridges: 00:1c.0 -> 01:00.0 -> 02:00.0, an absent
    # device, a broken path (bridge missing), and an over-deep path.
    t["dmar_bridges"] = (dmar(48, 0, [
        drhd(0xFED92000, 0, 0, scope(EP, [(0x1C, 0), (0, 0), (0, 0)]) +
             scope(BR, [(0x1C, 0), (0, 0)]) +
             scope(EP, [(5, 0)]) + scope(EP, [(6, 0), (0, 0)]) +
             scope(EP, [(0x1C, 0)] * 9)),
        drhd(0xFED93000, 1, 1),
        drhd(0xFED94000, 1, 0),
        drhd(0xFED95000, 1, 0),
        ]),
        ["S DMAR haw=48 flags=0x0 drhd=3 rmrr=0 atsr=0 rhsa=0 satc=0 andd=0 inv=2 drop=0 trunc=0 csum=0",
         "S DRHD 0 base=0xfed92000 seg=0 all=0 gfx=0 scopes=4 inc=1",
         "S SC dev id=0 bdf=02:00.0 res=1",
         "S SC bridge id=0 bdf=01:00.0 res=1 bus=02-02",
         "S SC dev id=0 bdf=00:05.0 res=0",
         "S SC dev id=0 bdf=00:00.0 res=0",
         "S DRHD 1 base=0xfed93000 seg=1 all=1 gfx=0 scopes=0 inc=0",
         "S DRHD 2 base=0xfed94000 seg=0 all=1 gfx=0 scopes=0 inc=0",
         "S FOR 02:00.0 -> 0", "S FOR 00:14.0 -> 2"])
    # Invalid entries: unaligned base, bad RMRR, unknown type, short RHSA.
    bad = dmar(39, 0, [
        drhd(0xFED90800, 0, 0),
        drhd(0, 1, 0),
        rmrr(0x1000, 0x0FFF),
        rmrr(0x1000, 0x1FFE),
        struct.pack("<HH", 99, 8) + b"\0" * 4,
        struct.pack("<HHI", 3, 8, 0),
        drhd(0xFED90000, 1, 0),
        ])
    t["dmar_invalid"] = (bad,
        ["S DMAR haw=39 flags=0x0 drhd=1 rmrr=0 atsr=0 rhsa=0 satc=0 andd=0 inv=5 drop=0 trunc=0 csum=0",
         "S DRHD 0 base=0xfed90000 seg=0 all=1 gfx=0 scopes=0 inc=0"])
    # Truncated: header length beyond the buffer, a structure overrunning.
    tr = bytearray(dmar(39, 0, [drhd(0xFED90000, 1, 0), drhd(0xFED91000, 0, 0, scope(EP, [(2, 0)]))]))
    struct.pack_into("<H", tr, 48 + 16 + 2, 200)
    tr[9] = 0
    tr[9] = (-sum(tr)) & 0xFF
    t["dmar_trunc"] = (bytes(tr),
        ["S DMAR haw=39 flags=0x0 drhd=1 rmrr=0 atsr=0 rhsa=0 satc=0 andd=0 inv=0 drop=0 trunc=1 csum=0",
         "S DRHD 0 base=0xfed90000 seg=0 all=1 gfx=0 scopes=0 inc=0"])
    # Many DRHDs: the static limit (16) drops the rest.
    many = dmar(39, 0, [drhd(0xFE000000 + i * 0x1000, 0, 0, scope(EP, [(i % 32, 0)])) for i in range(20)])
    t["dmar_many"] = (many,
        ["S DMAR haw=39 flags=0x0 drhd=16 rmrr=0 atsr=0 rhsa=0 satc=0 andd=0 inv=0 drop=4 trunc=0 csum=0"])

    # QEMU 9.2 amd-iommu: IVHD 10h + 11h with the same entries.
    ents = [dev4(2, bdf(0, 0, 0)), dev4(2, bdf(0, 2, 0)), dev4(3, bdf(1, 0, 0)),
            dev4(4, bdf(1, 31, 7)), alias(0x43, bdf(3, 0, 0), bdf(3, 0, 0)),
            dev4(4, bdf(3, 31, 7)), special(0, bdf(0, 0x14, 0), 1)]
    t["ivrs_qemu"] = (ivrs(0x2801, [ivhd(0x10, bdf(0, 2, 0), 0xFED80000, ents, flags=0xD1),
                                    ivhd(0x11, bdf(0, 2, 0), 0xFED80000, ents, flags=0x11)]),
        ["S IVRS ivinfo=0x2801 ivhd=2 ivmd=0 primary=0x11 inv=0 drop=0 trunc=0 csum=0",
         "S IVHD 0 type=0x10 prim=0 dev=00:02.0 base=0xfed80000 seg=0 n=5",
         "S IVHD 1 type=0x11 prim=1 dev=00:02.0 base=0xfed80000 seg=0 n=5",
         "S DEV select 00:00.0-00:00.0", "S DEV select 00:02.0-00:02.0",
         "S DEV range 01:00.0-01:1f.7", "S DEV alias 03:00.0-03:1f.7 ->03:00.0",
         "S DEV special 00:14.0-00:14.0 v=1 h=0",
         "S LOOKUP 01:05.0 -> 1 alias=01:05.0", "S LOOKUP 03:02.0 -> 1 alias=03:00.0",
         "S LOOKUP 05:00.0 -> none", "S SPECIAL ioapic0 00:14.0"])
    # 10h + 11h + 40h, ALL, ext, ACPI HID, IVMD of each type, bad entries.
    ents40 = [dev4(1, 0), ext(0x46, bdf(0, 3, 0), 0x80000000), ext(0x47, bdf(4, 0, 0), 1),
              dev4(4, bdf(4, 0, 7)), acpihid(bdf(0, 0x14, 5), b"AMDI0020", b"\0" * 8, b"ID00"),
              special(1, bdf(0, 0x14, 0), 2), dev4(4, bdf(9, 0, 0))]
    t["ivrs_full"] = (ivrs(0x2800, [
        ivhd(0x10, bdf(0, 0, 2), 0xFEB80000, [dev4(1, 0)]),
        ivhd(0x40, bdf(0, 0, 2), 0xFEB80000, ents40, efr=0x1234),
        ivhd(0x11, bdf(0, 0, 2), 0xFEB80000, [dev4(1, 0)]),
        ivmd(0x20, 0, 0, 0x9D000000, 0x10000, 0x7),
        ivmd(0x21, bdf(0, 0x12, 0), 0, 0x9D100000, 0x1000, 0x1),
        ivmd(0x22, bdf(0, 0x13, 0), bdf(0, 0x13, 7), 0x9D200000, 0x2000, 0x3),
        ivmd(0x22, bdf(0, 0x13, 7), bdf(0, 0x13, 0), 0x9D300000, 0x1000, 0x3),
        ivhd(0x11, bdf(0, 0, 3), 0xFEB80001, [dev4(1, 0)]),
        ]),
        ["S IVRS ivinfo=0x2800 ivhd=3 ivmd=3 primary=0x40 inv=3 drop=0 trunc=0 csum=0",
         "S IVHD 0 type=0x10 prim=0 dev=00:00.2 base=0xfeb80000 seg=0 n=1",
         "S IVHD 1 type=0x40 prim=1 dev=00:00.2 base=0xfeb80000 seg=0 n=5",
         "S DEV all 00:00.0-ff:1f.7", "S DEV ext 00:03.0-00:03.0",
         "S DEV ext 04:00.0-04:00.7", "S DEV acpihid 00:14.5-00:14.5",
         "S DEV special 00:14.0-00:14.0 v=2 h=1",
         "S IVHD 2 type=0x11 prim=0 dev=00:00.2 base=0xfeb80000 seg=0 n=1",
         "S IVMD 0 type=0x20 00:00.0-ff:1f.7 0x9d000000+0x10000",
         "S IVMD 1 type=0x21 00:12.0-00:12.0 0x9d100000+0x1000",
         "S IVMD 2 type=0x22 00:13.0-00:13.7 0x9d200000+0x2000",
         "S LOOKUP 01:05.0 -> 1 alias=01:05.0", "S SPECIAL hpet1 00:14.0"])
    # Bad checksum: parsed, flagged.
    cs = bytearray(t["ivrs_qemu"][0])
    cs[9] ^= 0x55
    t["ivrs_badcsum"] = (bytes(cs), ["S IVRS ivinfo=0x2801 ivhd=2 ivmd=0 primary=0x11 inv=0 drop=0 trunc=0 csum=1"])
    # Unknown IVHD entry type >= 0x80 (not F0h): the device walk stops.
    t["ivrs_badent"] = (ivrs(0, [ivhd(0x10, 0x10, 0xFED80000, [dev4(2, 0x08), b"\x90\0\0\0", dev4(2, 0x10)])]),
        ["S IVRS ivinfo=0x0 ivhd=1 ivmd=0 primary=0x10 inv=1 drop=0 trunc=1 csum=0",
         "S IVHD 0 type=0x10 prim=1 dev=00:02.0 base=0xfed80000 seg=0 n=1"])
    return t


def gen(outdir):
    os.makedirs(outdir, exist_ok=True)
    tb = tables()
    for name, (data, _) in tb.items():
        with open(os.path.join(outdir, name + ".bin"), "wb") as f:
            f.write(data)
    return tb


def check(harness, outdir):
    tb = gen(outdir)
    fails = 0
    for name, (_, want) in tb.items():
        path = os.path.join(outdir, name + ".bin")
        r = subprocess.run([harness, "dump", path], capture_output=True, text=True)
        got = [ln for ln in r.stdout.splitlines() if ln.startswith("S ")]
        # Expected lines must appear in this order (others may come between).
        missing, at = [], 0
        for w in want:
            try:
                at = got.index(w, at) + 1
            except ValueError:
                missing.append(w)
        if r.returncode != 0 or missing:
            fails += 1
            print(f"FAIL {name} rc={r.returncode}")
            for m in missing:
                print(f"   missing: {m}")
            print("   got:\n     " + "\n     ".join(got))
            if r.stderr.strip():
                print("   stderr: " + r.stderr.strip()[:2000])
        else:
            print(f"ok   {name} ({len(want)} lines)")
    print(f"[acpi_tables] {len(tb) - fails}/{len(tb)} tables OK")
    return 1 if fails else 0


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "gen":
        gen(sys.argv[2])
        return 0
    if len(sys.argv) == 4 and sys.argv[1] == "check":
        return check(sys.argv[2], sys.argv[3])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
