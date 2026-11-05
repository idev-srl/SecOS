/*
 * kiface.h — kernel interfaces for standard Linux userland tools (syscalls
 * 65-79: clocks, nanosleep, syslog, statfs, sync, rename, link, truncate,
 * rmdir, access, umask, uname, poll, sysinfo) plus the per-process / per-CPU
 * accounting /proc reads. See kernel/kiface.c.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SECOS_KIFACE_H
#define SECOS_KIFACE_H
#include <stdint.h>
#include "trapframe.h"

struct process;

/* libc places socket descriptors at SECOS_SOCKFD_BASE + kernel sockfd so they
 * share one namespace with file fds (poll() decodes them the same way). */
#define SECOS_SOCKFD_BASE 64

/* Process lifecycle hooks (kernel/process.c). argv may be NULL. */
void kiface_proc_init(struct process* p, int argc, const char* const* argv);
void kiface_proc_fork(struct process* child);
/* (Re)set the name /proc reports (comm = basename of argv[0], else of path). */
void kiface_proc_set_cmdline(struct process* p, int argc, const char* const* argv, const char* path);

/* Scheduler hooks (kernel/sched.c). */
void kiface_account_tick(struct process* cur, int from_user, int is_idle);
void kiface_count_switch(void);

/* Syscalls 65..79: returns 1 and sets *ret when `num` is one of them. May block
 * (poll, nanosleep): then it does not return (rip rewound, task switched). */
int  kiface_syscall(trapframe_t* tf, uint64_t num, uint64_t* ret);

/* Apply mode & ~umask to an object the caller just created (open O_CREAT,
 * mkdir). mode 0 = the POSIX default (0666 file / 0777 dir). */
void kiface_apply_create_mode(const char* abs_path, uint32_t mode, int is_dir);

/* /proc data. */
typedef struct kiface_cpu_stat { uint64_t user, sys, idle; } kiface_cpu_stat_t;
uint32_t kiface_cpu_count(void);
void     kiface_cpu_stat(uint32_t cpu, kiface_cpu_stat_t* out);
uint64_t kiface_ctxt(void);
uint64_t kiface_forks(void);
uint32_t kiface_last_pid(void);
/* Load averages, fixed point with 11 fractional bits (Linux FSHIFT). */
void     kiface_loadavg(uint64_t out[3]);

#endif
