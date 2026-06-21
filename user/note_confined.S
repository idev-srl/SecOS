/* SecOS .note.secos — [M35] CONFINED manifest. Opts into least-privilege:
 * flags = CAP_ENFORCE | CAP_FS_READ | CAP_TIME. The kernel restricts this signed
 * process to exactly those capabilities — file writes, spawn, pipe/IPC, sockets
 * and signals are all denied (and audited). The flags are covered by the QSIG
 * Ed25519 signature, so the process cannot widen its own capability set.
 * SPDX-License-Identifier: MIT */
.section .note.secos, "a", @note
.align 4

/* CAP_ENFORCE(1<<5)=0x20 | CAP_FS_READ(1<<6)=0x40 | CAP_TIME(1<<11)=0x800 = 0x860 */
.long 6                 /* namesz */
.long 24                /* descsz */
.long 0x51534543        /* type = 'QSEC' */
.ascii "SECOS\0"
.balign 4
.long 1                 /* version */
.long 0x860             /* flags = CAP_ENFORCE|CAP_FS_READ|CAP_TIME */
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
