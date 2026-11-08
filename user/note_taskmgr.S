/* SecOS .note.secos — [U5] manifest of /bin/taskmgr, the task manager.
 * The confined client manifest (note_confined.S) plus CAP_SIGNAL:
 * flags = CAP_ENFORCE | CAP_FS_READ | CAP_SIGNAL | CAP_TIME. It reads /proc and
 * /etc/passwd, draws its window and sends signals ("End task"); file writes,
 * spawn, pipe/IPC, sockets and the rest stay denied (and audited). Which
 * processes it may signal is decided by the kernel's credential rules
 * (cred_may_signal: root, or the same user), not by this bit. The flags are
 * covered by the QSIG Ed25519 signature.
 * SPDX-License-Identifier: MIT */
.section .note.secos, "a", @note
.align 4

/* CAP_ENFORCE(1<<5)=0x20 | CAP_FS_READ(1<<6)=0x40 | CAP_SIGNAL(1<<10)=0x400 |
 * CAP_TIME(1<<11)=0x800 = 0xC60 */
.long 6                 /* namesz */
.long 24                /* descsz */
.long 0x51534543        /* type = 'QSEC' */
.ascii "SECOS\0"
.balign 4
.long 1                 /* version */
.long 0xC60             /* flags = CAP_ENFORCE|CAP_FS_READ|CAP_SIGNAL|CAP_TIME */
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
