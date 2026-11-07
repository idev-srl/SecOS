/*
 * bkl.c — the kernel lock (M48) on top of klock (M50). See bkl.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "bkl.h"
#include "klock.h"
#include "debugcon.h"
#include "syscall.h"

klock_t g_bkl = KLOCK_INIT_RECURSIVE(LK_K0_BKL, "bkl");

/* [M50] A task spinning for the BKL (interrupts on) polls the USB HID
 * keyboard, at most once a millisecond system-wide. The console reader is the
 * only regular poller (keyboard_has_char / keyboard_getc_raw), and it must
 * win the BKL first; with fork, exec (signature check) and disk syscalls
 * holding the BKL for 10-30 ms back to back, the poll gap grew past the
 * ~30 ms a key stays pressed, and QEMU's usb-kbd (like a real keyboard
 * report) then never shows that key: keystrokes were lost under load (M50 B3
 * gate). usb_hid_poll() only trylocks the HID and xHCI locks, so it never
 * waits here; nothing is held while it runs (the BKL is the outermost lock). */
extern void usb_hid_poll(void);
extern uint64_t tsc_read(void);
extern uint64_t tsc_hz(void);
static volatile uint64_t g_bkl_poll_tsc;
void bkl_wait_poll(void) {
    uint64_t hz = tsc_hz(), now = tsc_read(), last = g_bkl_poll_tsc;
    if (!hz || now - last < hz / 1000) return;
    if (!__sync_bool_compare_and_swap(&g_bkl_poll_tsc, last, now)) return;   /* another CPU polls */
    usb_hid_poll();
}

void bkl_lock(void)            { klock_lock(&g_bkl); }
void bkl_lock_enable_irq(void) { klock_lock_irq_on(&g_bkl); }
void bkl_unlock(void)          { klock_unlock(&g_bkl); }

uint32_t bkl_suspend(void)          { return klock_suspend(&g_bkl); }
void     bkl_resume(uint32_t depth) { klock_resume(&g_bkl, depth); }
void     bkl_drop_for(struct process* p) { klock_drop_owner(&g_bkl, p); }
int      bkl_held_by(struct process* p)  { return klock_owned_by(&g_bkl, p); }

/* ---- [M50] mode + per-syscall mask ---- */
int g_bkl_mode = 1;

/* Every syscall starts under the BKL; phase D of the BKL removal (M52,
 * docs/devlog/M52-bkl-removal.md) lists below, step by step, the ones whose
 * subsystems have their own locks. bkl_init_mode clears their bits (they
 * only matter with bkl=off). A syscall not listed keeps the BKL, as does any
 * number past SYSCALL_BKL_NR. */
#define ALL64 0xFFFFFFFFFFFFFFFFULL
uint64_t syscall_bkl_mask[SYSCALL_BKL_WORDS] = { ALL64, ALL64, ALL64, ALL64 };
typedef char syscall_bkl_mask_init_covers_all[(SYSCALL_BKL_WORDS == 4) ? 1 : -1];

/* D1: process-local state, signals, sockets, pipes/IPC, scheduling, memory
 * (VMAs + page tables under mm_lock), credentials (published under
 * proc_lock), fd-table ops that call no filesystem (close/dup/dup2/fcntl:
 * stores under mm_lock, object refs under each object's lock). */
static const uint16_t g_bkl_free_d1[] = {
    SYS_YIELD, SYS_CLOSE, SYS_GETPID, SYS_DRIVER, SYS_WAIT, SYS_GETTICKS,
    SYS_MSG_SEND, SYS_MSG_RECV, SYS_SLEEP, SYS_MMAP, SYS_MUNMAP, SYS_BRK,
    SYS_MPROTECT,
    SYS_SOCKET, SYS_CONNECT, SYS_BIND, SYS_LISTEN, SYS_ACCEPT, SYS_SEND,
    SYS_RECV, SYS_SENDTO, SYS_RECVFROM, SYS_SOCKCLOSE, SYS_SOCKNAME, SYS_NETCTL,
    SYS_PIPE,
    SYS_SIGACTION, SYS_SIGRETURN, SYS_KILL, SYS_SIGPROCMASK, SYS_SETPGID,
    SYS_GETPGID, SYS_SIGSUSPEND, SYS_SETSID, SYS_GETSID,
    SYS_DUP2, SYS_DUP, SYS_FCNTL, SYS_GETPPID, SYS_WAITANY, SYS_REBOOT,
    SYS_PRCTL, SYS_SCHED_GETAFFINITY,
    SYS_CLOCK_GETTIME, SYS_CLOCK_SETTIME, SYS_NANOSLEEP, SYS_SYSLOG,
    SYS_UMASK, SYS_UNAME, SYS_SYSINFO,
    SYS_GETUID, SYS_GETEUID, SYS_GETGID, SYS_GETEGID, SYS_SETUID, SYS_SETGID,
    SYS_SETRESUID, SYS_SETRESGID, SYS_GETGROUPS, SYS_SETGROUPS,
};

/* D3: file syscalls. Every filesystem op runs under its driver's instance
 * lock (M51 C2-C4), namespace changes under vfs_ns_lock with unmount
 * draining (C1); open pins the mount until its fd is installed; O_APPEND
 * resolves the end of file under the FS lock; open(O_TRUNC), truncate,
 * chmod, chown and utimes act on the inode they checked (inode-keyed
 * ftruncate/fsetattr), and new entries get their final attributes in the
 * driver section that creates them. */
