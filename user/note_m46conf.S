/* SecOS .note.secos - [M46] confined exec-confinement driver (user/m46_confine.c):
 * flags = CAP_ENFORCE | FS_READ | PROC | IPC | SIGNAL | TIME = 0xF60. It may
 * spawn, fork and exec, but (M46) nothing it starts can hold more than this.
 * SPDX-License-Identifier: MIT */
.section .note.secos, "a", @note
.align 4

/* 0x20 | 0x40 | 0x100 | 0x200 | 0x400 | 0x800 = 0xF60 */
.long 6                 /* namesz */
.long 24                /* descsz */
.long 0x51534543        /* type = 'QSEC' */
.ascii "SECOS\0"
.balign 4
.long 1                 /* version */
.long 0xF60             /* flags = CAP_ENFORCE|FS_READ|PROC|IPC|SIGNAL|TIME */
.quad 0                 /* max_mem (0 = unlimited) */
.quad 0                 /* entry_hint */
.balign 4

.long 6                 /* namesz */
.long 72                /* descsz */
.long 0x51534947        /* type = 'QSIG' */
.ascii "SECOS\0"
.balign 4
.long 1                 /* version */
.long 0                 /* key_id */
.fill 64, 1, 0          /* sig[64] — filled by secos-sign */
