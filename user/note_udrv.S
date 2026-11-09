/* SecOS .note.secos — [M57] ring-3 PCI DRIVER manifest (QSEC v3) + signature.
 * Used by the M57 test programs (user/udrv_edu.c, user/udrv_negtest.c).
 *   flags     = CAP_ENFORCE | CAP_FS_READ | CAP_PROC | CAP_TIME (0x960): confined
 *               (no FS writes, sockets, IPC or signals to others), so
 *               no_new_privs follows automatically (M46).
 *   proc_type = DRIVER; match: QEMU edu (1234:11e8) and e1000e (8086:10d3);
 *   bar_mask  = all six BARs; irq_max = 4; dma_max = 64 KiB;
 *   cfg_write_mask = COMMAND | DEVSPEC.
 * The QSIG sig[64] is filled in by tools/secos-sign after linking, so every
 * field below is covered by the Ed25519 signature.
 * SPDX-License-Identifier: MIT */
.section .note.secos, "a", @note
.align 4

.long 6                 /* namesz */
.long 96                /* descsz (v3) */
.long 0x51534543        /* type = 'QSEC' */
.ascii "SECOS\0"
.balign 4
.long 3                 /* version = MANIFEST_VERSION_UDRV */
.long 0x960             /* flags */
.quad 0                 /* max_mem (0 = unlimited) */
.quad 0                 /* entry_hint */
.long 1                 /* proc_type = DRIVER */
.long 0                 /* dev_id (M11, unused) */
.long 0                 /* dev_caps (M11, unused) */
.long 0                 /* reserved */
.long 0                 /* udrv_flags */
.short 0x3F             /* bar_mask */
.byte 4                 /* irq_max */
.byte 2                 /* nmatch */
.quad 0x10000           /* dma_max = 64 KiB */
.long 3                 /* cfg_write_mask = COMMAND | DEVSPEC */
.long 0                 /* reserved2 */
.short 0x1234, 0x11e8   /* match[0]: QEMU edu */
.byte 0xFF, 0xFF
.short 0
.short 0x8086, 0x10d3   /* match[1]: e1000e 82574L */
.byte 0xFF, 0xFF
.short 0
.fill 16, 1, 0          /* match[2..3] unused */
.balign 4

.long 6                 /* namesz */
.long 72                /* descsz */
.long 0x51534947        /* type = 'QSIG' */
.ascii "SECOS\0"
.balign 4
.long 1                 /* version */
.long 0                 /* key_id */
.fill 64, 1, 0          /* sig[64] — filled by secos-sign */