static const uint16_t g_bkl_free_d3[] = {
    SYS_OPEN, SYS_LSEEK, SYS_STAT, SYS_LSTAT, SYS_FSTAT, SYS_CHMOD, SYS_CHOWN,
    SYS_UTIMES, SYS_READLINK, SYS_SYMLINK, SYS_GETDENTS, SYS_CREATE, SYS_MKDIR,
    SYS_UNLINK, SYS_CHDIR, SYS_GETCWD, SYS_STATFS, SYS_SYNC, SYS_RENAME,
    SYS_LINK, SYS_TRUNCATE, SYS_RMDIR, SYS_ACCESS,
};

/* D4: read/write/ioctl/poll, which dispatch on the fd type: files (page cache
 * or the driver under its lock, user copies through a bounce buffer outside
 * every lock), pipes (g_pipe_lock), sockets (net_lock), ptys (g_pty_lock),
 * the console line discipline (tty_con_lock; its waits hold no klock),
 * devfs nodes (no devfs klock on the console/tty paths; /dev/random in
 * 256-byte csprng chunks; raw block nodes under the base disk's lock),
 * evdev (g_input_lock), fb0 (stateless; KD mode under con_lock), procfs and
 * sysfs (their own locks, generated into kernel buffers). */
static const uint16_t g_bkl_free_d4[] = {
    SYS_READ, SYS_WRITE, SYS_IOCTL, SYS_POLL,
};

/* D5: process creation and the namespace. fork/spawn/execve: pid, home CPU
 * and the table under proc_lock, kernel stacks under kstack_lock, the
 * address space under mm_lock, fd object refs under each object's lock, a
 * new process is EMBRYO until it is built; spawn reads, checks and set-ids
 * one pinned inode; exec images are per call (no shared staging buffer).
 * mount/umount: under vfs_ns_lock (the kernel shell's mounts too), umount
 * drains in-flight operations and then checks open fds and mappings. */
static const uint16_t g_bkl_free_d5[] = {
    SYS_FORK, SYS_SPAWN, SYS_EXECVE, SYS_MOUNT, SYS_UMOUNT,
};

/* D6: exit (its teardown takes the BKL only with bkl=on, kernel/sched.c).
 * After D6 no syscall takes the BKL with bkl=off. The bits still set are
 * numbers no syscall uses (they return -ENOSYS at once): they stay set so a
 * syscall added later starts under the BKL until it has been audited and
 * listed here.
 *
 * g_bkl_keep puts a syscall back under the BKL with bkl=off (a hidden BKL
 * dependency found later can be parked here without a redesign). Empty. */
static const uint16_t g_bkl_free_d6[] = { SYS_EXIT };
static const uint16_t g_bkl_keep[] = { 0xFFFF };   /* 0xFFFF: no entry */

static void bkl_mask_clear(const uint16_t* nums, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        if (nums[i] < SYSCALL_BKL_NR)
            syscall_bkl_mask[nums[i] >> 6] &= ~(1ULL << (nums[i] & 63));
}
#define BKL_MASK_CLEAR(a) bkl_mask_clear((a), sizeof(a) / sizeof((a)[0]))

/* [M52] The mode when the command line names none (Makefile BKL_DEFAULT). */
#ifdef SECOS_BKL_DEFAULT_OFF
#define BKL_MODE_DEFAULT 0
#else
#define BKL_MODE_DEFAULT 1
#endif

void bkl_init_mode(void) {
    extern int kernel_cmdline_has(const char* word);
    BKL_MASK_CLEAR(g_bkl_free_d1);
    BKL_MASK_CLEAR(g_bkl_free_d3);
    BKL_MASK_CLEAR(g_bkl_free_d4);
    BKL_MASK_CLEAR(g_bkl_free_d5);
    BKL_MASK_CLEAR(g_bkl_free_d6);
    for (unsigned i = 0; i < sizeof(g_bkl_keep) / sizeof(g_bkl_keep[0]); i++)
        if (g_bkl_keep[i] < SYSCALL_BKL_NR)
            syscall_bkl_mask[g_bkl_keep[i] >> 6] |= 1ULL << (g_bkl_keep[i] & 63);
    if (kernel_cmdline_has("bkl=off")) g_bkl_mode = 0;
    else if (kernel_cmdline_has("bkl=on")) g_bkl_mode = 1;
    else g_bkl_mode = BKL_MODE_DEFAULT;
    debugcon_writestring(g_bkl_mode ? "[BKL] kernel lock mode: bkl=on"
                                    : "[BKL] kernel lock mode: bkl=off");
    debugcon_writestring(BKL_MODE_DEFAULT == g_bkl_mode ? " (default)" : " (boot word)");
    debugcon_writestring(", syscall numbers under the BKL (unused ones included): ");
    debugcon_print_hex(syscall_bkl_mask_count());
    debugcon_writestring("/0x100\n");
}

int syscall_needs_bkl(uint64_t num) {
    if (g_bkl_mode || num >= SYSCALL_BKL_NR) return 1;
    return (int)((syscall_bkl_mask[num >> 6] >> (num & 63)) & 1);
}

uint32_t syscall_bkl_mask_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < SYSCALL_BKL_WORDS; i++)
        for (uint64_t w = syscall_bkl_mask[i]; w; w &= w - 1) n++;
    return n;
}

int bkl_syscall_enter(uint64_t num) {
    if (!syscall_needs_bkl(num)) return 0;
    klock_lock_irq_on(&g_bkl);
    return 1;
}

void bkl_syscall_exit(int taken) {
    if (taken) klock_unlock_to_user(&g_bkl);    /* unlock + check in one step */
    else klock_check_user_return("syscall exit");
}
