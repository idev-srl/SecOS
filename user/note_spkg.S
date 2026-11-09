/* SecOS .note.secos — [U6] manifest of /bin/spkg (package installer).
 * flags = CAP_ENFORCE | CAP_FS_READ | CAP_FS_WRITE | CAP_SIGNAL | CAP_TIME:
 * spkg reads packages and writes /usr, /opt, /etc and /var/lib/spkg (as
 * root), probes its lock owner with kill(pid, 0) and reads the clock; it
 * never spawns, opens sockets or uses IPC, so a bug in it cannot. Being
 * CAP_ENFORCE it also runs with no_new_privs (M46).
 * SPDX-License-Identifier: MIT */
.section .note.secos, "a", @note
.align 4

/* 0x20 | 0x40 | 0x80 | 0x400 | 0x800 = 0xCE0 */
.long 6                 /* namesz */
.long 24                /* descsz */
.long 0x51534543        /* type = 'QSEC' */
.ascii "SECOS\0"
.balign 4
.long 1                 /* version */
.long 0xCE0             /* flags */
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
