/* SecOS .note.secos - [M46] ambient probe (user/m46_confine.c) with the explicit
 * caps CAP_NET | CAP_INPUT | CAP_DISPLAY = 0x6010 (no CAP_ENFORCE). Started by an
 * ambient parent it holds them; started under no_new_privs it must not.
 * SPDX-License-Identifier: MIT */
.section .note.secos, "a", @note
.align 4

/* CAP_NET(1<<4) | CAP_INPUT(1<<13) | CAP_DISPLAY(1<<14) = 0x6010 */
.long 6                 /* namesz */
.long 24                /* descsz */
.long 0x51534543        /* type = 'QSEC' */
.ascii "SECOS\0"
.balign 4
.long 1                 /* version */
.long 0x6010            /* flags = CAP_NET|CAP_INPUT|CAP_DISPLAY */
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
