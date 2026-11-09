/*
 * thread.c — [U9] threads: shared address space / process state, clone, exit.
 * SPDX-License-Identifier: MIT
 *
 * See thread.h and docs/devlog/U9-threads.md. Locking:
 *   - mm->refs / tg->refs: atomics; the last put frees (never under a lock).
 *   - tg->nr_live, group_exit/group_code, exec_tid, stop_*: proc_lock (S30).
 *   - the fd table / cwd: as before (kernel/syscall.c): stores that change
 *     what an fd is are made under mm->lock (procfs reads them), and since
 *     U9 the read-modify-write paths (close, dup*) run under it too.
 *   - nothing here holds a lock across a user copy, a block or switch_to.
 */
#include "thread.h"
#include "process.h"
#include "sched.h"
#include "signal.h"
#include "futex.h"
#include "heap.h"
#include "debugcon.h"
#include "user_copy.h"
#include "pty.h"
#include "vmm.h"
#include "mm/elf_manifest.h"
#include "klock.h"
#include "syscall.h"

extern int  kernel_cmdline_has(const char* word);
extern void kiface_proc_fork(process_t* c);
extern int  process_table_add(process_t* p);
extern void process_table_remove_unbuilt(process_t* p);
extern uint32_t process_alloc_pid(void);

int g_threads_enabled = 1;

void thread_boot_init(void) {
    if (kernel_cmdline_has("nothreads")) {
        g_threads_enabled = 0;
        debugcon_writestring("[THREAD] nothreads: clone(CLONE_THREAD) refused\n");
    }
}

static void zero(void* d, size_t n) { uint8_t* p = (uint8_t*)d; while (n--) *p++ = 0; }

