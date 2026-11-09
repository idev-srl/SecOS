/* SecOS .note.secos — [M58] manifest of /sbin/udrv-e1000e (QSEC v3) + signature.
 *   flags      = CAP_ENFORCE | CAP_FS_READ | CAP_PROC | CAP_TIME (0x960):
 *                confined, so no_new_privs follows (M46);
 *   proc_type  = DRIVER; match: Intel 82574L (8086:10d3) only;
 *   udrv_flags = UDRV_DRV_NET (may attach the kernel network proxy);
 *   bar_mask   = BAR 0; irq_max = 1; dma_max = 64 KiB (two descriptor
 *                pages; the packet slots are the kernel's);
 *   cfg_write_mask = COMMAND.
 * The QSIG sig[64] is filled in by tools/secos-sign after linking.
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
.long 1                 /* udrv_flags = UDRV_DRV_NET */
.short 0x01             /* bar_mask */
.byte 1                 /* irq_max */
.byte 1                 /* nmatch */
.quad 0x10000           /* dma_max = 64 KiB */
.long 1                 /* cfg_write_mask = COMMAND */
.long 0                 /* reserved2 */
.short 0x8086, 0x10d3   /* match[0]: e1000e 82574L */
.byte 0xFF, 0xFF
.short 0
.fill 24, 1, 0          /* match[1..3] unused */
.balign 4

.long 6                 /* namesz */
.long 72                /* descsz */
.long 0x51534947        /* type = 'QSIG' */
.ascii "SECOS\0"
.balign 4
.long 1                 /* version */
.long 0                 /* key_id */
.fill 64, 1, 0          /* sig[64] — filled by secos-sign */
