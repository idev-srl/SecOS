#!/usr/bin/env python3
# SecOS code-signing — shared helpers (host side).
# See docs/SIGNING.md.  SPDX-License-Identifier: MIT
#
# Format: a `.note.secos` PT_NOTE carries two notes, name "SECOS":
#   - QSEC (0x51534543): manifest (version, flags, [proc_type, caps...], max_mem, entry)
#   - QSIG (0x51534947): { uint32 version; uint32 key_id; uint8 sig[64] }
#
# Signed digest = SHA-256 over the ENTIRE ELF file with the 64-byte QSIG `sig`
# field set to zero.  The 32-byte digest is then Ed25519-signed.  Host and kernel
# compute the identical digest, so this is the single source of truth.

import struct, hashlib

QSEC = 0x51534543  # 'QSEC' little-endian note type for the manifest
QSIG = 0x51534947  # 'QSIG' note type for the signature
NOTE_NAME = b"SECOS\x00"

# DEV-ONLY signing seed. In production this is a random secret kept OFFLINE and
# never committed; only the public key is embedded in the kernel. The fixed dev
# seed lets the build sign automatically while bootstrapping.
DEV_SEED = (b"SecOS-DEV-signing-key-2026-v0!!!!")[:32]
assert len(DEV_SEED) == 32

PT_NOTE = 4

def _u32(b, o): return struct.unpack_from("<I", b, o)[0]

def find_qsig_sig_offset(data):
    """Return the absolute file offset of the QSIG note's sig[64] field, or None."""
    if len(data) < 64 or data[:4] != b"\x7fELF":
        return None
    e_phoff   = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsz = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum   = struct.unpack_from("<H", data, 0x38)[0]
    for i in range(e_phnum):
        ph = e_phoff + i * e_phentsz
        p_type   = _u32(data, ph)
        p_offset = struct.unpack_from("<Q", data, ph + 8)[0]
        p_filesz = struct.unpack_from("<Q", data, ph + 32)[0]
        if p_type != PT_NOTE:
            continue
        note, end = p_offset, p_offset + p_filesz
        while note + 12 <= end:
            namesz = _u32(data, note); descsz = _u32(data, note + 4); typ = _u32(data, note + 8)
            desc = note + 12 + ((namesz + 3) & ~3)
            nxt  = desc + ((descsz + 3) & ~3)
            if nxt > end:
                break
            if typ == QSIG and namesz >= 6 and data[note+12:note+17] == b"SECOS":
                # desc = { u32 version; u32 key_id; u8 sig[64] }  -> sig at desc+8
                return desc + 8
            note = nxt
    return None

def compute_digest(data, sig_off):
    """SHA-256 over the file with the 64 sig bytes (at sig_off) zeroed."""
    h = hashlib.sha256()
    h.update(data[:sig_off])
    h.update(b"\x00" * 64)
    h.update(data[sig_off + 64:])
    return h.digest()

def build_note_blob(manifest_raw, key_id=0):
    """Build the raw bytes of a `.note.secos` containing a QSEC manifest note and
    a QSIG signature note (sig zeroed). Returns the blob; the QSIG sig is filled
    in later by signing the containing file."""
    def note(typ, desc):
        nm = NOTE_NAME
        nm_pad = (-len(nm)) % 4
        d_pad  = (-len(desc)) % 4
        return (struct.pack("<III", len(nm), len(desc), typ)
                + nm + b"\x00" * nm_pad + desc + b"\x00" * d_pad)
    qsec = note(QSEC, manifest_raw)
    qsig_desc = struct.pack("<II", 1, key_id) + b"\x00" * 64
    qsig = note(QSIG, qsig_desc)
    return qsec + qsig