// ---- proc_mm_t ----
proc_mm_t* mm_alloc(void) {
    proc_mm_t* mm = (proc_mm_t*)kmalloc(sizeof(proc_mm_t));
    if (!mm) return NULL;
    zero(mm, sizeof(*mm));
    mm->refs = 1;
    spin_init_class_dynamic(&mm->lock, LK_S35_MM, "mm");
    klock_init(&mm->map_lock, LK_K13_MMAP, "mmap", 0);
    return mm;
}
void mm_get(proc_mm_t* mm) { if (mm) __atomic_add_fetch(&mm->refs, 1, __ATOMIC_RELAXED); }
void mm_put(proc_mm_t* mm) {
    if (!mm) return;
    if (__atomic_sub_fetch(&mm->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    // Last thread gone: nobody runs on these page tables any more (the
    // reaper only destroys a task once no CPU is on it).
    if (mm->image) kfree(mm->image);
    if (mm->space) vmm_space_destroy(mm->space);    // frees every present user frame
    kfree(mm);
}

// ---- thread_group_t ----
static void tg_init_common(thread_group_t* tg, uint32_t tgid) {
    klock_init(&tg->fd_lock, LK_K12_FILES, "files", 0);
    tg->refs = 1;
    tg->tgid = tgid;
    tg->nr_live = 1;
}
thread_group_t* tg_alloc(uint32_t tgid) {
    thread_group_t* tg = (thread_group_t*)kmalloc(sizeof(thread_group_t));
    if (!tg) return NULL;
    zero(tg, sizeof(*tg));
    tg_init_common(tg, tgid);
    // [M25] fds 0/1/2 reserved as stdin/stdout/stderr (no inode: the console
    // special-cases are keyed on the fd number).
    tg->fd[0].used = 1; tg->fd[1].used = 1; tg->fd[2].used = 1;
    tg->cwd[0] = '/'; tg->cwd[1] = 0;
    tg->umask = 022;
    return tg;
}
thread_group_t* tg_copy_for_fork(const thread_group_t* src, uint32_t tgid) {
    thread_group_t* tg = (thread_group_t*)kmalloc(sizeof(thread_group_t));
    if (!tg) return NULL;
    zero(tg, sizeof(*tg));
    tg_init_common(tg, tgid);
    for (int i = 0; i < PROC_NFDS; i++) tg->fd[i] = src->fd[i];   // the caller refs the objects
    for (int i = 0; i < (int)sizeof(tg->cwd); i++) tg->cwd[i] = src->cwd[i];
    tg->cwd[sizeof(tg->cwd) - 1] = 0;
    tg->umask = src->umask;
    for (int i = 0; i < PROC_NSIG; i++) tg->handler[i] = src->handler[i];
    tg->restorer = src->restorer;
    return tg;
}
void tg_get(thread_group_t* tg) { if (tg) __atomic_add_fetch(&tg->refs, 1, __ATOMIC_RELAXED); }
void tg_put(thread_group_t* tg) {
    if (!tg) return;
    if (__atomic_sub_fetch(&tg->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    kfree(tg);   // the fd objects were dropped when the last thread exited
}

void thread_bind(process_t* p, proc_mm_t* mm, thread_group_t* tg) {
    p->mm = mm;
    p->tg = tg;
    p->space = mm ? mm->space : NULL;
    p->fds = tg->fd;
    p->cwd = tg->cwd;
    p->sig_handler = tg->handler;
}

void thread_attach_kernel_task(process_t* p) {
    if (!p || p->mm) return;
    proc_mm_t* mm = mm_alloc();
    thread_group_t* tg = tg_alloc(p->pid);
    if (!mm || !tg) {
        debugcon_writestring("[THREAD] kernel task: mm/group alloc failed\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    vmm_space_t* sp = p->space;        // the kernel space: not owned
    thread_bind(p, mm, tg);
    p->space = sp;
    p->tgid = p->pid;
}

int thread_group_live(const process_t* p) {
    return p && p->tg ? (int)__atomic_load_n(&p->tg->nr_live, __ATOMIC_RELAXED) : 1;
}

struct cnt_ctx { uint32_t tgid; int n; };
static void cnt_cb(process_t* p, void* u) {
    struct cnt_ctx* c = (struct cnt_ctx*)u;
    if (p->tgid == c->tgid && p->state != PROC_ZOMBIE) c->n++;
}
int thread_count_tg(uint32_t tgid) {
    struct cnt_ctx c = { tgid, 0 };
    process_foreach(cnt_cb, &c);
    return c.n;
}

// ---- per-syscall fd pin ----
// Syscalls whose first argument is an fd whose object (pipe end, socket,
// pty end, evdev client, udrv claim) the body uses through a raw pointer.
// (poll pins nothing here: kiface's readiness scan runs under fd_lock.)
extern void ksys_fd_entry_ref(struct proc_fd_entry* e);
extern void ksys_fd_entry_unref(struct proc_fd_entry* e);
static int fd_pin_call(uint64_t num) {
    switch (num) {
    case SYS_READ: case SYS_WRITE: case SYS_IOCTL: case SYS_FSTAT: case SYS_LSEEK:
    case SYS_FCNTL: case SYS_DUP: case SYS_DUP2:
    case SYS_RECV: case SYS_SEND: case SYS_RECVFROM: case SYS_SENDTO:
    case SYS_ACCEPT: case SYS_CONNECT: case SYS_BIND: case SYS_LISTEN:
        return 1;
    default:
        return 0;
    }
}
void thread_fd_pin(process_t* cur, uint64_t num, uint64_t fd) {
    if (cur->fd_pin_on || !cur->tg || fd >= PROC_NFDS || !fd_pin_call(num)) return;
    if (__atomic_load_n(&cur->tg->nr_live, __ATOMIC_RELAXED) <= 1) return;
    klock_lock(&cur->tg->fd_lock);
    struct proc_fd_entry e = cur->fds[fd];
    if (e.used && (e.is_pipe || e.is_sock || e.inode)) {
        ksys_fd_entry_ref(&e);
        cur->fd_pin = e;
        cur->fd_pin_on = 1;
    }
    klock_unlock(&cur->tg->fd_lock);
}
void thread_fd_unpin(process_t* cur) {
    if (!cur || !cur->fd_pin_on) return;
    struct proc_fd_entry e = cur->fd_pin;
    cur->fd_pin_on = 0;
    ksys_fd_entry_unref(&e);
}

// ---- process-wide copies ----
struct sync_ctx { process_t* src; int what; };
static void sync_cb(process_t* p, void* u) {
    struct sync_ctx* s = (struct sync_ctx*)u;
    if (p == s->src || p->tg != s->src->tg) return;
    if (s->what & TG_SYNC_CRED) p->cred = s->src->cred;
    if (s->what & TG_SYNC_PGID) { p->pgid = s->src->pgid; p->sid = s->src->sid; }
    if (s->what & TG_SYNC_NNP)  p->no_new_privs = s->src->no_new_privs;
}
void thread_group_sync_locked(process_t* src, int what) {
    if (!src || !src->tg || __atomic_load_n(&src->tg->nr_live, __ATOMIC_RELAXED) <= 1) return;
    struct sync_ctx s = { src, what };
    process_foreach_locked(sync_cb, &s);
}

// ---- clone ----
#define E_PERM   1
#define E_AGAIN  11
#define E_NOMEM  12
#define E_FAULT  14
#define E_INVAL  22

long thread_clone(process_t* cur, trapframe_t* tf, uint64_t flags, uint64_t newsp,
                  uint64_t ptid, uint64_t ctid, uint64_t tls, process_t** out) {
    *out = NULL;
    if (!cur || !tf || !cur->mm || !cur->tg) return -E_INVAL;
    if ((flags & CLONE_THREAD_REQUIRED) != CLONE_THREAD_REQUIRED) return -E_INVAL;
    if (flags & ~(uint64_t)(CLONE_THREAD_REQUIRED | CLONE_THREAD_OPTIONAL)) return -E_INVAL;
    if (!g_threads_enabled) return -E_INVAL;                  // boot word `nothreads`
    // A ring-3 driver's claims, BAR and DMA mappings belong to one task
    // (kernel/udrv.c): drivers stay single-threaded.
    if (cur->proc_type == PROC_TYPE_DRIVER) return -E_PERM;
    if (!newsp || newsp >= USER_ADDR_MAX) return -E_INVAL;
    if ((flags & CLONE_SETTLS) && tls >= USER_ADDR_MAX) return -E_PERM;

    process_t* c = (process_t*)kmalloc(sizeof(process_t));
    if (!c) return -E_NOMEM;
    *c = *cur;                                     // mm, tg, aliases, creds, caps, ids
    klock_task_reset(&c->klk);
    c->refs = 0;
    c->pid = process_alloc_pid();
    c->tgid = cur->tgid;
    c->cpu_affinity = process_pick_home();
    c->on_cpu = 0; c->wake_snap = 0; c->ready_tick = 0; c->smp_logged = 0;
    c->kstack_top = 0; c->kstack_slot = 0; c->mapped_pages = NULL; c->mapped_page_count = 0;
    c->tf = NULL; c->manifest = NULL;
    c->sig_pending = 0;                            // the mask is inherited (Linux)
    c->sig_eintr_rip = 0; c->sig_rerun_eintr = 0;
    c->sigsusp_active = 0; c->sigsusp_saved = 0;
    c->wait_pid = -1; c->wait_result = 0; c->wait_ready = 0; c->collected = 0;
    c->sleep_until = 0; c->recv_chan = -1; c->exit_code = 0; c->cpu_ticks = 0;
    c->wait_pipe = NULL; c->pipe_wdone = 0; c->wait_tty = 0; c->wait_pty = NULL;
    c->wait_udrv = NULL; c->power_phase = 0; c->power_deadline = 0;
    c->stop_report = 0; c->cont_report = 0; c->stop_sig = 0;
    c->drv_dev_id = -1; c->drv_caps = 0;
    c->robust_list = 0; c->thread_killed = 0; c->fd_pin_on = 0;
    c->futex_next = NULL; c->futex_key0 = c->futex_key1 = 0; c->futex_bits = 0;
    c->futex_active = 0; c->futex_queued = 0; c->futex_woken = 0; c->futex_deadline = 0;
    c->fs_base = (flags & CLONE_SETTLS) ? tls : cur->fs_base;
    c->clear_child_tid = (flags & CLONE_CHILD_CLEARTID) ? ctid : 0;
    // The creator's live x87/SSE registers (MXCSR, control word: Linux copies them).
    __asm__ volatile("fxsave64 %0" : "=m"(*(uint8_t(*)[512])c->fpu));
    c->fpu_valid = 1;
    c->state = PROC_EMBRYO;
    { extern void kiface_proc_fork(process_t*); kiface_proc_fork(c); }

    if (cur->manifest) {                           // per-task copy (freed by process_destroy)
        c->manifest = kmalloc(sizeof(elf_manifest_t));
        if (!c->manifest) { kfree(c); return -E_NOMEM; }
        const uint8_t* s = (const uint8_t*)cur->manifest; uint8_t* d = (uint8_t*)c->manifest;
        for (size_t i = 0; i < sizeof(elf_manifest_t); i++) d[i] = s[i];
    }
    c->tf = (trapframe_t*)kmalloc(sizeof(trapframe_t));
    if (!c->tf) { if (c->manifest) kfree(c->manifest); kfree(c); return -E_NOMEM; }
    *c->tf = *tf;                                  // same registers as the caller...
    c->tf->rax = 0;                                // ...clone() returns 0 in the thread
    c->tf->rsp = newsp;                            // on its own stack

    uint32_t tid = c->pid;
    // CLONE_PARENT_SETTID / CLONE_CHILD_SETTID: the tid at those addresses
    // (one address space, so both writes land in our memory) before the
    // thread can run; a bad pointer is EFAULT and nothing was created.
    if ((flags & CLONE_PARENT_SETTID) && copy_to_user((void*)ptid, &tid, sizeof(tid)) != 0) {
        kfree(c->tf); if (c->manifest) kfree(c->manifest); kfree(c); return -E_FAULT;
    }
    if ((flags & CLONE_CHILD_SETTID) && copy_to_user((void*)ctid, &tid, sizeof(tid)) != 0) {
        kfree(c->tf); if (c->manifest) kfree(c->manifest); kfree(c); return -E_FAULT;
    }

    if (process_table_add(c) != 0) {
        kfree(c->tf); if (c->manifest) kfree(c->manifest); kfree(c); return -E_AGAIN;
    }
    c->kstack_top = vmm_alloc_kernel_stack_for_slot(c->kstack_slot);
    if (!c->kstack_top) {
        process_table_remove_unbuilt(c);
        kfree(c->tf); if (c->manifest) kfree(c->manifest); kfree(c); return -E_NOMEM;
    }
    // Join the group (atomic with a group exit / exec starting on another CPU:
    // a thread created after the kill sweep would survive it).
    int joined = 0;
    {
        uint64_t fl = process_lock_irqsave();
        thread_group_t* tg = cur->tg;
        if (!tg->group_exit && !tg->exec_tid && !cur->thread_killed) {
            tg->nr_live++;
            joined = 1;
        }
        process_unlock_irqrestore(fl);
    }
    if (!joined) {
        vmm_free_kernel_stack_for_slot(c->kstack_slot); c->kstack_top = 0;
        process_table_remove_unbuilt(c);
        kfree(c->tf); if (c->manifest) kfree(c->manifest); kfree(c); return -E_AGAIN;
    }
    mm_get(c->mm);
    tg_get(c->tg);
    pty_ctty_ref(c->ctty);                         // every task holds its ctty reference
    debugcon_writestring("[CLONE] tgid="); debugcon_print_hex(c->tgid);
    debugcon_writestring(" tid="); debugcon_print_hex(c->pid); debugcon_writestring("\n");
    c->state = PROC_NEW;
    *out = c;
    return (long)tid;
}

// ---- exit ----
void thread_clear_child_tid(process_t* cur) {
    uint64_t a = cur->clear_child_tid;
    if (!a) return;
    cur->clear_child_tid = 0;
    uint32_t z = 0;
    // The address space is still ours (and shared): zero the word and wake one
    // waiter (pthread_join, musl's thread-list lock). A bad pointer is ignored.
    if ((a & 3) == 0 && copy_to_user((void*)a, &z, sizeof(z)) == 0)
        futex_wake_addr(cur, a, 1);
}

struct kill_ctx { process_t* self; int n; process_t* t[64]; int more; };
static void kill_cb(process_t* p, void* u) {
    struct kill_ctx* k = (struct kill_ctx*)u;
    if (p == k->self || p->tg != k->self->tg) return;
    if (p->state == PROC_ZOMBIE || p->thread_killed) return;
    if (k->n >= 64) { k->more = 1; return; }
    p->thread_killed = 1;
    k->t[k->n++] = process_get(p);
}
void thread_kill_others(process_t* cur, int group, int code) {
    thread_group_t* tg = cur->tg;
    for (;;) {
        struct kill_ctx k; k.self = cur; k.n = 0; k.more = 0;
        uint64_t fl = process_lock_irqsave();
        if (group && !tg->group_exit) { tg->group_exit = 1; tg->group_code = code; }
        if (__atomic_load_n(&tg->nr_live, __ATOMIC_RELAXED) > 1)
            process_foreach_locked(kill_cb, &k);
        process_unlock_irqrestore(fl);
        for (int i = 0; i < k.n; i++) {
            signal_post_thread(k.t[i], SIGKILL);
            process_put(k.t[i]);
        }
        if (!k.more) break;
    }
}

int thread_exit_begin(process_t* cur, int code, int group) {
    thread_group_t* tg = cur->tg;
    if (group) thread_kill_others(cur, 1, code);
    thread_clear_child_tid(cur);
    futex_exit(cur);                     // not queued on any futex any more
    int last;
    uint64_t fl = process_lock_irqsave();
    cur->exit_code = code;
    last = (--tg->nr_live == 0);
    // A thread that is not the leader is never waited for: reapable at once.
    // The leader stays an uncollected zombie until the whole group is gone.
    if (!thread_is_leader(cur)) cur->collected = 1;
    process_unlock_irqrestore(fl);
    return last;
}

struct lead_ctx { uint32_t tgid; process_t* lead; };
static void lead_cb(process_t* p, void* u) {
    struct lead_ctx* l = (struct lead_ctx*)u;
    if (!l->lead && p->pid == l->tgid) l->lead = p;
}
void thread_exit_group_info(process_t* cur, uint32_t* tgid, int* code) {
    thread_group_t* tg = cur->tg;
    *tgid = cur->tgid;
    uint64_t fl = process_lock_irqsave();
    struct lead_ctx l = { cur->tgid, NULL };
    process_foreach_locked(lead_cb, &l);
    int c = tg->group_exit ? tg->group_code : (l.lead ? l.lead->exit_code : cur->exit_code);
    if (l.lead) l.lead->exit_code = c;   // what wait() reports
    if (l.lead != cur) cur->exit_code = c;
    process_unlock_irqrestore(fl);
    *code = c;
}
void thread_mark_dead(process_t* cur) {
    uint64_t fl = process_lock_irqsave();
    cur->tg->dead = 1;
    process_unlock_irqrestore(fl);
}

// [U9] execve from a thread (Linux de_thread): the other threads are killed
// (without a group exit) and the caller waits until it is alone, then takes
// over the leader's pid if it was not the leader.
struct swap_ctx { process_t* self; process_t* lead; };
static void swap_cb(process_t* p, void* u) {
    struct swap_ctx* s = (struct swap_ctx*)u;
    if (p != s->self && p->pid == s->self->tgid) s->lead = p;
}
int thread_exec_dethread(process_t* cur) {
    thread_group_t* tg = cur->tg;
    int kill = 0;
    {
        uint64_t fl = process_lock_irqsave();
        if (tg->group_exit || cur->thread_killed || (tg->exec_tid && tg->exec_tid != cur->pid)) {
            process_unlock_irqrestore(fl);
            return -1;                                 // we are being killed
        }
        if (tg->nr_live <= 1 && thread_is_leader(cur)) { tg->exec_tid = 0; process_unlock_irqrestore(fl); return 0; }
        if (!tg->exec_tid) { tg->exec_tid = cur->pid; kill = 1; }
        process_unlock_irqrestore(fl);
    }
    if (kill) thread_kill_others(cur, 0, 0);
    uint64_t fl = process_lock_irqsave();
    if (tg->nr_live > 1) { process_unlock_irqrestore(fl); return 1; }   // not yet alone
    // Alone. A non-leader takes the leader's pid: the old leader (a zombie
    // no CPU runs: its thread exited) gets ours and is reaped as a thread.
    if (!thread_is_leader(cur)) {
        struct swap_ctx s = { cur, NULL };
        process_foreach_locked(swap_cb, &s);
        uint32_t old = cur->pid;
        cur->pid = cur->tgid;
        if (s.lead) {
            s.lead->pid = old;
            s.lead->collected = 1;                     // nobody waits for it now
            cur->ppid = s.lead->ppid; cur->spawner_pid = s.lead->spawner_pid;
            cur->collected = 0;
        }
        debugcon_writestring("[EXEC] thread "); debugcon_print_hex(old);
        debugcon_writestring(" takes over pid "); debugcon_print_hex(cur->pid); debugcon_writestring("\n");
    }
    tg->exec_tid = 0;
    process_unlock_irqrestore(fl);
    return 0;
}
