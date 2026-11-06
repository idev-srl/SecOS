#!/usr/bin/env python3
"""tools/exfat_img.py - host-side exFAT image helper for the SecOS M41 tests.

Independent of the kernel driver (written from the public exFAT spec), used to
inspect images and to derive deliberately corrupted variants.

  exfat_img.py dirty IMG              print VolumeDirty (0/1)
  exfat_img.py chain IMG NAME         print stream layout of /NAME (root dir)
  exfat_img.py fragmented IMG NAME    exit 0 iff /NAME is a non-contiguous FAT chain
  exfat_img.py corrupt BASE OUTDIR    write the corrupt-image suite into OUTDIR
  exfat_img.py pattern OUT LEN SEED   write the shared test pattern to OUT
  exfat_img.py checkpat FILE LEN SEED exit 0 iff FILE holds that pattern
  exfat_img.py populate DIR           write the M41 "good" tree into DIR
  exfat_img.py populate-corrupt DIR   write the corrupt-suite base tree into DIR
  exfat_img.py verifytree DIR         check DIR after the SecOS M41 run
  exfat_img.py deepcheck IMG          strict structural check (stricter than
                                      fsck.exfat 1.1.3): bitmap == owned
                                      clusters exactly, no cross-links, every
                                      SetChecksum + NameHash, chain lengths
"""
import os
import shutil
import struct
import sys


def pat_bytes(length, seed, start=0):
    # Must match pat() in fs/exfat_test.c.
    return bytes(((i * 131 + seed * 17 + (i >> 12)) & 0xFF) for i in range(start, start + length))


