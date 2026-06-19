#!/usr/bin/env python3
# [M27a] Synthesize a DIRTY JBD2 journal on an ext3/ext4 image: stage one
# committed-but-not-checkpointed transaction that rewrites a target file's first
# data block, leaving the in-place block stale. A correct journal-recovery
# implementation (SecOS, or e2fsck) must replay it on mount, turning the file
# from its OLD content to NEW.
#
# The synthetic journal is INDEPENDENTLY VALIDATED by running `e2fsck -fy` on a
# copy: if Linux's own jbd2 recovery replays it, it is a genuine JBD2 journal,
# so SecOS matching that result is a real cross-check (not self-consistency).
#
# No root needed: block locations are resolved via `debugfs bmap` (userspace).
# Targets the simplest journal format: no metadata_csum, no 64bit, v2 8-byte tags.
#
# Usage: mk_dirty_journal.py <image> <target-path-in-fs> <NEW-3-byte-tag>
import struct, subprocess, sys

JBD2_MAGIC = 0xc03b3998
FLAG_LAST_TAG = 0x8

def bmap(img, spec, lblk):
    out = subprocess.check_output(["debugfs","-R",f"bmap {spec} {lblk}",img],
                                  stderr=subprocess.DEVNULL).decode().strip()
    return int(out.split()[-1])

def main():
    img, target, tag = sys.argv[1], sys.argv[2], (sys.argv[3] if len(sys.argv)>3 else "NEW")
    data = open(img,'rb').read(); data = bytearray(data)
    bs = 1024 << struct.unpack('<I', data[1024+24:1024+28])[0]      # s_log_block_size
    tgt_blk = bmap(img, target, 0)                                  # target file's block 0
    jphys = lambda lb: bmap(img, "<8>", lb)                         # journal logical -> phys
    JSB = jphys(0)*bs
    assert struct.unpack('>I', data[JSB:JSB+4])[0]==JBD2_MAGIC, "not a JBD2 journal"
    s_first = struct.unpack('>I', data[JSB+20:JSB+24])[0]
    s_seq   = struct.unpack('>I', data[JSB+24:JSB+28])[0]
    s_uuid  = data[JSB+48:JSB+64]

    def put(lb, buf):
        p = jphys(lb)*bs; data[p:p+bs] = buf.ljust(bs, b'\0')[:bs]

    # descriptor (one tag: target block, LAST_TAG) + 16-byte UUID
    d = bytearray(bs)
    d[0:4]=struct.pack('>I',JBD2_MAGIC); d[4:8]=struct.pack('>I',1); d[8:12]=struct.pack('>I',s_seq)
    d[12:16]=struct.pack('>I',tgt_blk); d[16:18]=b'\0\0'; d[18:20]=struct.pack('>H',FLAG_LAST_TAG)
    d[20:36]=s_uuid
    put(s_first, d)
    put(s_first+1, (tag.encode()* (bs//len(tag)+1))[:bs])           # NEW data
    c = bytearray(bs)
    c[0:4]=struct.pack('>I',JBD2_MAGIC); c[4:8]=struct.pack('>I',2); c[8:12]=struct.pack('>I',s_seq)
    put(s_first+2, c)

    data[JSB+28:JSB+32]=struct.pack('>I', s_first)                  # journal s_start = first txn
    fi = struct.unpack('<I', data[1024+96:1024+100])[0]
    data[1024+96:1024+100]=struct.pack('<I', fi | 0x4)             # ext4 NEEDS_RECOVERY
    open(img,'wb').write(data)
    print(f"[mkdirtyjournal] {img}: staged txn seq={s_seq} target_block={tgt_blk} -> '{tag}'")

if __name__=="__main__":
    main()
