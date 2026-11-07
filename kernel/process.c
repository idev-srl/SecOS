/*
 * SecOS Kernel - Process Management
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "kverbose.h"
#include "process.h"
#include "elf.h"
#include "heap.h"
#include "terminal.h"
#include "panic.h"
#include "mm/elf_manifest.h"
#include "mm/elf_sign.h"
#include "driver_if.h"
#include "cap.h"
#include "pmm.h"
#include "debugcon.h"
#include "spinlock.h"   // [M29] SMP: serialize the process table
#include "percpu.h"     // [M29] smp_online_count for CPU affinity
#include "user_copy.h"  // EFAULT
#include "pty.h"        // pty fd / controlling-terminal references
#include "input.h"      // [M40] evdev client references
#include "fbdev.h"      // [M40] graphics-owner exit -> text console

#define MAX_PROCESSES 32
static process_t* volatile proc_table[MAX_PROCESSES];
static uint32_t next_pid = 1;
static int proc_inited = 0;

// [M29] One lock guards the process table, next_pid and the home rotor. It
// is taken around the whole process_foreach() iteration (so the table cannot be
// mutated mid-scan); the scheduler's foreach callbacks therefore must NOT call
// any proc_lock-taking function (they don't — only read/write process fields).
// [M48] It also serializes every scheduling claim (process_claim_next) and the
// RUNNING -> BLOCKED step, so state changes made by wake-ups are atomic with
// respect to the CPU that picks a task. Process teardown frees a process_t only
// after detaching it from the table under this lock, and only once no CPU is
// on it (process_reap_one).
static spinlock_t proc_lock = SPINLOCK_INIT_CLASS(LK_S30_PROC, "proc");
extern uint64_t timer_get_ticks(void);
static uint32_t affinity_rotor = 0;

int process_init_system(void) {
    for (int i=0;i<MAX_PROCESSES;i++) proc_table[i]=0;
    proc_inited = 1;
    terminal_writestring("[PROC] process table initialized\n");
    return 0;
}

// [M29] Allocate a unique pid (locked).
static uint32_t alloc_pid(void) {
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    uint32_t pid = next_pid++;
    spin_unlock_irqrestore(&proc_lock, fl);
    return pid;
}

// [M48] Push on create/fork: a new task's home is the online CPU with the
// fewest runnable (NEW/READY/RUNNING) tasks homed on it; ties rotate so a burst
// spreads out. Any idle CPU may still pull it later.
uint32_t process_pick_home(void) {
    uint32_t n = smp_cpu_count(); if (n == 0) n = 1;
    if (n > SMP_MAX_CPUS) n = SMP_MAX_CPUS;
    static uint16_t load[SMP_MAX_CPUS];
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    for (uint32_t c = 0; c < n; c++) load[c] = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* p = proc_table[i];
        if (!p || p->cpu_affinity >= n) continue;
        if (p->state == PROC_NEW || p->state == PROC_READY || p->state == PROC_RUNNING)
            load[p->cpu_affinity]++;
    }
    uint32_t best = 0, best_load = 0xFFFFFFFFu;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t c = (affinity_rotor + k) % n;
        if (!cpu_by_index(c)->online) continue;
        if (load[c] < best_load) { best = c; best_load = load[c]; }
    }
    affinity_rotor++;
    spin_unlock_irqrestore(&proc_lock, fl);
    return best;
}

uint64_t process_lock_irqsave(void) { return spin_lock_irqsave(&proc_lock); }
void process_unlock_irqrestore(uint64_t fl) { spin_unlock_irqrestore(&proc_lock, fl); }

// [M48] Choose and claim the next task for CPU `cpu`. Eligible: READY, not on
// any CPU (on_cpu == 0: whoever ran it last has fully switched off it), and
// allowed by its hard pin. Round-robin in table order starting just after
// `after`. Tasks homed on `cpu` win; otherwise the first eligible task of any
// other CPU is pulled (load balancing: an idle or quantum-expired CPU never
// sits on runnable work). The claim (RUNNING + on_cpu + new home) happens
// under proc_lock, so two CPUs can never claim the same task.
process_t* process_claim_next(uint32_t cpu, process_t* after) {
    // Lock-free peek first: idle CPUs call this on every tick, and with many
    // CPUs taking proc_lock just to find nothing became the bottleneck. A
    // stale read (an entry being freed is still mapped heap memory) can only
    // miss a task that the next tick or a reschedule IPI picks up.
    {
        int any = 0;
        for (int i = 0; i < MAX_PROCESSES && !any; i++) {
            process_t* p = proc_table[i];
            if (p && p != after && p->state == PROC_READY && !p->on_cpu) any = 1;
        }
        if (!any) return NULL;
    }
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    int start = 0;
    if (after) {
        for (int i = 0; i < MAX_PROCESSES; i++) if (proc_table[i] == after) { start = i + 1; break; }
    }
    process_t* home = NULL; process_t* any = NULL;
    uint64_t now = timer_get_ticks();
    uint32_t ncpu = smp_cpu_count();
    for (int k = 0; k < MAX_PROCESSES && !home; k++) {
        process_t* p = proc_table[(start + k) % MAX_PROCESSES];
        if (!p || p == after || p->state != PROC_READY || p->on_cpu) continue;
        if (p->cpu_pin >= 0 && (uint32_t)p->cpu_pin != cpu) continue;
        if (p->cpu_affinity == cpu) { home = p; continue; }
        if (any) continue;
        // Pull another CPU's task only when that CPU cannot run it soon: it is
        // busy with something else, offline, or the task has already waited a
        // couple of ticks. An idle home CPU was kicked and picks it itself
        // (keeps a waking task on its warm CPU instead of ping-ponging).
        if (p->cpu_affinity < ncpu) {
            cpu_t* h = cpu_by_index(p->cpu_affinity);
            if (h->online && h->in_idle && p->ready_tick && now - p->ready_tick < 2) continue;
        }
        any = p;
    }
    process_t* r = home ? home : any;
    if (r) {
        r->state = PROC_RUNNING;
        r->on_cpu = 1;
        if (r->cpu_affinity != cpu) { r->cpu_affinity = cpu; this_cpu()->nr_migrations++; }
    }
    spin_unlock_irqrestore(&proc_lock, fl);
    return r;
}

int process_count_runnable(void) {
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    int n = 0;
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (proc_table[i] && proc_table[i]->state == PROC_READY && !proc_table[i]->on_cpu) n++;
    spin_unlock_irqrestore(&proc_lock, fl);
    return n;
}

// [M48] Zombies detached by process_reap_one() whose process_destroy() has not
// finished yet (it runs outside proc_lock, possibly on another CPU). Counted by
// process_count_all() so "the table is empty" really means every teardown is
// done: the M29-3 leak check sampled free memory while another CPU was still
// freeing a detached zombie's address space and kernel stack.
static volatile int g_reaping;
void process_reap_done(void) { __atomic_sub_fetch(&g_reaping, 1, __ATOMIC_SEQ_CST); }

int process_count_all(void) {
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    int n = g_reaping;
    for (int i = 0; i < MAX_PROCESSES; i++) if (proc_table[i]) n++;
    spin_unlock_irqrestore(&proc_lock, fl);
    return n;
}

int process_set_pin(uint32_t pid, int32_t cpu) {
    if (cpu >= 0 && ((uint32_t)cpu >= smp_cpu_count() || !cpu_by_index((uint32_t)cpu)->online)) return -22;
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    int r = -3;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* p = proc_table[i];
        if (p && p->pid == pid) {
            p->cpu_pin = cpu;
            if (cpu >= 0) p->cpu_affinity = (uint32_t)cpu;
            r = 0; break;
        }
    }
    spin_unlock_irqrestore(&proc_lock, fl);
    return r;
}

// sched_getaffinity: the CPUs `pid` may run on, as a bit mask
// over CPU indexes (bit i of mask[i / 64]): its pinned CPU when it has a hard
// affinity, else every online CPU (the soft home CPU is no restriction).
// `nwords` 64-bit words are written. Returns 0, or -3 when no such pid.
int process_get_affinity(uint32_t pid, uint64_t* mask, uint32_t nwords) {
    for (uint32_t w = 0; w < nwords; w++) mask[w] = 0;
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    process_t* p = NULL;
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (proc_table[i] && proc_table[i]->pid == pid) { p = proc_table[i]; break; }
    int r = -3;
    if (p) {
        uint32_t n = smp_cpu_count();
        if (n > nwords * 64) n = nwords * 64;
        for (uint32_t c = 0; c < n; c++) {
            if (!smp_cpu_online(c)) continue;
            if (p->cpu_pin >= 0 && (uint32_t)p->cpu_pin != c) continue;
            mask[c / 64] |= 1ULL << (c % 64);
        }
        r = 0;
    }
    spin_unlock_irqrestore(&proc_lock, fl);
    return r;
}

// A table index doubles as the process's kernel-stack slot. The reaper detaches
// a zombie from the table (process_reap_one) before process_destroy() unmaps
// its kernel stack, so the index stays reserved here until then: otherwise
// another CPU's proc_add() could take it and map its kstack over the still-
// mapped old one ("[M2][PANIC] vmm_map failed (-3)"), and the late unmap would
// then free the new process's stack frames. Guarded by proc_lock.
static uint8_t kstack_slot_busy[MAX_PROCESSES];

static int proc_add(process_t* p) {
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (!proc_table[i] && !kstack_slot_busy[i]) {
            proc_table[i]=p; p->kstack_slot = (uint8_t)i; kstack_slot_busy[i] = 1;
            spin_unlock_irqrestore(&proc_lock, fl); return 0; }
    }
    spin_unlock_irqrestore(&proc_lock, fl);
    return -1;
}

// Called once p's kernel stack is gone (process_destroy, failed creation): the
// table entry and the kstack slot may be handed out again.
static void proc_remove(process_t* p) {
    if (!p) return;
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    int found = 0;
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (proc_table[i] == p) { proc_table[i] = NULL; found = 1; break; }
    }
    // A reaped zombie was detached by process_reap_one; anything else that was
    // never added owns no slot.
    if ((found || p->state == PROC_ZOMBIE) && p->kstack_slot < MAX_PROCESSES &&
        !proc_table[p->kstack_slot])
        kstack_slot_busy[p->kstack_slot] = 0;
    spin_unlock_irqrestore(&proc_lock, fl);
}

// [M50-B1] References. Taken under proc_lock, so a lookup can never race the
// reaper's detach (process_reap_one checks refs under the same lock); dropped
// with a plain atomic decrement: the reaper reads 0 only after the last user
// is done with the process. A zombie skipped because of a reference is reaped
// by the next sched_reap_zombies() (every fork/spawn, every idle loop).
static inline void proc_ref_locked(process_t* p) {
    __atomic_add_fetch(&p->refs, 1, __ATOMIC_RELAXED);
}

process_t* process_get(process_t* p) {
    if (p) proc_ref_locked(p);
    return p;
}

void process_put(process_t* p) {
    if (!p) return;
    uint32_t r = __atomic_sub_fetch(&p->refs, 1, __ATOMIC_RELEASE);
    if (r == 0xFFFFFFFFu) {
        __atomic_add_fetch(&p->refs, 1, __ATOMIC_RELAXED);
        debugcon_writestring("[PROC] BUG: process_put without a reference\n");
    }
}

process_t* process_get_last(void) {
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    process_t* best = NULL; uint32_t best_pid = 0;
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (proc_table[i] && proc_table[i]->pid > best_pid) { best = proc_table[i]; best_pid = proc_table[i]->pid; }
    }
    if (best) proc_ref_locked(best);
    spin_unlock_irqrestore(&proc_lock, fl);
    return best;
}

process_t* process_get_by_pid(uint32_t pid) {
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    process_t* r = NULL;
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (proc_table[i] && proc_table[i]->pid == pid) { r = proc_table[i]; break; }
    }
    if (r) proc_ref_locked(r);
    spin_unlock_irqrestore(&proc_lock, fl);
    return r;
}

int process_pid_state(uint32_t pid) {
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    int st = -1;
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (proc_table[i] && proc_table[i]->pid == pid) { st = (int)proc_table[i]->state; break; }
    }
    spin_unlock_irqrestore(&proc_lock, fl);
    return st;
}

process_t* process_detach_unstarted(uint32_t pid) {
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    process_t* r = NULL;
    for (int i=0;i<MAX_PROCESSES;i++) {
        process_t* p = proc_table[i];
        if (!p || p->pid != pid) continue;
        // Never ran (no CPU has its stack or CR3) and nobody holds a pointer.
        // Detached like a reaped zombie (proc_remove frees the kstack slot,
        // the caller ends with process_reap_done()).
        if (p->state == PROC_NEW && !p->on_cpu && p->refs == 0) {
            p->state = PROC_ZOMBIE;
            proc_table[i] = NULL;
            __atomic_add_fetch(&g_reaping, 1, __ATOMIC_SEQ_CST);
            r = p;
        }
        break;
    }
    spin_unlock_irqrestore(&proc_lock, fl);
    return r;
}

void process_set_cred(process_t* p, const struct proc_cred* cr) {
    if (!p || !cr) return;
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    p->cred = *cr;
    spin_unlock_irqrestore(&proc_lock, fl);
}

void process_foreach(void (*cb)(process_t*, void*), void* user) {
    if (!cb) return;
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (proc_table[i]) cb(proc_table[i], user);
    }
    spin_unlock_irqrestore(&proc_lock, fl);
}

// [M39] wait-any support (SYS_WAITANY, bash's waitpid(-1) pattern): collect an
// uncollected ZOMBIE child of `ppid`. [M50-B1] Collected under proc_lock and
// only its pid/status leave the lock: once collected, the reaper may free it.
int process_collect_zombie_child(uint32_t ppid, uint32_t* pid, int* code) {
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    int found = 0;
    for (int i=0;i<MAX_PROCESSES;i++) {
        process_t* p = proc_table[i];
        if (p && p->ppid == ppid && p->state == PROC_ZOMBIE && !p->collected) {
            *pid = p->pid; *code = p->exit_code;
            p->collected = 1;
            found = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&proc_lock, fl);
    return found;
}
// Job control: find a child of `ppid` with an unreported stop (WUNTRACED) or
// continue (WCONTINUED), consume the report, and return its pid (0 = none);
// *status gets the POSIX encoding: stopped = (sig << 8) | 0x7f, continued =
// 0xffff.
uint32_t process_take_jobctl_child(uint32_t ppid, int want_stop, int want_cont, int* status) {
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    uint32_t r = 0;
    for (int i=0;i<MAX_PROCESSES && !r;i++) {
        process_t* p = proc_table[i];
        if (!p || p->ppid != ppid || p->state == PROC_ZOMBIE) continue;
        if (want_stop && p->stop_report && p->state == PROC_STOPPED) {
            p->stop_report = 0;
            *status = ((p->stop_sig & 0xff) << 8) | 0x7f;
            r = p->pid;
        } else if (want_cont && p->cont_report) {
            p->cont_report = 0;
            *status = 0xffff;
            r = p->pid;
        }
    }
    spin_unlock_irqrestore(&proc_lock, fl);
    return r;
}
// PID 1 (/bin/init, started by the kernel at boot). 0 = no init running.
static uint32_t g_init_pid = 0;
void     process_set_init_pid(uint32_t pid) { g_init_pid = pid; }
uint32_t process_init_pid(void) { return g_init_pid; }

// A process `dead` is exiting: hand its children to init (POSIX re-parenting),
// so init's waitpid(-1) collects their status and no zombie is left without a
// parent. With no init running the children keep their stale ppid and the
// reaper frees them as orphans (the pre-init behaviour). Returns 1 when a
// re-parented child is already a zombie (the caller wakes init's wait).
int process_reparent_children(uint32_t dead) {
    uint32_t init = g_init_pid;
    if (!init || dead == init || dead == 0) return 0;
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    int alive = 0, zombie = 0;
    for (int i=0;i<MAX_PROCESSES;i++) {
        process_t* q = proc_table[i];
        if (q && q->pid == init && q->state != PROC_ZOMBIE) { alive = 1; break; }
    }
    for (int i=0;i<MAX_PROCESSES && alive;i++) {
        process_t* p = proc_table[i];
        if (!p || p->ppid != dead || p->pid == dead) continue;
        p->ppid = init;
        if (p->state == PROC_ZOMBIE && !p->collected) zombie = 1;
    }
    spin_unlock_irqrestore(&proc_lock, fl);
    return zombie;
}

int process_has_children(uint32_t ppid) {
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    int n = 0;
    for (int i=0;i<MAX_PROCESSES;i++) {
        process_t* p = proc_table[i];
        // A zombie whose status was already collected is no longer a child the
        // caller can wait for (it only awaits the reaper). Counting it made
        // waitpid(-1) block forever instead of returning ECHILD after the last
        // child was collected (bash `cmd & wait`).
        if (p && p->ppid == ppid && !(p->state == PROC_ZOMBIE && p->collected)) { n++; break; }
    }
    spin_unlock_irqrestore(&proc_lock, fl);
    return n;
}

// [M29/M48] Atomically detach one reapable ZOMBIE from the table and return it
// (the caller frees it via process_destroy outside the lock).
process_t* process_reap_one(void) {
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    process_t* z = NULL;
    for (int i=0;i<MAX_PROCESSES;i++) {
        process_t* p = proc_table[i];
        // [M48] Any CPU may reap a zombie once its last CPU has switched off it
        // (on_cpu == 0): its kernel stack, CR3 and FPU image are no longer in use.
        if (!p || p->state != PROC_ZOMBIE || p->on_cpu) continue;
        // [M50-B1] Someone still holds a reference (procfs, kill, a waiter):
        // keep it; a later reap pass frees it after the last process_put().
        if (__atomic_load_n(&p->refs, __ATOMIC_ACQUIRE)) continue;
        // [M39] Unix zombie semantics: keep a zombie until its parent collects the
        // status via wait() — otherwise a parent that forks and continues (bash
        // subshells/pipelines) races this reaper and its SYS_WAIT(pid) finds the
        // child already gone (-1), so it loses the exit status. Reapable when the
        // parent collected it, there's no tracking parent (ppid==0, the M10 shell
        // poll path), or the parent is gone (orphan).
        int reapable = p->collected || p->ppid == 0;
        if (!reapable) {
            int parent_alive = 0;
            for (int j=0;j<MAX_PROCESSES;j++) {
                process_t* q = proc_table[j];
                if (q && q->pid == p->ppid && q->state != PROC_ZOMBIE) { parent_alive = 1; break; }
            }
            if (!parent_alive) reapable = 1;   // orphan -> safe to reap
        }
        if (reapable) { proc_table[i] = NULL; z = p; __atomic_add_fetch(&g_reaping, 1, __ATOMIC_SEQ_CST); break; }
    }
    spin_unlock_irqrestore(&proc_lock, fl);
    return z;
}

// [M39] User-space ASLR: per-process random page-aligned slide for the stack,
// heap and mmap arena. Entropy from the TSC + xorshift mix (good enough for
// layout randomization, not crypto). Gate off with -DSECOS_NO_ASLR (slide 0 =
// the old fixed layout). ELF base ASLR would need PIE user binaries (deferred).
static uint64_t aslr_slide(uint64_t span_bytes) {
#ifdef SECOS_NO_ASLR
    (void)span_bytes; return 0;
#else
    if (span_bytes < 0x2000) return 0;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t x = ((uint64_t)hi << 32) | lo;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;   // xorshift64 so close TSC values diverge
    uint64_t pages = span_bytes >> 12;
    return (x % pages) << 12;                    // page-aligned offset in [0, span)
#endif
}

// [M16] Write 'n' bytes into user VA 'va' of a not-yet-running space, faulting
// in the backing (demand-paged) stack pages as needed. Copies a page at a time
// through the physmap. Returns 0 / -1.
static int uwrite(vmm_space_t* space, const vma_set_t* vmas, uint64_t va,
                  const void* src, uint64_t n) {
    const uint8_t* s = (const uint8_t*)src;
    while (n) {
        uint64_t phys = vmm_translate_in_space(space, va);
        if (!phys) {
            const vma_t* vv = vma_find(vmas, va);
            if (!vv) return -1;
            if (vma_fault_in(space, vv, va) != 0) return -1;
            phys = vmm_translate_in_space(space, va);
            if (!phys) return -1;
        }
        uint64_t chunk = 0x1000ULL - (va & 0xFFFULL);
        if (chunk > n) chunk = n;
        uint8_t* d = (uint8_t*)phys_to_virt(phys);
        for (uint64_t i = 0; i < chunk; i++) d[i] = s[i];
        va += chunk; s += chunk; n -= chunk;
    }
    return 0;
}

#define PROC_MAX_ARGS    16
#define PROC_MAX_ARG_LEN 256
#define PROC_STACK_PAGES 8

// [M16] Build the System V initial stack (top-down) for a new image:
//   strings: argv then envp, packed exactly as 'blob'          <- st_top
//   padding to 16
//   NULL, envp[envc-1..0], NULL, argv[argc-1..0], argc         <- rsp
// 'blob' holds argc+envc NUL-terminated strings back to back (blen bytes). rsp
// is 16-byte aligned; crt0 hands rdi/rsi/rdx = argc/argv/envp to main. At least
// one page of the stack VMA stays free for the program itself. 0 / -1.
static int setup_user_args(vmm_space_t* space, const vma_set_t* vmas,
                           uint64_t st_top, uint64_t st_lo,
                           const char* blob, uint64_t blen, int argc, int envc,
                           uint64_t* rsp_out, uint64_t* argv_out, uint64_t* envp_out) {
    if (argc < 0 || envc < 0) return -1;
    if (blen > st_top - st_lo) return -1;
    uint64_t strbase = st_top - blen;
    uint64_t nslots = 1ULL + (uint64_t)argc + 1ULL + (uint64_t)envc + 1ULL;
    if (nslots * 8ULL > (strbase & ~15ULL) - st_lo) return -1;
    uint64_t base = ((strbase & ~15ULL) - nslots * 8ULL) & ~15ULL;
    if (base < st_lo + 0x1000ULL) return -1;
    if (blen && uwrite(space, vmas, strbase, blob, blen) != 0) return -1;
    uint64_t slot = base, z = 0, v = (uint64_t)argc, off = 0;
    if (uwrite(space, vmas, slot, &v, 8) != 0) return -1;
    slot += 8;
    *argv_out = slot;
    for (int pass = 0; pass < 2; pass++) {
        int cnt = pass ? envc : argc;
        if (pass) *envp_out = slot;              // envp[] follows argv's NULL
        for (int i = 0; i < cnt; i++) {
            if (off >= blen) return -1;          // blob shorter than the counts
            uint64_t ptr = strbase + off;
            if (uwrite(space, vmas, slot, &ptr, 8) != 0) return -1;
            slot += 8;
            while (off < blen && blob[off]) off++;
            off++;                               // past the NUL
        }
        if (uwrite(space, vmas, slot, &z, 8) != 0) return -1;
        slot += 8;
    }
    *rsp_out = base;
    return 0;
}

// A loaded, validated, not-yet-running user image: its own address space with
// the ELF segments + stack registered as demand-paged VMAs, the pinned ELF copy
// backing the FILE VMAs, and the validated signed manifest (or NULL).
typedef struct proc_image {
    vmm_space_t*    space;
    uint8_t*        image;
    size_t          image_size;
    uint64_t        entry;
    uint64_t        stack_top;
    uint64_t        stack_lo;
    uint64_t        footprint;      // reserved bytes (sum of VMA sizes)
    elf_manifest_t* mf;
} proc_image_t;

static void proc_image_free(proc_image_t* im) {
    if (im->mf) kfree(im->mf);
    if (im->image) kfree(im->image);
    if (im->space) vmm_space_destroy(im->space);   // frees any faulted frames too
    im->mf = NULL; im->image = NULL; im->space = NULL;
}

// Signature gate + lazy ELF load + stack VMA + manifest (shared by process
// creation and execve). If 'owned' is non-NULL it is the kmalloc'd buffer that
// holds elf_buf and becomes the pinned image (freed on failure); otherwise the
// image is copied. 'extra_stack' bytes are added to the stack VMA (for exec's
// argv/envp). On failure everything is freed and -errno returned.
static int proc_image_build(const void* elf_buf, size_t size, uint8_t* owned,
                            uint64_t extra_stack, vma_set_t* vmas, proc_image_t* im) {
    im->space = NULL; im->image = owned; im->image_size = size; im->mf = NULL;
    im->entry = 0; im->footprint = 0; im->stack_top = 0; im->stack_lo = 0;
    vmas->count = 0;

    // [M9] Code-signing gate (root of trust). Every ELF must carry a valid
    // Ed25519 signature (docs/SIGNING.md) to run. -DDEV_ALLOW_UNSIGNED downgrades
    // refusal to a warning for bootstrap (e.g. the M7/M8 synthetic demos).
    {
        int sv = elf_signature_verify(elf_buf, size);
#ifdef DEV_ALLOW_UNSIGNED
        if (sv != ELF_SIG_OK)
            debugcon_writestring("[SEC] WARN: unsigned/invalid ELF allowed (DEV_ALLOW_UNSIGNED)\n");
#else
        if (sv != ELF_SIG_OK) {
            terminal_writestring("[SEC] REFUSED: ELF signature missing/invalid\n");
            debugcon_writestring("[SEC] REFUSED unsigned/invalid ELF\n");
            proc_image_free(im);
            return -EACCES;
        }
        debugcon_writestring("[SEC] ELF signature OK\n");
#endif
    }

    im->space = vmm_space_create_user();
    if (!im->space) {
        terminal_writestring("[PROC] space alloc failed\n");
        proc_image_free(im);
        return -ENOMEM;
    }
    // [M14] Pin a private copy of the (already signature-verified) ELF image for
    // the process lifetime: FILE-backed VMAs fill pages from it on demand.
    if (!im->image) {
        im->image = (uint8_t*)kmalloc(size);
        if (!im->image) {
            terminal_writestring("[PROC] image pin alloc failed\n");
            proc_image_free(im);
            return -ENOMEM;
        }
        const uint8_t* src = (const uint8_t*)elf_buf;
        for (size_t i = 0; i < size; i++) im->image[i] = src[i];
    }

    int r = elf_load_image_lazy(im->image, size, im->space, &im->entry, vmas, &im->footprint);
    if (r != ELF_OK) {
        terminal_writestring("[PROC] elf lazy load fail\n");
        proc_image_free(im);
        return -ENOEXEC;
    }
    // [M14] Reserve the user stack as a demand-zero (ANON) VMA below the
    // (ASLR-randomized, [M39] <=16MB slide) stack top. Pages fault in on first
    // push; the absence of a VMA below the region is the guard.
    uint64_t st_bytes = (uint64_t)PROC_STACK_PAGES * 0x1000ULL
                      + ((extra_stack + 0xFFFULL) & ~0xFFFULL);
    im->stack_top = USER_STACK_TOP - aslr_slide(16ULL * 1024 * 1024);
    im->stack_lo  = im->stack_top - st_bytes;
    if (vma_add(vmas, im->stack_lo, im->stack_top,
                VMM_FLAG_USER | VMM_FLAG_RW | VMM_FLAG_NOEXEC,
                VMA_TYPE_ANON, NULL, 0, 0, 0) != 0) {
        terminal_writestring("[PROC] stack VMA add fail\n");
        proc_image_free(im);
        return -ENOMEM;
    }
    im->footprint += st_bytes;

    // Signed manifest (the signature covers it, so everything derived from it —
    // memory limit, capabilities, driver claim — is trust-rooted).
    elf_manifest_t* mf = (elf_manifest_t*)kmalloc(sizeof(elf_manifest_t));
    if (mf && elf_manifest_parse(im->image, size, mf) == 0) {
        if (elf_manifest_validate(mf, im->entry) == MANIFEST_OK) {
            // [M13/M14] max_mem is checked against the RESERVED footprint (sum
            // of VMA sizes), not lazily-mapped pages.
            if (mf->max_mem && im->footprint > mf->max_mem) {
                terminal_writestring("[MANIFEST] max_mem superato, abort processo\n");
                debugcon_writestring("[M13] max_mem exceeded -> process REFUSED (used=");
                debugcon_print_hex(im->footprint);
                debugcon_writestring(" limit=");
                debugcon_print_hex(mf->max_mem);
                debugcon_writestring(")\n");
                kfree(mf);
                proc_image_free(im);
                return -ENOMEM;
            }
            im->mf = mf;
        } else {
            terminal_writestring("[MANIFEST] validation fail, scarto manifest\n");
            kfree(mf);
        }
    } else if (mf) {
        kfree(mf);
    }
    return 0;
}

// [M18/M24/M35] Limits + capabilities from the signed manifest (NULL = ambient,
// unlimited, no CAP_NET).
static void proc_apply_caps(process_t* p, const elf_manifest_t* mf) {
    p->cap_net = 0; p->cap_enforce = 0; p->cap_mask = 0; p->mem_limit = 0;
    if (!mf) return;
    p->mem_limit   = mf->max_mem;                  // [M18] runtime brk/mmap bound
    p->cap_net     = (mf->flags & MANIFEST_FLAG_CAP_NET) ? 1 : 0;
    p->cap_enforce = (mf->flags & MANIFEST_FLAG_CAP_ENFORCE) ? 1 : 0;
    p->cap_mask    = mf->flags & MANIFEST_CAP_MASK;
    if (p->cap_enforce) {
        debugcon_writestring("[M35] confined process pid loaded, caps=");
        debugcon_print_hex(p->cap_mask);
        debugcon_writestring("\n");
    }
}

// Record the randomized layout (debugcon only) so the selftest can assert that
// per-process slides are actually applied and differ between processes.
static void aslr_log(uint64_t st_top, uint64_t brk, uint64_t mm) {
    debugcon_writestring("[ASLR] stack_top="); debugcon_print_hex(st_top);
    debugcon_writestring(" brk="); debugcon_print_hex(brk);
    debugcon_writestring(" mmap="); debugcon_print_hex(mm);
    debugcon_writestring("\n");
}

// [M43] Does p's signed manifest allow set-id execution (MANIFEST_FLAG_SETID)?
int process_manifest_setid(const process_t* p) {
    const elf_manifest_t* mf = p ? (const elf_manifest_t*)p->manifest : NULL;
    return mf && (mf->flags & MANIFEST_FLAG_SETID) ? 1 : 0;
}

process_t* process_create_from_elf(const void* elf_buf, size_t size) {
    return process_create_from_elf_args(elf_buf, size, 0, NULL);
}

process_t* process_create_from_elf_args(const void* elf_buf, size_t size,
                                        int argc, const char* const argv[]) {
    if (!proc_inited) process_init_system();

    process_t* p = (process_t*)kmalloc(sizeof(process_t));
    if (!p) { terminal_writestring("[PROC] pcb alloc failed\n"); return NULL; }
    klock_task_reset(&p->klk);               // [M50] holds no klock
    p->refs = 0;                             // [M50-B1] lifetime + address-space lock
    spin_init_class_dynamic(&p->mm_lock, LK_S35_MM, "mm");
    p->mapped_pages = NULL;
    p->mapped_page_count = 0;

    proc_image_t im;
    if (proc_image_build(elf_buf, size, NULL, 0, &p->vmas, &im) != 0) {
        kfree(p);
        return NULL;
    }
    vmm_space_t* space = im.space;
    uint64_t entry = im.entry;
    uint64_t st_top = im.stack_top;
    p->image = im.image;
    p->image_size = im.image_size;

    p->pid = alloc_pid();
    p->cpu_affinity = process_pick_home();   // [M48] home CPU (soft; may migrate)
    p->cpu_pin = -1; p->on_cpu = 0; p->wake_snap = 0; p->ready_tick = 0; p->smp_logged = 0;
    p->space = space;
    p->entry = entry;
    p->stack_top = st_top;
    p->kstack_top = 0;
    p->kstack_slot = 0;
    p->tf = NULL;
    // [M8] Non-runnable while under construction: with preemptive scheduling
    // active, the scheduler must not pick this process (tf still NULL) until it
    // is fully built. Promoted to PROC_NEW at the end.
    p->state = PROC_BLOCKED;
    p->manifest = NULL;
    // [M11] Default privilege: USER, no device binding. Upgraded below only if
    // the (already signature-verified) manifest declares PROC_TYPE_DRIVER.
    p->proc_type = PROC_TYPE_USER;
    p->drv_dev_id = -1;
    p->drv_caps = 0;
    p->cpu_ticks = 0;
    p->exit_code = 0;
    p->collected = 0;
    p->wait_pid = -1; p->wait_result = 0; p->wait_ready = 0;
    p->sleep_until = 0; p->recv_chan = -1;
    // [M30] Signals: default dispositions, empty masks, own process group, no
    // parent process (a shell-spawned program is reaped via the poll path; the
    // shell sets pgid for pipelines after creation). pid is assigned above.
    { extern void signal_init_proc(process_t*); signal_init_proc(p); }
    p->pgid = p->pid; p->ppid = 0; p->spawner_pid = 0;
    p->stop_report = 0; p->cont_report = 0; p->stop_sig = 0;
    p->wait_tty = 0; p->power_phase = 0; p->power_deadline = 0;
    p->wait_pty = NULL; p->sid = p->pid; p->ctty = NULL; p->ctty_detached = 0;  // own session, console
    p->sigsusp_active = 0; p->sigsusp_saved = 0; p->sig_eintr_rip = 0; p->sig_rerun_eintr = 0; p->fpu_valid = 0;
    p->cwd[0] = '/'; p->cwd[1] = 0;   // [M39] cwd defaults to root (inherited via fork copy)
    cred_init_root(&p->cred);         // [M43] kernel-made: root (SYS_SPAWN copies the spawner's)
    // [M39] User ASLR: random page-aligned slide of the heap and mmap arena bases
    // (<=32MB each, within their multi-GB regions). Transparent to programs.
    p->brk_start = USER_HEAP_BASE + aslr_slide(32ULL*1024*1024);
    p->brk_cur = p->brk_start;
    p->mmap_next = USER_MMAP_BASE + aslr_slide(32ULL*1024*1024);
    aslr_log(st_top, p->brk_start, p->mmap_next);
    // [M14] mapped_page_count/user_mem_bytes report the RESERVED footprint.
    p->user_mem_bytes = im.footprint;
    p->mapped_page_count = (uint32_t)(im.footprint / 4096ULL);
    elf_manifest_t* mf = im.mf;
    p->manifest = mf;
    proc_apply_caps(p, mf);
    // [M46] Kernel-made: no bound; a confined image gets no_new_privs. SYS_SPAWN
    // re-applies the rule with the spawner's bound (ksys_spawn_argv).
    p->no_new_privs = 0;
    cap_exec_transition(p, cap_exec_bound(NULL), "load");
    // [M11] Driver Space: the signed manifest is the trust root for the
    // driver claim. If it declares PROC_TYPE_DRIVER, validate the device
    // and grant only capabilities the device actually supports (subset),
    // then bind the process. A bogus claim degrades to USER (the process
    // still runs, but SYS_DRIVER will refuse it).
    if (mf && mf->proc_type == MANIFEST_PROC_TYPE_DRIVER) {
        const device_desc_t* dev = driver_get_device((int)mf->dev_id);
        if (!dev) {
            debugcon_writestring("[M11] driver claim refused: unknown device\n");
        } else {
            uint32_t granted = mf->dev_caps & dev->caps_mask;
            if (granted != mf->dev_caps)
                debugcon_writestring("[M11] WARN: requested caps reduced to device-supported subset\n");
            if (driver_register_binding_caps(p, (int)mf->dev_id, granted) == DRV_OK) {
                p->proc_type = PROC_TYPE_DRIVER;
                p->drv_dev_id = (int)mf->dev_id;
                p->drv_caps = granted;
                debugcon_writestring("[M11] driver bound dev=");
                debugcon_print_hex((uint64_t)mf->dev_id);
                debugcon_writestring(" caps=");
                debugcon_print_hex((uint64_t)granted);
                debugcon_writestring("\n");
            } else {
                debugcon_writestring("[M11] driver bind failed (no slot)\n");
            }
        }
    }
    p->regs.rip = entry;
    p->regs.rsp = st_top;
    p->regs.rflags = 0x202; // IF enabled by default
    p->regs.rax = p->regs.rbx = p->regs.rcx = p->regs.rdx = 0;
    p->regs.rsi = p->regs.rdi = p->regs.rbp = 0;
    // Init fd table
    for(int i=0;i<32;i++){ p->fds[i].inode=NULL; p->fds[i].offset=0; p->fds[i].flags=0; p->fds[i].used=0; p->fds[i].is_pipe=0; p->fds[i].pipe_w=0; p->fds[i].cloexec=0; p->fds[i].is_sock=0; }
    // [M25] Reserve fds 0/1/2 as stdin/stdout/stderr so open()/pipe() allocate
    // from fd 3 up (otherwise a pipe end would land on fd 0/1/2 and collide with
    // the console special-cases in ksys_read/ksys_write). They carry no inode —
    // the std-stream paths are keyed on the fd number.
    p->fds[0].used=1; p->fds[1].used=1; p->fds[2].used=1;
    p->wait_pipe=NULL; p->pipe_wdone=0; // [M25]
    { extern void kiface_proc_init(process_t*, int, const char* const*);
      kiface_proc_init(p, argc, argv); }   // procfs identity, umask, CPU accounting
    if (proc_add(p)!=0) {
        terminal_writestring("[PROC] table full\n");
        goto fail_unbound;
    }
    // [M5] Allocate per-process guarded kernel stack using bounded slot
    p->kstack_top = vmm_alloc_kernel_stack_for_slot(p->kstack_slot);
    if (!p->kstack_top) {
        terminal_writestring("[PROC] kstack alloc failed\n");
        proc_remove(p);
        goto fail_unbound;
    }
    // [M6] Allocate and initialize trapframe for initial iretq entry
    p->tf = (trapframe_t*)kmalloc(sizeof(trapframe_t));
    if (!p->tf) {
        terminal_writestring("[PROC] trapframe alloc failed\n");
        vmm_free_kernel_stack_for_slot(p->kstack_slot); p->kstack_top = 0;
        proc_remove(p);
        goto fail_unbound;
    }
    {
        // Zero all GPRs
        uint8_t* z = (uint8_t*)p->tf;
        for (int i = 0; i < (int)sizeof(trapframe_t); i++) z[i] = 0;
        // CPU iret frame
        p->tf->rip    = entry;
        p->tf->rflags = 0x202;       // IF enabled
        p->tf->rsp    = st_top;
        // [M6] User selectors for ring3 execution
        p->tf->cs     = 0x1B;  // User code: GDT entry 3, RPL=3
        p->tf->ss     = 0x23;  // User data: GDT entry 4, RPL=3
        // Markers
        p->tf->int_no   = 0x80;
        p->tf->err_code = 0;
    }
    // [M16] Set up argc/argv/envp on the user stack and pass them to the entry
    // per the System V convention (rdi=argc, rsi=argv, rdx=envp). main(void)
    // programs simply ignore the extra registers. On failure (too many/large
    // args) fall back to argc=0 rather than refusing to run.
    if (argc > 0) {
        if (argc > PROC_MAX_ARGS) argc = PROC_MAX_ARGS;
        char* blob = (char*)kmalloc((size_t)PROC_MAX_ARGS * PROC_MAX_ARG_LEN);
        uint64_t blen = 0, rsp = 0, argvp = 0, envp = 0;
        if (blob) {
            for (int i = 0; i < argc; i++) {
                const char* a = (argv && argv[i]) ? argv[i] : "";
                for (uint64_t k = 0; a[k] && k < PROC_MAX_ARG_LEN - 1; k++) blob[blen++] = a[k];
                blob[blen++] = 0;
            }
        }
        // [M50-B1] p is already in the table (procfs can see it): fault the
        // stack pages in under its mm_lock (anonymous pages, no page cache).
        int sr = -1;
        if (blob) {
            uint64_t mfl = mm_lock(p);
            sr = setup_user_args(space, &p->vmas, st_top, im.stack_lo, blob, blen,
                                 argc, 0, &rsp, &argvp, &envp);
            mm_unlock(p, mfl);
        }
        if (sr == 0) {
            p->tf->rsp = rsp;
            p->tf->rdi = (uint64_t)argc;
            p->tf->rsi = argvp;
            p->tf->rdx = envp;
        } else {
            terminal_writestring("[PROC] argv setup failed; running with argc=0\n");
        }
        if (blob) kfree(blob);
    }
    // Harden the shared kernel-half mappings
    vmm_harden_user_space(space);
    // [M8] Fully built — now safe for the scheduler to pick.
    p->state = PROC_NEW;
    if (g_kverbose) {
        terminal_writestring("[PROC] creato PID=");
        char hx[]="0123456789ABCDEF"; for(int i=28;i>=0;i-=4) terminal_putchar(hx[(p->pid>>i)&0xF]);
        terminal_writestring(" entry="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(entry>>i)&0xF]);
        terminal_writestring(" stack_top="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(st_top>>i)&0xF]);
        terminal_writestring(" pages="); for(int i=28;i>=0;i-=4) terminal_putchar(hx[(p->mapped_page_count>>i)&0xF]); terminal_writestring("\n");
    }
    return p;

fail_unbound:
    driver_remove_all_bindings(p);   // never leave a binding to a freed PCB
    if (p->manifest) kfree(p->manifest);
    if (p->image) kfree(p->image);
    vmm_space_destroy(space);
    kfree(p);
    return NULL;
}

// Reset the x87/SSE register file to its power-on state (FCW=0x37F, MXCSR=
// 0x1F80, all ST/XMM registers zero) so a new image inherits no register
// contents from the old one: fxrstor of a clean, 16-byte aligned save area.
static const uint8_t g_fpu_clean[512] __attribute__((aligned(16))) = {
    [0] = 0x7F, [1] = 0x03,                        // FCW = 0x037F
    [24] = 0x80, [25] = 0x1F,                      // MXCSR = 0x1F80
};
static void proc_fpu_reset(void) {
    __asm__ volatile("fxrstor64 %0" : : "m"(g_fpu_clean));
}

int process_exec(process_t* p, trapframe_t* tf, uint8_t* image, size_t size,
                 const char* blob, uint64_t blen, int argc, int envc) {
    if (!p || !tf || !image) { if (image) kfree(image); return -EFAULT; }
    // A script (or anything that is not an ELF) is ENOEXEC — that is what lets a
    // shell fall back to interpreting it. Only ELFs reach the signature gate.
    if (size < 4 || image[0] != 0x7F || image[1] != 'E' || image[2] != 'L' || image[3] != 'F') {
        kfree(image);
        return -ENOEXEC;
    }
    vma_set_t* nv = (vma_set_t*)kmalloc(sizeof(vma_set_t));
    if (!nv) { kfree(image); return -ENOMEM; }

    // ---- 1. Build + validate the new image. p is not touched. ----
    uint64_t argbytes = blen + ((uint64_t)argc + (uint64_t)envc + 3ULL) * 8ULL + 32ULL;
    proc_image_t im;
    int r = proc_image_build(image, size, image, argbytes, nv, &im);
    if (r != 0) { kfree(nv); return r; }
    // Driver Space privilege is granted only when a process is created from a
    // signed driver manifest (spawn / kernel), never through exec: neither a
    // plain user process nor a driver can acquire a device binding by exec'ing.
    if (im.mf && im.mf->proc_type == MANIFEST_PROC_TYPE_DRIVER) {
        debugcon_writestring("[EXEC] REFUSED: driver manifest (exec never grants driver privilege)\n");
        proc_image_free(&im); kfree(nv);
        return -EACCES;
    }
    uint64_t rsp = 0, argvp = 0, envp = 0;
    if (setup_user_args(im.space, nv, im.stack_top, im.stack_lo, blob, blen,
                        argc, envc, &rsp, &argvp, &envp) != 0) {
        debugcon_writestring("[EXEC] argv/envp do not fit the new stack\n");
        proc_image_free(&im); kfree(nv);
        return -E2BIG;
    }
    vmm_harden_user_space(im.space);
    uint64_t brk = USER_HEAP_BASE + aslr_slide(32ULL*1024*1024);
    uint64_t mm  = USER_MMAP_BASE + aslr_slide(32ULL*1024*1024);

    // ---- 2. Point of no return: swap in the new image. ----
    // Driver bindings have their own lock (driver_if_lock, S15 < S35): drop
    // them before taking mm_lock.
    driver_remove_all_bindings(p);
    // [M50-B1] The space / VMA / image swap is one mm_lock section (it also
    // keeps interrupts off across the CR3 switch, like the old cli): a
    // cross-process reader sees either the old image or the new one.
    uint64_t fl = mm_lock(p);
    vmm_space_t* old_space = p->space;
    uint8_t*     old_image = p->image;
    void*        old_mf    = p->manifest;
    // The driver type of the old image goes with it.
    p->proc_type = PROC_TYPE_USER; p->drv_dev_id = -1; p->drv_caps = 0;
    // Load the new CR3 before the old tables are freed. p is pinned to this CPU
    // (cpu_affinity), so no other core has the old space loaded or cached; the
    // CR3 write flushes this core's non-global (user) TLB entries.
    p->space = im.space;
    vmm_switch_space(im.space);
    p->vmas = *nv;
    p->image = im.image; p->image_size = im.image_size;
    p->manifest = im.mf;
    {   // [M46] Exec confinement: the new image's caps are bounded by the old's.
        cap_exec_bound_t bound = cap_exec_bound(p);
        proc_apply_caps(p, im.mf);
        cap_exec_transition(p, bound, "execve");
    }
    p->entry = im.entry;
    p->stack_top = im.stack_top;
    p->brk_start = brk; p->brk_cur = brk; p->mmap_next = mm;
    p->user_mem_bytes = im.footprint;
    p->mapped_page_count = (uint32_t)(im.footprint / 4096ULL);
    p->regs.rip = im.entry; p->regs.rsp = rsp;
    mm_unlock(p, fl);
    kfree(nv);
    // Tear down the old user image: every present leaf frame is unref'd (pages
    // still COW-shared with a fork relative stay with it), then the tables.
    if (old_mf) kfree(old_mf);
    if (old_image) kfree(old_image);
    if (old_space) vmm_space_destroy(old_space);

    // Close-on-exec descriptors.
    for (int i = 0; i < 32; i++) {
        if (!p->fds[i].used || !p->fds[i].cloexec) continue;
        if (p->fds[i].is_pipe) {
            extern void pipe_unref(void*, int);
            pipe_unref(p->fds[i].inode, p->fds[i].pipe_w);
        } else if (p->fds[i].is_sock) {
            extern void socket_unref(int);
            socket_unref(p->fds[i].is_sock - 1);
        } else {
            pty_unref(p->fds[i].inode);            // a pty end: drop this holder
            input_obj_unref(p->fds[i].inode);      // [M40] an evdev client
        }
        p->fds[i].used = 0; p->fds[i].inode = NULL; p->fds[i].is_pipe = 0;
        p->fds[i].pipe_w = 0; p->fds[i].cloexec = 0; p->fds[i].is_sock = 0; p->fds[i].offset = 0; p->fds[i].flags = 0;
    }
    // Signals (POSIX): caught signals revert to SIG_DFL (their handlers lived in
    // the old image); ignored stay ignored; mask and pending set are preserved.
    for (int s = 0; s < 32; s++)
        if (p->sig_handler[s] != 0 /*SIG_DFL*/ && p->sig_handler[s] != 1 /*SIG_IGN*/)
            p->sig_handler[s] = 0;
    p->sig_restorer = 0;
    p->sigsusp_active = 0;
    p->sleep_until = 0; p->wait_pipe = NULL; p->pipe_wdone = 0; p->wait_tty = 0; p->wait_pty = NULL; p->recv_chan = -1;
    p->sig_eintr_rip = 0; p->sig_rerun_eintr = 0;
    p->fpu_valid = 0;                          // proc_fpu_reset() loads the clean state
    p->ki.nsleep_deadline = 0; p->ki.poll_active = 0; p->ki.poll_wait = 0;

    // Fresh register state: entry point, new stack, argc/argv/envp (System V).
    {
        uint64_t ino = tf->int_no;
        uint8_t* z = (uint8_t*)tf;
        for (int i = 0; i < (int)sizeof(trapframe_t); i++) z[i] = 0;
        tf->int_no = ino;
        tf->cs = 0x1B; tf->ss = 0x23;              // ring-3 selectors
        tf->rip = im.entry;
        tf->rsp = rsp;
        tf->rflags = 0x202;
        tf->rdi = (uint64_t)argc;
        tf->rsi = argvp;
        tf->rdx = envp;
    }
    proc_fpu_reset();
    aslr_log(im.stack_top, brk, mm);
    debugcon_writestring("[EXEC] pid="); debugcon_print_hex(p->pid);
    debugcon_writestring(" entry="); debugcon_print_hex(im.entry);
    debugcon_writestring(" argc="); debugcon_print_hex((uint64_t)argc);
    debugcon_writestring("\n");
    return 0;
}

