/* SecOS .note.secos — manifest (QSEC) + signature (QSIG, sig zeroed).
 * The QSIG sig[64] is filled in by tools/secos-sign after linking.
 * SPDX-License-Identifier: MIT */
.section .note.secos, "a", @note
.align 4

/* --- QSEC: manifest { u32 version; u32 flags; u64 max_mem; u64 entry_hint } --- */
.long 6                 /* namesz */
.long 24                /* descsz */
.long 0x51534543        /* type = 'QSEC' */
.ascii "SECOS\0"
.balign 4
.long 1                 /* version */
.long 0                 /* flags   */
.quad 0                 /* max_mem */
.quad 0                 /* entry_hint (0 = ignore) */
.balign 4

/* --- QSIG: signature { u32 version; u32 key_id; u8 sig[64] } --- */
.long 6                 /* namesz */
.long 72                /* descsz */
.long 0x51534947        /* type = 'QSIG' */
.ascii "SECOS\0"
.balign 4
.long 1                 /* version */
.long 0                 /* key_id (0 = project key) */
.fill 64, 1, 0          /* sig[64] — filled by secos-sign */
