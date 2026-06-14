/* SecOS .note.secos — DRIVER manifest (QSEC v2) + signature (QSIG, sig zeroed).
 * The QSIG sig[64] is filled in by tools/secos-sign after linking. Because the
 * signature covers the whole file, the driver claim below is trust-rooted.
 * SPDX-License-Identifier: MIT */
.section .note.secos, "a", @note
.align 4

/* --- QSEC v2: manifest
 *   { u32 version; u32 flags; u64 max_mem; u64 entry_hint;
 *     u32 proc_type; u32 dev_id; u32 dev_caps; u32 reserved }  (descsz = 40) --- */
.long 6                 /* namesz */
.long 40                /* descsz */
.long 0x51534543        /* type = 'QSEC' */
.ascii "SECOS\0"
.balign 4
.long 2                 /* version = MANIFEST_VERSION_DRIVER */
.long 0                 /* flags   */
.quad 0                 /* max_mem */
.quad 0                 /* entry_hint (0 = ignore) */
.long 1                 /* proc_type = PROC_TYPE_DRIVER */
.long 0                 /* dev_id = 0 (seed device) */
.long 0x13              /* dev_caps = READ_REG|WRITE_REG|GET_INFO (no MAP_MEM) */
.long 0                 /* reserved */
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