// [M19] fork: create a copy-on-write child of 'parent'. 'tf' is the parent's
// live syscall trapframe; the child resumes there with rax=0 (fork()==0). The
// address space is COW-shared (vmm_fork_space); the pinned image + manifest are
// copied (the child owns them); a fresh kernel stack + trapframe are allocated.
// Driver privilege is NOT inherited (children are plain users). Returns the child
// (PROC_NEW: the caller makes it runnable with sched_make_ready) or NULL.
process_t* process_fork(process_t* parent, trapframe_t* tf) {
    if (!parent || !parent->space || !tf) return NULL;
    process_t* c = (process_t*)kmalloc(sizeof(process_t));
    if (!c) return NULL;
    *c = *parent;                              // shallow-copy all fields (incl. VMAs, fds)
    klock_task_reset(&c->klk);                 // [M50] the parent's klocks (the BKL) are not the child's
    c->refs = 0;                               // [M50-B1] the parent's references and its
    spin_init_class_dynamic(&c->mm_lock, LK_S35_MM, "mm");   //  (maybe held) mm_lock are not copied
    c->pid = alloc_pid();
    c->cpu_affinity = process_pick_home();     // [M48] push the child to the least-loaded CPU
    c->on_cpu = 0; c->wake_snap = 0; c->ready_tick = 0; c->smp_logged = 0;   // cpu_pin is inherited
    c->space = NULL; c->image = NULL; c->manifest = NULL; c->tf = NULL;
    c->kstack_top = 0; c->kstack_slot = 0; c->mapped_pages = NULL;
    c->sig_eintr_rip = 0; c->sig_rerun_eintr = 0;
    // The parent's live x87/SSE registers (its saved image may be stale).
    __asm__ volatile("fxsave64 %0" : "=m"(*(uint8_t(*)[512])c->fpu));
    c->fpu_valid = 1;

    {   // [M50-B1] Copy the VMA set and share the pages COW as one step under
        // the parent's mm_lock (vmm_fork_space rewrites the parent's PTEs).
        uint64_t mfl = mm_lock(parent);
        c->vmas = parent->vmas;
        c->space = vmm_fork_space(parent->space);
        mm_unlock(parent, mfl);
    }
    if (!c->space) { kfree(c); return NULL; }

    if (parent->image && parent->image_size) {
        c->image = (uint8_t*)kmalloc(parent->image_size);
        if (!c->image) { vmm_space_destroy(c->space); kfree(c); return NULL; }
        for (size_t i = 0; i < parent->image_size; i++) c->image[i] = parent->image[i];
        c->image_size = parent->image_size;
        // Re-point FILE VMA backing pointers into the child's own image copy.
        for (uint32_t i = 0; i < c->vmas.count; i++) {
            vma_t* v = &c->vmas.v[i];
            if (v->type == VMA_TYPE_FILE && v->file_base)
                v->file_base = c->image + ((uint64_t)v->file_base - (uint64_t)parent->image);
        }
    }
    if (parent->manifest) {
        c->manifest = kmalloc(sizeof(elf_manifest_t));
        if (c->manifest) { uint8_t* d = (uint8_t*)c->manifest; uint8_t* s = (uint8_t*)parent->manifest;
                           for (size_t i = 0; i < sizeof(elf_manifest_t); i++) d[i] = s[i]; }
    }
    c->tf = (trapframe_t*)kmalloc(sizeof(trapframe_t));
    if (!c->tf) { if (c->manifest) kfree(c->manifest); if (c->image) kfree(c->image);
                  vmm_space_destroy(c->space); kfree(c); return NULL; }
    *c->tf = *tf;                              // child resumes at the fork syscall...
    c->tf->rax = 0;                            // ...but fork() returns 0 in the child

    c->wait_pid = -1; c->wait_result = 0; c->wait_ready = 0; c->collected = 0;
    c->sleep_until = 0; c->recv_chan = -1; c->exit_code = 0; c->cpu_ticks = 0;
    c->wait_pipe = NULL; c->pipe_wdone = 0;    // [M25] not blocked on a pipe
    c->proc_type = PROC_TYPE_USER; c->drv_dev_id = -1; c->drv_caps = 0;
    // [M30] Signal dispositions/mask/restorer are inherited (shallow-copied); the
    // pending set starts empty in the child (POSIX). ppid = parent; pgid inherited.
    c->sig_pending = 0;
    c->ppid = parent->pid; c->pgid = parent->pgid; c->spawner_pid = 0;
    c->stop_report = 0; c->cont_report = 0; c->stop_sig = 0;
    c->wait_tty = 0; c->power_phase = 0; c->power_deadline = 0;
    c->wait_pty = NULL;                        // sid/ctty/ctty_detached inherited (copied)
    c->sigsusp_active = 0; c->sigsusp_saved = 0;
    c->state = PROC_BLOCKED;                   // not runnable until fully built
    { extern void kiface_proc_fork(process_t*); kiface_proc_fork(c); }

    // [M25] The child inherits the parent's open pipe ends (fds were shallow
    // copied) — bump each inherited end's refcount so EOF/EPIPE accounting stays
    // correct across fork. The pipe object itself is shared (kernel-owned).
    // Pty ends, sockets and the controlling terminal are counted the same way.
    { extern void pipe_ref(void*, int); extern void socket_ref(int);
      for (int i = 0; i < 32; i++) {
          if (!c->fds[i].used) continue;
          if (c->fds[i].is_pipe) pipe_ref(c->fds[i].inode, c->fds[i].pipe_w);
          else if (c->fds[i].is_sock) socket_ref(c->fds[i].is_sock - 1);
          else { pty_ref(c->fds[i].inode); input_obj_ref(c->fds[i].inode); }
      }
      pty_ctty_ref(c->ctty); }

    if (proc_add(c) != 0) {
        process_release_pipes(c);
        kfree(c->tf); if (c->manifest) kfree(c->manifest); if (c->image) kfree(c->image);
        vmm_space_destroy(c->space); kfree(c); return NULL;
    }
    c->kstack_top = vmm_alloc_kernel_stack_for_slot(c->kstack_slot);
    if (!c->kstack_top) {
        proc_remove(c); process_release_pipes(c); kfree(c->tf); if (c->manifest) kfree(c->manifest);
        if (c->image) kfree(c->image); vmm_space_destroy(c->space); kfree(c); return NULL;
    }
    debugcon_writestring("[FORK] parent="); debugcon_print_hex(parent->pid);
    debugcon_writestring(" -> child="); debugcon_print_hex(c->pid); debugcon_writestring("\n");
    c->state = PROC_NEW;                       // [M48] the caller publishes it (sched_make_ready)
    return c;
}

