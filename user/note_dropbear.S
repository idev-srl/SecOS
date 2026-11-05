/* SecOS .note.secos — Dropbear SSH server manifest (least privilege).
 * flags = CAP_ENFORCE | CAP_NET | CAP_FS_READ | CAP_FS_WRITE | CAP_PROC |
 *         CAP_IPC | CAP_SIGNAL | CAP_TIME = 0xFF0
 * Sockets (CAP_NET), host key / authorized_keys / pid file (FS_READ/WRITE),
 * fork + exec of the session shell (PROC), pipes to the child (IPC), signals
 * to the child (SIGNAL), timeouts (TIME). Everything else (POWER, Driver Space)
 * is denied and audited. The flags are covered by the QSIG Ed25519 signature.
 * SPDX-License-Identifier: MIT */
.section .note.secos, "a", @note
.align 4

.long 6                 /* namesz */
.long 24                /* descsz */
.long 0x51534543        /* type = 'QSEC' */
.ascii "SECOS\0"
.balign 4
.long 1                 /* version */
.long 0xFF0             /* flags = CAP_ENFORCE|NET|FS_READ|FS_WRITE|PROC|IPC|SIGNAL|TIME */
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
