/*
 * thread.h — [U9] threads: the shared parts of a process and their lifetime.
 * SPDX-License-Identifier: MIT
 *
 * docs/USERLAND_PLAN.md D5 (option T2), docs/devlog/U9-threads.md. A thread is
 * a process_t (the schedulable task); the threads of a process share one
 * proc_mm_t (address space) and one thread_group_t (fds, cwd, umask, signal
 * dispositions, process-directed pending signals, group exit/stop state),
 * both defined in process.h. Every process_t holds one reference on each.
 *
 * Process-wide values that stay per-process_t copies (credentials, pgid, sid,
 * ppid, the controlling tty, capabilities, no_new_privs, mem_limit) are kept
 * identical in every thread: a change is written to all of them under
 * proc_lock (thread_group_sync_*), or happens only when the process has one
 * thread (execve kills the others first).
 */
#ifndef THREAD_H
#define THREAD_H
#include <stdint.h>
#include "process.h"
#include "trapframe.h"

// Linux clone flags (the subset SecOS knows about).
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SYSVSEM        0x00040000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_DETACHED       0x00400000
#define CLONE_CHILD_SETTID   0x01000000
// What a thread must share (musl's pthread_create asks for exactly these plus
// the optional bits below); anything else is -EINVAL.
#define CLONE_THREAD_REQUIRED (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD)
#define CLONE_THREAD_OPTIONAL (CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID | \
                               CLONE_CHILD_CLEARTID | CLONE_DETACHED | CLONE_CHILD_SETTID)

// ---- the shared objects ----
proc_mm_t*      mm_alloc(void);                 // refs 1, empty, lock initialised
void            mm_get(proc_mm_t* mm);
void            mm_put(proc_mm_t* mm);          // the last put destroys space + image
thread_group_t* tg_alloc(uint32_t tgid);        // refs 1, nr_live 1, fds 0-2 reserved, cwd "/"
thread_group_t* tg_copy_for_fork(const thread_group_t* src, uint32_t tgid);  // fd objects NOT re-referenced
void            tg_get(thread_group_t* tg);
void            tg_put(thread_group_t* tg);
// Point p at mm/tg (no reference taken) and set the fds/cwd/sig_handler aliases.
void thread_bind(process_t* p, proc_mm_t* mm, thread_group_t* tg);
// Kernel tasks (idle tasks, demo PCBs) get a private mm + group on first use.
void thread_attach_kernel_task(process_t* p);

// ---- queries ----
static inline int thread_is_leader(const process_t* p) { return p->pid == p->tgid; }
int  thread_group_live(const process_t* p);     // live threads (racy read: a hint)
int  thread_count_tg(uint32_t tgid);            // live threads of tgid (proc_lock)

// ---- clone / exit ----
// clone(2) with the thread flag set (SecOS 512 + 56). Returns the new tid or
// -errno. The new thread is PROC_NEW; the caller publishes it.
long thread_clone(process_t* cur, trapframe_t* tf, uint64_t flags, uint64_t newsp,
                  uint64_t ptid, uint64_t ctid, uint64_t tls, process_t** out);
// The exit bookkeeping of the CURRENT thread (called by the scheduler's exit
// paths before they switch away; nothing held). 'code' is the thread's own
// wait status; 'group' = 1 for exit_group/SYS_EXIT/a fatal signal (the other
// threads are killed). Returns 1 when this thread ended the process (the
// caller then does the process-exit notifications for *lead_pid / *code_out).
int  thread_exit_begin(process_t* cur, int code, int group);
// After thread_exit_begin returned 1: the leader's pid and the group status
// (also stored as the leader's exit_code), then, once the fds are closed,
// thread_mark_dead() makes the leader collectable.
void thread_exit_group_info(process_t* cur, uint32_t* tgid, int* code);
void thread_mark_dead(process_t* cur);
// Kill every other thread of cur's group (SIGKILL, thread_killed). 'group'
// = 1: as a group exit with 'code'; 0: for execve (no group exit).
void thread_kill_others(process_t* cur, int group, int code);
// execve from a multi-threaded process: 1 while other threads are still
// alive (the caller sleeps a tick and re-runs execve), 0 when cur is alone
// (and, if it was not the leader, has taken over the leader's pid), -1 when
// the group is exiting (cur must die).
int  thread_exec_dethread(process_t* cur);
// Thread-exit bookkeeping for CLONE_CHILD_CLEARTID (zero + futex wake).
void thread_clear_child_tid(process_t* cur);

// ---- per-syscall fd pin ----
// Called at syscall entry/exit (syscall_trap.c) and before a task blocks,
// stops or exits (sched.c). Only does something in a multi-threaded process
// (with one thread nobody else can close the fd under the call).
void thread_fd_pin(process_t* cur, uint64_t num, uint64_t fd);
void thread_fd_unpin(process_t* cur);

// ---- process-wide copies ----
// Copy cur's credentials / pgid+sid / no_new_privs to every other thread of
// its group. proc_lock held by the caller (process_foreach_locked).
void thread_group_sync_locked(process_t* src, int what);
#define TG_SYNC_CRED  1
#define TG_SYNC_PGID  2
#define TG_SYNC_NNP   4

// Boot word `nothreads`: clone with thread flags is refused (-EINVAL).
extern int g_threads_enabled;
void thread_boot_init(void);

#endif // THREAD_H