void process_print(const process_t* p) {
    if (!p) return;
    terminal_writestring("[PROC] PID=");
    char hx[]="0123456789ABCDEF"; for(int i=28;i>=0;i-=4) terminal_putchar(hx[(p->pid>>i)&0xF]);
    terminal_writestring(" entry="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(p->entry>>i)&0xF]);
    terminal_writestring(" stack_top="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(p->stack_top>>i)&0xF]);
    terminal_writestring(" state="); terminal_putchar('0'+p->state);
    terminal_writestring("\n");
}

// Drop every pipe end the process still holds. Called when a process exits
// (SYS_EXIT or a fatal signal/fault), not only when its zombie is reaped: a
// zombie is kept until the parent wait()s, so a write end held until reap means
// a parent that reads to EOF before waiting (bash command substitution `$(...)`)
// never sees EOF and deadlocks. Idempotent: released slots are cleared.
// Pty ends are dropped here too (a master's last close hangs the slave up; a
// slave's last close lets the master read EIO), and so is the controlling
// terminal (a session leader's exit hangs up its foreground group).
void process_release_pipes(process_t* p) {
    extern void pipe_unref(void*, int); extern void socket_unref(int);
    if (!p) return;
    for (int i = 0; i < 32; i++) {
        if (p->fds[i].used && p->fds[i].is_sock) {   // last ref closes the connection (FIN)
            int sx = p->fds[i].is_sock - 1;
            p->fds[i].is_sock = 0; p->fds[i].used = 0; p->fds[i].inode = NULL;
            socket_unref(sx);
            continue;
        }
        if (p->fds[i].used && p->fds[i].is_pipe) {
            void* pp = p->fds[i].inode;
            int   w  = p->fds[i].pipe_w;
            p->fds[i].is_pipe = 0; p->fds[i].used = 0; p->fds[i].inode = NULL;
            pipe_unref(pp, w);
        } else if (p->fds[i].used && pty_is_inode(p->fds[i].inode)) {
            void* ino = p->fds[i].inode;
            p->fds[i].used = 0; p->fds[i].inode = NULL;
            pty_unref(ino);
        } else if (p->fds[i].used && input_is_client(p->fds[i].inode)) {
            void* ino = p->fds[i].inode;           // [M40] evdev client (drops a grab)
            p->fds[i].used = 0; p->fds[i].inode = NULL;
            input_obj_unref(ino);
        }
    }
    pty_proc_exit(p);
    vtcon_proc_exit(p);                            // [M40] graphics owner gone: text console back
}

