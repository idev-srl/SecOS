/* <secos/secos.h> — SecOS-only system calls for programs built with musl
 * (tools/secos-cc; link with -lsecos). Everything POSIX/Linux is in musl;
 * this is what has no Linux equivalent. Every function returns the kernel's
 * raw result: >= 0 on success, a negative errno (or -1) on failure; errno is
 * not touched. ABI: kernel/syscall.h. SPDX-License-Identifier: MIT */
#ifndef SECOS_SECOS_H
#define SECOS_SECOS_H
#ifdef __cplusplus
extern "C" {
#endif

/* Raw SecOS syscall (int $0x80: rax = num, rdi, rsi, rdx, rcx, r8, r9). */
long secos_syscall(long num, long a0, long a1, long a2, long a3, long a4, long a5);

/* SYS_SPAWN: load, verify and start a signed program; returns its pid. The
 * child inherits fds 0-2, cwd, umask, credentials and the process group. */
int  secos_spawn(const char *path, char *const argv[]);
/* SYS_WAIT: block until child 'pid' exits; returns its wait status. */
int  secos_wait(int pid);
/* SYS_GETTICKS: uptime in scheduler ticks (1 kHz). */
unsigned long secos_getticks(void);
/* SYS_SLEEP: block for 'ticks' scheduler ticks. */
void secos_sleep_ticks(unsigned long ticks);
/* SYS_MSG_SEND / SYS_MSG_RECV: kernel IPC channels 0-3 (recv blocks). */
long secos_msg_send(int chan, const void *buf, long len);
long secos_msg_recv(int chan, void *buf, long len);
/* SYS_NETCTL: NIC info / static config / DHCP (CAP_NET; net/netctl.c). */
long secos_netctl(int op, int ifindex, void *arg);
/* prctl(PR_SECOS_GET_CAPS): effective MANIFEST_FLAG_CAP_* bits, | bit 30 if
 * confined, | bit 29 under no_new_privs. */
long secos_get_caps(void);
#define SECOS_CAPS_CONFINED (1L << 30)
#define SECOS_CAPS_NNP      (1L << 29)
/* SYS_REBOOT: 1 = power off, 2 = restart, 3 = halt (orderly; root). */
int  secos_reboot(int cmd);
/* SYS_SIGVERIFY: Ed25519 signature over a SHA-256 digest against the
 * kernel's trusted key 'key_id': 0 = valid, -74 (EBADMSG) = invalid/revoked. */
int  secos_sigverify(const void *digest32, const void *sig64, unsigned key_id);

#ifdef __cplusplus
}
#endif
#endif
