#!/usr/bin/env python3
"""tools/ntfs_img.py - host-side helpers for the SecOS M42 NTFS tests.

Independent of the SecOS driver. Subcommands:

  populate DIR             fill a mounted (ntfs-3g) volume with the test tree
  populate-fuzz DIR        scaled-down full tree (fuzz base)
  populate-small DIR       small tree for the corrupt-image base
  list DIR                 walk a mounted tree, print the reference listing
  compare REF OURS         compare two listings (see ntfs_fuzz `list`)
  efs IMG INODE            mark a file encrypted (EFS) in place
  dirty IMG                set the $Volume dirty flag
  info IMG INODE           attributes of a record (checks: attr list, depth)
  corrupt BASE OUTDIR      derive the damaged-image suite from BASE

Listing format (one line per object, sorted by path, tab-separated):
  F <size> <fnv1a64> <path>   regular file
  E <errno> <size> <path>     file whose read fails
  D - - <path>                directory
  L - <target> <path>         symlink
"""
import os
import struct
import sys

# ---------------------------------------------------------------- test data
def pat(i, seed):
    return (i * 131 + seed * 17 + (i >> 12)) & 0xFF

def pattern(n, seed):
    return bytes(pat(i, seed) for i in range(n))

def text_blob(n):
    words = ("lorem ipsum dolor sit amet NTFS SecOS compressione file "
             "documento pronto revisione cluster index record ").split()
    out, i = [], 0
    while sum(len(w) + 1 for w in out) < n:
        out.append(words[(i * 7 + i // 13) % len(words)] + ("\n" if i % 11 == 10 else ""))
        i += 1
    return (" ".join(out).encode())[:n]

def fnv(data, h=0xCBF29CE484222325):
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h

LONG_UNI = "Relazione è già pronta — ünïcødé 😀.txt"
CJK = "日本語のファイル名.txt"
LONG_ASCII = "A_very_long_file_name_" + "x" * 150 + ".txt"   # 176 chars
MANY = 2000

def setxattr(path, name, value):
    os.setxattr(path, name, value)

def reparse_symlink(target, relative):
    t = target.encode("utf-16-le")
    pb = t + t                                # substitute name + print name
    body = struct.pack("<HHHHI", 0, len(t), len(t), len(t), 1 if relative else 0) + pb
    return struct.pack("<IHH", 0xA000000C, len(body), 0) + body

def reparse_junction(subst, printname):
    s = subst.encode("utf-16-le") + b"\0\0"
    p = printname.encode("utf-16-le") + b"\0\0"
    body = struct.pack("<HHHH", 0, len(s) - 2, len(s), len(p) - 2) + s + p
    return struct.pack("<IHH", 0xA0000003, len(body), 0) + body

def reparse_lx(target):
    body = struct.pack("<I", 2) + target.encode()
    return struct.pack("<IHH", 0xA000001D, len(body), 0) + body

def wfile(path, data):
    with open(path, "wb") as f:
        f.write(data)

def populate(d, scale=1.0):
    """Full test tree; scale < 1 builds the smaller fuzz base."""
    many, filler = int(MANY * scale), int(3000 * scale)
    mib = max(1, int(5 * scale))
    wfile(f"{d}/hello.txt", b"hello from ntfs\n")
    wfile(f"{d}/{LONG_UNI}", "Il documento è pronto per la revisione.\n".encode())
    wfile(f"{d}/{CJK}", "こんにちは NTFS\n".encode())
    wfile(f"{d}/{LONG_ASCII}", b"long ascii name\n")
    os.makedirs(f"{d}/Cartella è bella 😀/sub", exist_ok=True)
    wfile(f"{d}/Cartella è bella 😀/sub/inner.txt", b"inner\n")
    os.makedirs(f"{d}/a/b/c/d/e/f/g", exist_ok=True)
    wfile(f"{d}/a/b/c/d/e/f/g/deep.txt", b"deep down\n")
    wfile(f"{d}/tiny_resident.txt", b"r")
    wfile(f"{d}/empty.txt", b"")
    os.makedirs(f"{d}/emptydir")
    # 8.3 alias: Win32 + DOS pair, the DOS name must not be listed
    wfile(f"{d}/Long Document Name.txt", b"has a DOS alias\n")
    setxattr(f"{d}/Long Document Name.txt", "system.ntfs_dos_name", "LONGDO~1.TXT".encode("utf-16-le"))
    # hard link
    wfile(f"{d}/linked_a.txt", b"two names\n")
    os.link(f"{d}/linked_a.txt", f"{d}/linked_b.txt")
    # sparse: 48 MiB logical, three data islands
    sp = 48 if scale >= 1 else 8
    with open(f"{d}/sparse.bin", "wb") as f:
        f.write(pattern(10000, 3))
        f.seek((sp // 2) << 20); f.write(pattern(70000, 4))
        f.seek((sp << 20) - 5000); f.write(pattern(5000, 5))
    # compressed (LZNT1): directory with the compressed attribute
    os.makedirs(f"{d}/compressed")
    setxattr(f"{d}/compressed", "system.ntfs_attrib_be", (0x800).to_bytes(4, "big"))
    wfile(f"{d}/compressed/text.txt", text_blob(300000))
    mix = bytearray(text_blob(70000)) + bytearray(os.urandom(70000)) + bytearray(70000) + bytearray(text_blob(33333))
    wfile(f"{d}/compressed/mixed.bin", bytes(mix))
    wfile(f"{d}/compressed/small.txt", b"compressed but tiny\n")
    # named data stream (alternate data stream) next to the unnamed one
    wfile(f"{d}/with_ads.txt", b"main stream\n")
    setxattr(f"{d}/with_ads.txt", "user.secret", b"alternate stream data")
    # symlinks: Windows relative symlink, absolute symlink, junction, WSL, Interix
    open(f"{d}/winlink_rel", "wb").close()
    setxattr(f"{d}/winlink_rel", "system.ntfs_reparse_data", reparse_symlink("a\\b\\c\\d\\e\\f\\g\\deep.txt", True))
    open(f"{d}/winlink_abs", "wb").close()
    setxattr(f"{d}/winlink_abs", "system.ntfs_reparse_data", reparse_symlink("C:\\hello.txt", False))
    os.makedirs(f"{d}/junction")
    setxattr(f"{d}/junction", "system.ntfs_reparse_data", reparse_junction("\\??\\C:\\a\\b", "C:\\a\\b"))
    open(f"{d}/wsl_link", "wb").close()
    setxattr(f"{d}/wsl_link", "system.ntfs_reparse_data", reparse_lx("hello.txt"))
    os.symlink("hello.txt", f"{d}/interix_link")
    # to be marked encrypted (EFS) afterwards
    wfile(f"{d}/secret.efs", b"this must not be readable\n")
    # big directory: several B+ tree levels
    os.makedirs(f"{d}/many")
    for i in range(many):
        wfile(f"{d}/many/entry_{i:05d}_with_a_reasonably_long_name.dat", b"%d\n" % i)
    # fragmentation: 4 KiB files, then ballast until the volume is full, then
    # every other 4 KiB file deleted: the only free space left is ~1500
    # one-cluster holes, so the next file is written in ~1300 runs (too many
    # for one MFT record -> $ATTRIBUTE_LIST + extension records).
    os.makedirs(f"{d}/filler")
    for i in range(filler):
        wfile(f"{d}/filler/f{i:04d}", pattern(4096, i & 0xFF))
    os.sync()
    chunk = bytes(1 << 20)
    for name, sz in (("ballast", 1 << 20), ("ballast2", 4096)):
        try:
            with open(f"{d}/{name}", "wb") as f:
                while True:
                    f.write(chunk[:sz]); f.flush()
        except OSError:
            pass
    os.sync()
    for i in range(0, filler, 2):
        os.unlink(f"{d}/filler/f{i:04d}")
    os.sync()
    wfile(f"{d}/fragmented.bin", pattern((mib << 20) if scale >= 1 else (filler // 2 - 100) * 4096, 9))
    os.sync()
    for name in ("ballast", "ballast2"):
        if os.path.exists(f"{d}/{name}"):
            os.unlink(f"{d}/{name}")
    os.sync()
    wfile(f"{d}/big.bin", pattern(mib << 20, 11))

def populate_small(d):
    wfile(f"{d}/ok.txt", b"intact\n")
    wfile(f"{d}/runbad.bin", pattern(20000, 1))
    wfile(f"{d}/attrbad.bin", pattern(20000, 2))
    wfile(f"{d}/lznt_bad.bin", b"")
    os.makedirs(f"{d}/compressed")
    setxattr(f"{d}/compressed", "system.ntfs_attrib_be", (0x800).to_bytes(4, "big"))
    wfile(f"{d}/compressed/c.txt", text_blob(100000))
    wfile(f"{d}/secret.efs", b"encrypted\n")
    os.makedirs(f"{d}/loopdir")
    for i in range(400):
        wfile(f"{d}/loopdir/n{i:04d}_padding_padding_padding.txt", b"x")
    os.makedirs(f"{d}/indxbad")
    for i in range(400):
        wfile(f"{d}/indxbad/n{i:04d}_padding_padding_padding.txt", b"x")
    wfile(f"{d}/hiberfil.sys", b"HIBR" + bytes(8188))

def list_tree(d):
    out = []
    for root, dirs, files in os.walk(d, followlinks=False):
        for n in sorted(dirs + files):
            p = os.path.join(root, n)
            rel = "/" + os.path.relpath(p, d)
            if os.path.islink(p):
                out.append((rel, f"L\t-\t{os.readlink(p)}\t{rel}"))
            elif os.path.isdir(p):
                out.append((rel, f"D\t-\t-\t{rel}"))
            else:
                try:
                    with open(p, "rb") as f:
                        data = f.read()
                    out.append((rel, f"F\t{len(data)}\t{fnv(data):016x}\t{rel}"))
                except OSError as e:
                    out.append((rel, f"E\t{e.errno}\t{os.path.getsize(p)}\t{rel}"))
        dirs[:] = [x for x in dirs if not os.path.islink(os.path.join(root, x))]
    out.sort(key=lambda t: t[0].encode())
    for _, line in out:
        print(line)

# Symlink targets differ by design (SecOS maps absolute targets under the
# mount point, ntfs-3g has its own scheme): expected SecOS targets here.
EXPECT_LINKS = {
    "/winlink_rel": "a/b/c/d/e/f/g/deep.txt",
    "/winlink_abs": "/mnt/hello.txt",
    "/junction": "/mnt/a/b",
    "/wsl_link": "hello.txt",
    "/interix_link": "hello.txt",
}
EXPECT_ERR = {"/secret.efs": "13"}

def compare(ref_path, ours_path, mp="/mnt"):
    def load(p):
        m = {}
        for line in open(p, encoding="utf-8", errors="surrogateescape"):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            if line.count("\t") < 3:
                m[line] = ("BAD",); continue
            t, a, b, path = line.split("\t", 3)
            m[path] = (t, a, b)
        return m
    ref, ours = load(ref_path), load(ours_path)
    fail = 0
    for path in sorted(set(ref) | set(ours)):
        r, o = ref.get(path), ours.get(path)
        if path in EXPECT_LINKS:
            want = EXPECT_LINKS[path].replace("/mnt", mp, 1)
            if not o or o[0] != "L" or o[2] != want:
                print(f"  [FAIL] symlink {path}: got {o}, want L {want}"); fail = 1
            continue
        if path in EXPECT_ERR:
            if not o or o[0] != "E" or o[1] != EXPECT_ERR[path]:
                print(f"  [FAIL] {path}: got {o}, want read error {EXPECT_ERR[path]}"); fail = 1
            continue
        if r != o:
            print(f"  [FAIL] {path}: host {r} secos {o}"); fail = 1
    nf = sum(1 for v in ours.values() if v[0] == "F")
    print(f"  symlinks: " + ", ".join(f"{p}->{ours[p][2]}" for p in EXPECT_LINKS if p in ours))
    print(f"compare: {len(ours)} objects ({nf} files) {'OK' if not fail else 'MISMATCH'}")
    return fail

# ---------------------------------------------------------- image access
class Vol:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "r+b")
        b = self.read(0, 512)
        self.bps = struct.unpack_from("<H", b, 0x0B)[0]
        spc = b[0x0D]
        self.cb = self.bps * spc if spc <= 0x80 else self.bps << (256 - spc)
        self.mft_lcn, self.mirr_lcn = struct.unpack_from("<QQ", b, 0x30)
        c = struct.unpack_from("<b", b, 0x40)[0]
        self.rs = c * self.cb if c > 0 else 1 << -c
        c = struct.unpack_from("<b", b, 0x44)[0]
        self.isz = c * self.cb if c > 0 else 1 << -c
        r0 = self.fixed(self.read(self.mft_lcn * self.cb, self.rs))
        self.mft_runs = None
        for a in self.attrs(r0):
            if a["type"] == 0x80 and not a["name"]:
                self.mft_runs = self.runs(r0, a)
                break

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def write(self, off, data):
        self.f.seek(off)
        self.f.write(data)

    @staticmethod
    def fixed(b):
        b = bytearray(b)
        uo, uc = struct.unpack_from("<HH", b, 4)
        for i in range(1, uc):
            b[i * 512 - 2:i * 512] = b[uo + 2 * i:uo + 2 * i + 2]
        return b

    @staticmethod
    def unfixed(b):
        b = bytearray(b)
        uo, uc = struct.unpack_from("<HH", b, 4)
        usn = b[uo:uo + 2]
        for i in range(1, uc):
            b[uo + 2 * i:uo + 2 * i + 2] = b[i * 512 - 2:i * 512]
            b[i * 512 - 2:i * 512] = usn
        return b

    @staticmethod
    def attrs(r):
        pos = struct.unpack_from("<H", r, 0x14)[0]
        while pos + 16 <= len(r):
            t, ln = struct.unpack_from("<II", r, pos)
            if t == 0xFFFFFFFF or ln < 16 or pos + ln > len(r):
                return
            nr, nl, no = r[pos + 8], r[pos + 9], struct.unpack_from("<H", r, pos + 10)[0]
            name = bytes(r[pos + no:pos + no + 2 * nl]).decode("utf-16-le")
            yield {"type": t, "pos": pos, "len": ln, "nonres": nr, "name": name}
            pos += ln

    def runs(self, r, a):
        p = a["pos"] + struct.unpack_from("<H", r, a["pos"] + 0x20)[0]
        vcn = struct.unpack_from("<Q", r, a["pos"] + 0x10)[0]
        out, lcn = [], 0
        while r[p]:
            h = r[p]; lb, ob = h & 15, h >> 4
            ln = int.from_bytes(r[p + 1:p + 1 + lb], "little")
            if ob:
                lcn += int.from_bytes(r[p + 1 + lb:p + 1 + lb + ob], "little", signed=True)
                out.append((vcn, ln, lcn))
            else:
                out.append((vcn, ln, None))
            vcn += ln; p += 1 + lb + ob
        return out

    def rec_off(self, n):
        byte = n * self.rs
        for vcn, ln, lcn in self.mft_runs:
            if vcn * self.cb <= byte < (vcn + ln) * self.cb:
                return lcn * self.cb + byte - vcn * self.cb
        raise ValueError("record outside the mapped $MFT")

    def rec(self, n):
        return self.fixed(self.read(self.rec_off(n), self.rs))

    def put_rec(self, n, r, mirror=False):
        self.write(self.rec_off(n), self.unfixed(r))
        if mirror and n < 4:
            self.write(self.mirr_lcn * self.cb + n * self.rs, self.unfixed(r))

    def attr(self, r, t, name=""):
        for a in self.attrs(r):
            if a["type"] == t and a["name"] == name:
                return a
        return None

def efs(img, inode):
    v = Vol(img)
    r = v.rec(inode)
    si = v.attr(r, 0x10)
    vo = struct.unpack_from("<H", r, si["pos"] + 0x14)[0]
    fa = struct.unpack_from("<I", r, si["pos"] + vo + 0x20)[0]
    struct.pack_into("<I", r, si["pos"] + vo + 0x20, fa | 0x4000)
    d = v.attr(r, 0x80)
    fl = struct.unpack_from("<H", r, d["pos"] + 0x0C)[0]
    struct.pack_into("<H", r, d["pos"] + 0x0C, fl | 0x4000)
    v.put_rec(inode, r)

def dirty(img):
    v = Vol(img)
    r = v.rec(3)
    a = v.attr(r, 0x70)
    vo = struct.unpack_from("<H", r, a["pos"] + 0x14)[0]
    fl = struct.unpack_from("<H", r, a["pos"] + vo + 10)[0]
    struct.pack_into("<H", r, a["pos"] + vo + 10, fl | 1)
    v.put_rec(3, r)

def info(img, inode):
    v = Vol(img)
    r = v.rec(inode)
    for a in v.attrs(r):
        extra = ""
        if a["type"] == 0xA0 and a["nonres"]:
            # B+ tree: count INDX blocks that are internal nodes
            runs = v.runs(r, a)
            nodes = internal = 0
            for vcn, ln, lcn in runs:
                if lcn is None:
                    continue
                for k in range(ln * v.cb // v.isz):
                    b = v.read(lcn * v.cb + k * v.isz, v.isz)
                    if b[:4] == b"INDX":
                        nodes += 1
                        if v.fixed(b)[0x18 + 12] & 1:
                            internal += 1
            extra = f" indx_blocks={nodes} internal_nodes={internal}"
        if a["type"] == 0x80 and a["nonres"]:
            extra = f" runs={len(v.runs(r, a))}"
        print(f"attr 0x{a['type']:02X} name='{a['name']}' nonres={a['nonres']}{extra}")

# ---------------------------------------------------------- corrupt suite
def inode_of(v, name):
    """Find the base record whose $FILE_NAME is `name` (linear MFT scan)."""
    for n in range(24, 4096):
        try:
            r = v.rec(n)
        except Exception:
            break
        if r[:4] != b"FILE" or not (struct.unpack_from("<H", r, 0x16)[0] & 1):
            continue
        for a in v.attrs(r):
            if a["type"] == 0x30:
                vo = struct.unpack_from("<H", r, a["pos"] + 0x14)[0]
                k = a["pos"] + vo
                nl = r[k + 0x40]
                if bytes(r[k + 0x42:k + 0x42 + 2 * nl]).decode("utf-16-le") == name:
                    return n
    raise KeyError(name)

def corrupt(base, outdir):
    import shutil
    os.makedirs(outdir, exist_ok=True)
    def mk(name):
        p = os.path.join(outdir, name + ".img")
        shutil.copyfile(base, p)
        return p, Vol(p)

    # files: mounts; damaged objects give errors
    p, v = mk("files")
    ino = {k: inode_of(v, k) for k in ("runbad.bin", "attrbad.bin", "c.txt", "secret.efs", "loopdir", "indxbad")}
    n = ino["runbad.bin"]                 # run list: LCN far outside the volume
    r = v.rec(n); a = v.attr(r, 0x80)
    mp = a["pos"] + struct.unpack_from("<H", r, a["pos"] + 0x20)[0]
    h = r[mp]; lb, ob = h & 15, h >> 4
    r[mp + 1 + lb + ob - 1] = 0x7F                # top byte of the LCN delta
    v.put_rec(n, r)
    n = ino["attrbad.bin"]                # $DATA attribute length overflows the record
    r = v.rec(n); a = v.attr(r, 0x80)
    struct.pack_into("<I", r, a["pos"] + 4, 0x7FF8)
    v.put_rec(n, r)
    n = ino["c.txt"]                      # LZNT1: break the first chunk header
    r = v.rec(n); a = v.attr(r, 0x80)
    runs = v.runs(r, a)
    lcn = runs[0][2]
    blk = bytearray(v.read(lcn * v.cb, 16))
    blk[0], blk[1] = 0xFF, 0xBF                   # compressed chunk longer than its unit
    v.write(lcn * v.cb, bytes(blk))
    n = ino["secret.efs"]
    v.f.close(); efs(p, n); v = Vol(p)
    n = ino["loopdir"]                    # index node whose child is itself
    r = v.rec(n); a = v.attr(r, 0xA0, "$I30")
    runs = v.runs(r, a)
    lcn = runs[0][2]
    vcn_unit = v.cb if v.isz >= v.cb else 512
    b = v.fixed(v.read(lcn * v.cb, v.isz))
    eo, il = struct.unpack_from("<II", b, 0x18)
    e = 0x18 + eo
    while True:
        el, fl = struct.unpack_from("<H", b, e + 8)[0], struct.unpack_from("<H", b, e + 12)[0]
        if fl & 2:
            break
        e += el
    # make the last entry of block 0 point back at block 0
    if not (struct.unpack_from("<H", b, e + 12)[0] & 1):
        # needs room for the VCN: rewrite the last entry as a 24-byte subnode entry
        struct.pack_into("<HHH", b, e + 8, 24, 0, 3)
        struct.pack_into("<Q", b, e + 16, 0)
        struct.pack_into("<I", b, 0x18 + 4, e + 24 - 0x18)
        b[0x18 + 12] |= 1
    else:
        el = struct.unpack_from("<H", b, e + 8)[0]
        struct.pack_into("<Q", b, e + el - 8, 0)
    v.write(lcn * v.cb, bytes(v.unfixed(b)))
    n = ino["indxbad"]                    # INDX block with a torn sector
    r = v.rec(n); a = v.attr(r, 0xA0, "$I30")
    lcn = v.runs(r, a)[0][2]
    raw = bytearray(v.read(lcn * v.cb, v.isz))
    raw[1022] ^= 0x55
    v.write(lcn * v.cb, bytes(raw))
    v.f.close(); dirty(p)

    # bootmain: primary boot sector destroyed -> backup copy
    p, v = mk("bootmain")
    v.write(3, b"XXXXXXXX")
    # bootboth: both copies destroyed -> refused
    p, v = mk("bootboth")
    v.write(3, b"XXXXXXXX")
    size = os.path.getsize(p)
    v.write(size - 512 + 3, b"XXXXXXXX")
    # mft0: $MFT record 0 torn -> $MFTMirr copy
    p, v = mk("mft0")
    raw = bytearray(v.read(v.mft_lcn * v.cb, v.rs)); raw[510] ^= 0xFF
    v.write(v.mft_lcn * v.cb, bytes(raw))
    # mftboth: record 0 torn in both -> refused
    p, v = mk("mftboth")
    raw = bytearray(v.read(v.mft_lcn * v.cb, v.rs)); raw[510] ^= 0xFF
    v.write(v.mft_lcn * v.cb, bytes(raw))
    raw = bytearray(v.read(v.mirr_lcn * v.cb, v.rs)); raw[510] ^= 0xFF
    v.write(v.mirr_lcn * v.cb, bytes(raw))
    # upcase: $UpCase table damaged -> refused
    p, v = mk("upcase")
    r = v.rec(10); a = v.attr(r, 0x80)
    lcn = v.runs(r, a)[0][2]
    v.write(lcn * v.cb + 2 * ord("a"), struct.pack("<H", ord("a")))
    # rootbad: root $INDEX_ROOT collation rule changed -> refused
    p, v = mk("rootbad")
    r = v.rec(5); a = v.attr(r, 0x90, "$I30")
    vo = struct.unpack_from("<H", r, a["pos"] + 0x14)[0]
    struct.pack_into("<I", r, a["pos"] + vo + 4, 0x77)
    v.put_rec(5, r)
    # geo: impossible cluster size (boot + backup) -> refused
    p, v = mk("geo")
    v.write(0x0D, b"\x03")
    size = os.path.getsize(p)
    v.write(size - 512 + 0x0D, b"\x03")
    print(f"corrupt: suite written to {outdir}")

def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(2)
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "populate": populate(args[0])
    elif cmd == "populate-fuzz": populate(args[0], 0.35)
    elif cmd == "populate-small": populate_small(args[0])
    elif cmd == "list": list_tree(args[0])
    elif cmd == "compare": sys.exit(compare(args[0], args[1], args[2] if len(args) > 2 else "/mnt"))
    elif cmd == "efs": efs(args[0], int(args[1]))
    elif cmd == "dirty": dirty(args[0])
    elif cmd == "info": info(args[0], int(args[1]))
    elif cmd == "corrupt": corrupt(args[0], args[1])
    else:
        print(__doc__); sys.exit(2)

if __name__ == "__main__":
    main()