int process_destroy(process_t* p) {
    if (!p) return -1;
    // [M11] Drop any driver bindings first: the registry holds a process_t*; a
    // stale pointer surviving kfree(p) could later alias a reused process and
    // grant it the dead driver's device — clear it unconditionally.
    driver_remove_all_bindings(p);
    // [M25] Drop any still-open pipe ends and sockets so the peer observes
    // EOF/EPIPE and the objects are freed once the last reference is gone.
    // Normally already done at exit (process_release_pipes); this catches
    // processes destroyed without exiting.
    process_release_pipes(p);
    extern int elf_unload_process(process_t* p);
    elf_unload_process(p);                       // unmap + free user page frames, zero PTEs
    if (p->manifest) { kfree(p->manifest); p->manifest = NULL; }
    if (p->mapped_pages) { kfree(p->mapped_pages); p->mapped_pages = NULL; }
    // [M14] Free the pinned ELF image backing the demand-paged FILE VMAs.
    if (p->image) { kfree(p->image); p->image = NULL; p->image_size = 0; }
    // [M6] Free saved trapframe
    if (p->tf) { kfree(p->tf); p->tf = NULL; }
    // [M5] Free per-process kernel stack
    if (p->kstack_top) { vmm_free_kernel_stack_for_slot(p->kstack_slot); p->kstack_top = 0; }
    // [M8] Free the whole address space: PT/PD page-table frames, the private
    // PML4[0] PDPT, and the PML4 itself (also kfree's the vmm_space_t).
    if (p->space) { vmm_space_destroy(p->space); p->space = NULL; }
    proc_remove(p);
    kfree(p);
    kvlog("[PROC] distrutto\n");
    return 0;
}