class Img:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "r+b")
        bs = self.read(0, 512)
        assert bs[3:11] == b"EXFAT   ", "not exFAT"
        self.ss = 1 << bs[108]
        self.cb = self.ss << bs[109]
        self.fat_off, self.fat_len, self.heap_off, self.ccount, self.root = struct.unpack_from("<IIIII", bs, 80)

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def write(self, off, data):
        self.f.seek(off)
        self.f.write(data)

    def cl_off(self, c):
        return (self.heap_off * self.ss) + (c - 2) * self.cb

    def fat(self, c):
        return struct.unpack("<I", self.read(self.fat_off * self.ss + 4 * c, 4))[0]

    def set_fat(self, c, v):
        self.write(self.fat_off * self.ss + 4 * c, struct.pack("<I", v))

    def chain(self, first, n=None, nofat=False):
        if nofat:
            return [first + i for i in range(n)]
        out, c = [], first
        while c != 0xFFFFFFFF and len(out) < self.ccount:
            out.append(c)
            c = self.fat(c)
        return out

    def root_entries(self):
        """(offset, 32-byte entry) for every slot of the root directory."""
        res = []
        for c in self.chain(self.root):
            base = self.cl_off(c)
            data = self.read(base, self.cb)
            for i in range(0, self.cb, 32):
                res.append((base + i, data[i:i + 32]))
        return res

    def find(self, name):
        """Locate a File entry set by (ASCII/Unicode) name in the root dir."""
        ents = self.root_entries()
        for i, (off, e) in enumerate(ents):
            if e[0] != 0x85:
                continue
            sc = e[1]
            st = ents[i + 1][1]
            nlen = st[3]
            nm = b"".join(ents[i + 2 + k][1][2:32] for k in range((nlen + 14) // 15))
            if nm[:2 * nlen].decode("utf-16-le") == name:
                return [o for o, _ in ents[i:i + 1 + sc]]
        raise KeyError(name)

    def set_raw(self, offs):
        return bytearray(b"".join(self.read(o, 32) for o in offs))

    def put_set(self, offs, raw, fix=True):
        if fix:
            raw[2:4] = struct.pack("<H", set_csum(raw))
        for k, o in enumerate(offs):
            self.write(o, bytes(raw[32 * k:32 * k + 32]))

    def stream(self, name):
        offs = self.find(name)
        raw = self.set_raw(offs)
        flags = raw[33]
        first = struct.unpack_from("<I", raw, 52)[0]
        size = struct.unpack_from("<Q", raw, 56)[0]
        return offs, raw, flags, first, size

    def boot_fix(self, base_sector):
        """Recompute the boot checksum of the region starting at base_sector."""
        ss = self.ss
        data = self.read(base_sector * ss, 11 * ss)
        cs = 0
        for i, b in enumerate(data):
            if i in (106, 107, 112):
                continue
            cs = (((cs & 1) << 31) | (cs >> 1)) + b
            cs &= 0xFFFFFFFF
        self.write((base_sector + 11) * ss, struct.pack("<I", cs) * (ss // 4))


def set_csum(raw):
    cs = 0
    for i, b in enumerate(raw):
        if i in (2, 3):
            continue
        cs = (((cs & 1) << 15) | (cs >> 1)) + b
        cs &= 0xFFFF
    return cs


def cmd_dirty(img):
    with open(img, "rb") as f:
        f.seek(106)
        print((struct.unpack("<H", f.read(2))[0] >> 1) & 1)


def cmd_chain(img, name):
    im = Img(img)
    _, _, flags, first, size = im.stream(name)
    n = (size + im.cb - 1) // im.cb
    nofat = bool(flags & 2)
    ch = im.chain(first, n, nofat) if n else []
    runs = 1 + sum(1 for a, b in zip(ch, ch[1:]) if b != a + 1) if ch else 0
    print("flags=%#x nofat=%d first=%d size=%d clusters=%d runs=%d" % (flags, nofat, first, size, len(ch), runs))
    return nofat, runs


def cmd_fragmented(img, name):
    nofat, runs = cmd_chain(img, name)
    return 0 if (not nofat and runs > 1) else 1


def cmd_corrupt(base, outdir):
    os.makedirs(outdir, exist_ok=True)

    def variant(name):
        p = os.path.join(outdir, name + ".img")
        shutil.copyfile(base, p)
        return Img(p)

    # 1. files: a valid volume holding individually damaged objects.
    im = variant("files")
    offs, raw, flags, first, size = im.stream("oob.txt")          # cluster out of range
    struct.pack_into("<I", raw, 52, im.ccount + 100)
    im.put_set(offs, raw)
    offs, raw, flags, first, size = im.stream("cycle.bin")        # FAT cycle
    n = (size + im.cb - 1) // im.cb
    clus = im.chain(first, n, bool(flags & 2))
    assert len(clus) >= 3
    raw[33] = flags & ~2                                          # use the FAT
    im.put_set(offs, raw)
    im.set_fat(clus[0], clus[1])
    im.set_fat(clus[1], clus[0])                                  # c0 -> c1 -> c0 ...
    offs, raw, flags, first, size = im.stream("badsum.txt")       # SetChecksum wrong
    raw[2] ^= 0x5A
    im.put_set(offs, raw, fix=False)
    offs, raw, flags, first, size = im.stream("badcnt.txt")       # SecondaryCount > 18
    raw[1] = 30
    im.put_set(offs, raw)
    offs, raw, flags, first, size = im.stream("loopdir")          # directory FAT cycle
    raw[33] = flags & ~2
    struct.pack_into("<Q", raw, 40, 2 * im.cb)
    struct.pack_into("<Q", raw, 56, 2 * im.cb)
    im.put_set(offs, raw)
    im.set_fat(first, first)

    # 2. geometry: ClusterCount far beyond the volume (checksums recomputed, so
    #    only the geometry check can catch it).
    im = variant("geo")
    for b in (0, 12):
        im.write(b * im.ss + 92, struct.pack("<I", 0xFFFFFF00))
        im.boot_fix(b)

    # 3. main + backup boot checksum both wrong.
    im = variant("csum2")
    for b in (0, 12):
        o = b * im.ss + 300
        im.write(o, bytes([im.read(o, 1)[0] ^ 0xFF]))

    # 4. only the main boot checksum wrong: mount via the backup, read-only.
    im = variant("csum1")
    o = 300
    im.write(o, bytes([im.read(o, 1)[0] ^ 0xFF]))

    # 5. up-case table data damaged (TableChecksum mismatch).
    im = variant("upcase")
    for _, e in im.root_entries():
        if e[0] == 0x82:
            uc = struct.unpack_from("<I", e, 20)[0]
            o = im.cl_off(uc) + 40
            im.write(o, bytes([im.read(o, 1)[0] ^ 0x01]))
            break

    # 6. root directory FAT chain loops onto itself.
    im = variant("rootcycle")
    im.set_fat(im.root, im.root)
    print("corrupt suite written to", outdir)


LONG = "Relazione \u00e8 gi\u00e0 pronta.txt"
LONGBODY = "Il documento \u00e8 pronto per la revisione.\n"
SUBDIR = "Cartella con nome lungo \u00e0\u00e8\u00ec\u00f2\u00f9"
WNAME = "Scritto da SecOS \u2013 \u00f1and\u00fa.txt"
WBODY = "Scritto da SecOS su exFAT: \u00e0\u00e8\u00ec\u00f2\u00f9 \u2713\n"
NDIR = "Nuova cartella SecOS"


def wtext(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def rtext(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def rbin(path):
    with open(path, "rb") as f:
        return f.read()


def cmd_populate(d):
    j = lambda *p: os.path.join(d, *p)
    wtext(j("hello.txt"), "hello from disk\n")
    wtext(j(LONG), LONGBODY)
    wtext(j("emoji_\U0001F600.txt"), "smile\n")
    os.mkdir(j(SUBDIR))
    wtext(j(SUBDIR, "dentro.txt"), "inside\n")
    with open(j("big.bin"), "wb") as f:
        f.write(pat_bytes(3 * 1024 * 1024, 1))
    with open(j("trunc.bin"), "wb") as f:
        f.write(pat_bytes(100000, 4))
    wtext(j("delete_me.txt"), "bye\n")
    wtext(j("rename_me.txt"), "rename payload\n")
    # Two files appended one cluster at a time, alternately: both fragment.
    a = os.open(j("frag_a.bin"), os.O_WRONLY | os.O_CREAT, 0o644)
    b = os.open(j("frag_b.bin"), os.O_WRONLY | os.O_CREAT, 0o644)
    for k in range(40):
        os.write(a, pat_bytes(4096, 2, k * 4096)); os.fsync(a)
        os.write(b, pat_bytes(4096, 3, k * 4096)); os.fsync(b)
    os.close(a); os.close(b)


def cmd_populate_corrupt(d):
    j = lambda *p: os.path.join(d, *p)
    wtext(j("ok.txt"), "fine\n")
    wtext(j("oob.txt"), "out of range\n")
    with open(j("cycle.bin"), "wb") as f:
        f.write(pat_bytes(12288, 9))
    wtext(j("badsum.txt"), "checksum\n")
    wtext(j("badcnt.txt"), "count\n")
    os.mkdir(j("loopdir"))
    wtext(j("loopdir", "inner.txt"), "x\n")


def cmd_verifytree(d):
    j = lambda *p: os.path.join(d, *p)
    fails = 0

    def chk(name, fn):
        nonlocal fails
        try:
            good = bool(fn())
        except Exception as e:  # missing file etc.
            good = False
            name += " (%s)" % e.__class__.__name__
        print("  host [%s] %s" % ("PASS" if good else "FAIL", name))
        fails += 0 if good else 1

    sbig = bytearray(pat_bytes(2 * 1024 * 1024, 5))
    sbig[100000:100008] = b"PATCHED!"
    chk("SecOS unicode file", lambda: rtext(j(WNAME)) == WBODY)
    chk("subdir + nested dir created", lambda: os.path.isdir(j(NDIR, "sub")))
    chk("file in subdir", lambda: rtext(j(NDIR, "file interno.txt")) == "interno\n")
    chk("moved file", lambda: rtext(j(NDIR, "moved.txt")) == "rename payload\n")
    chk("utimes mtime", lambda: int(os.stat(j(NDIR, "moved.txt")).st_mtime) == 1700000000)
    chk("rename sources gone", lambda: not any(os.path.exists(j(n)) for n in ("rename_me.txt", "renamed.txt", "RENAMED.txt")))
    chk("rename over target", lambda: rtext(j("victim.txt")) == "winner\n" and not os.path.exists(j("winner.txt")))
    chk("unlinked file gone", lambda: not os.path.exists(j("delete_me.txt")))
    chk("rmdir'd dir gone", lambda: not os.path.exists(j("tmpdir")))
    chk("2MiB SecOS file + in-place patch", lambda: rbin(j("sbig.bin")) == bytes(sbig))
    chk("fragmented sfa.bin", lambda: rbin(j("sfa.bin")) == pat_bytes(32 * 4096, 6))
    chk("fragmented sfb.bin", lambda: rbin(j("sfb.bin")) == pat_bytes(32 * 4096, 7))
    chk("truncated file (head + zero tail)", lambda: rbin(j("trunc.bin")) == pat_bytes(5000, 4) + bytes(15000))
    chk("write past VDL zero gap", lambda: rbin(j("sparse.bin")) == bytes(70000) + b"END")
    chk("many/: 40 files left", lambda: len(os.listdir(j("many"))) == 40)
    chk("many/ content", lambda: rtext(j("many", "f_79_with_a_reasonably_long_name.txt")) == "/mnt/many/f_79_with_a_reasonably_long_name.txt")
    chk("M10 boot write", lambda: rtext(j("secos_w.txt")) == "SECoS disk write OK\n")
    chk("host files intact", lambda: rbin(j("big.bin")) == pat_bytes(3 * 1024 * 1024, 1)
        and rbin(j("frag_a.bin")) == pat_bytes(40 * 4096, 2) and rtext(j(LONG)) == LONGBODY)
    return 1 if fails else 0


def cmd_deepcheck(img):
    im = Img(img)
    errs = []
    owner = {}

    def own(clusters, who):
        for c in clusters:
            if not (2 <= c < im.ccount + 2):
                errs.append("%s: cluster %d out of range" % (who, c))
                continue
            if c in owner:
                errs.append("%s: cluster %d cross-linked with %s" % (who, c, owner[c]))
            owner[c] = who

    def chain_n(first, n, nofat, who):
        if n == 0:
            return []
        if nofat:
            return [first + i for i in range(n)]
        out, c = [], first
        for i in range(n):
            if not (2 <= c < im.ccount + 2):
                errs.append("%s: bad link %#x" % (who, c))
                return out
            out.append(c)
            nx = im.fat(c)
            if i == n - 1:
                if nx != 0xFFFFFFFF:
                    errs.append("%s: chain longer than DataLength" % who)
            c = nx
        return out

    root = im.chain(im.root)
    own(root, "/")
    up = None
    bitmap = None

    def read_stream(clusters, size):
        data = b"".join(im.read(im.cl_off(c), im.cb) for c in clusters)
        return data[:size]

    def entries(clusters):
        data = b"".join(im.read(im.cl_off(c), im.cb) for c in clusters)
        return [data[i:i + 32] for i in range(0, len(data), 32)]

    for e in entries(root):
        if e[0] == 0x81:
            first, size = struct.unpack_from("<IQ", e, 20)
            n = (size + im.cb - 1) // im.cb
            cl = chain_n(first, n, False, "$bitmap")
            own(cl, "$bitmap")
            bitmap = read_stream(cl, size)
        elif e[0] == 0x82:
            csum = struct.unpack_from("<I", e, 4)[0]
            first, size = struct.unpack_from("<IQ", e, 20)
            n = (size + im.cb - 1) // im.cb
            cl = chain_n(first, n, False, "$upcase")
            own(cl, "$upcase")
            raw = read_stream(cl, size)
            cs = 0
            for b in raw:
                cs = ((((cs & 1) << 31) | (cs >> 1)) + b) & 0xFFFFFFFF
            if cs != csum:
                errs.append("$upcase: table checksum mismatch")
            up = list(range(65536))
            o, i = 0, 0
            while i + 1 < len(raw) and o < 65536:
                v = struct.unpack_from("<H", raw, i)[0]
                if v == 0xFFFF and i + 3 < len(raw):
                    o += struct.unpack_from("<H", raw, i + 2)[0]
                    i += 4
                    continue
                up[o] = v
                o += 1
                i += 2
        elif e[0] == 0x00:
            break
    if bitmap is None or up is None:
        errs.append("missing bitmap/upcase")
        print("\n".join(errs))
        return 1

    def walk(clusters, path):
        ents = entries(clusters)
        i = 0
        while i < len(ents):
            e = ents[i]
            if e[0] == 0x00:
                break
            if e[0] != 0x85:
                i += 1
                continue
            sc = e[1]
            raw = b"".join(ents[i:i + 1 + sc])
            if set_csum(bytearray(raw)) != struct.unpack_from("<H", raw, 2)[0]:
                errs.append("%s: SetChecksum mismatch at slot %d" % (path, i))
            st = raw[32:64]
            nlen = st[3]
            nm16 = b"".join(raw[64 + 32 * k + 2:64 + 32 * k + 32] for k in range((nlen + 14) // 15))[:2 * nlen]
            units = struct.unpack("<%dH" % nlen, nm16)
            h = 0
            for u in units:
                u = up[u]
                for b in (u & 0xFF, u >> 8):
                    h = ((((h & 1) << 15) | (h >> 1)) + b) & 0xFFFF
            name = nm16.decode("utf-16-le", "replace")
            who = path.rstrip("/") + "/" + name
            if h != struct.unpack_from("<H", st, 4)[0]:
                errs.append("%s: NameHash mismatch" % who)
            flags = st[1]
            vdl, = struct.unpack_from("<Q", st, 8)
            first, size = struct.unpack_from("<IQ", st, 20)
            if vdl > size:
                errs.append("%s: VDL > DataLength" % who)
            n = (size + im.cb - 1) // im.cb
            cl = chain_n(first, n, bool(flags & 2), who)
            own(cl, who)
            attr, = struct.unpack_from("<H", raw, 4)
            if attr & 0x10:
                walk(cl, who)
            i += 1 + sc

    walk(root, "/")
    for c in range(2, im.ccount + 2):
        bit = (bitmap[(c - 2) // 8] >> ((c - 2) % 8)) & 1
        if bit and c not in owner:
            errs.append("cluster %d marked used but owned by nothing (leak)" % c)
        if not bit and c in owner:
            errs.append("cluster %d owned by %s but marked free" % (c, owner[c]))
    for e in errs[:30]:
        print("  deepcheck:", e)
    print("deepcheck: %s (%d clusters owned, %d errors)" % ("clean" if not errs else "ERRORS", len(owner), len(errs)))
    return 1 if errs else 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    c = argv[1]
    if c == "dirty":
        cmd_dirty(argv[2])
    elif c == "chain":
        cmd_chain(argv[2], argv[3])
    elif c == "fragmented":
        return cmd_fragmented(argv[2], argv[3])
    elif c == "corrupt":
        cmd_corrupt(argv[2], argv[3])
    elif c == "pattern":
        with open(argv[2], "wb") as f:
            f.write(pat_bytes(int(argv[3]), int(argv[4])))
    elif c == "checkpat":
        with open(argv[2], "rb") as f:
            return 0 if f.read() == pat_bytes(int(argv[3]), int(argv[4])) else 1
    elif c == "populate":
        cmd_populate(argv[2])
    elif c == "populate-corrupt":
        cmd_populate_corrupt(argv[2])
    elif c == "deepcheck":
        return cmd_deepcheck(argv[2])
    elif c == "verifytree":
        return cmd_verifytree(argv[2])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
