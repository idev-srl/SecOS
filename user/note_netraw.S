/* SecOS .note.secos — manifest (QSEC) + signature (QSIG, sig zeroed).
 * [U10] CAP_NET + CAP_NET_RAW variant (bits 4 and 17): the socket syscalls
 * and SOCK_RAW without being root (tools/netposix_test.sh nptest_raw). The
 * QSIG sig[64] is filled by
 * tools/secos-sign after linking.
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
.long 0x20010           /* flags = MANIFEST_FLAG_CAP_NET | MANIFEST_FLAG_CAP_NET_RAW */
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
