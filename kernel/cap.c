/*
 * SecOS Kernel - [M35] Generalized capability model + audit subsystem.
 */
#include "cap.h"
#include "process.h"
#include "debugcon.h"
#include "kverbose.h"
#include "../mm/elf_manifest.h"

#define AUDIT_RING 64
static cap_audit_rec_t g_audit[AUDIT_RING];
/* Total records ever written. [M50-B1] A writer claims its slot with an
 * atomic fetch_add (concurrent syscalls on several CPUs audit at once); a
 * record's seq is stored last, so a reader can tell a slot being rewritten. */
static uint64_t        g_audit_head;

const char* cap_name(uint32_t cap) {
    switch (cap) {
        case CAP_NET:      return "NET";
        case CAP_FS_READ:  return "FS_READ";
        case CAP_FS_WRITE: return "FS_WRITE";
        case CAP_PROC:     return "PROC";
        case CAP_IPC:      return "IPC";
        case CAP_SIGNAL:   return "SIGNAL";
        case CAP_TIME:     return "TIME";
        case CAP_POWER:    return "POWER";
        case CAP_INPUT:    return "INPUT";
        case CAP_DISPLAY:  return "DISPLAY";
        case CAP_NET_RAW:  return "NET_RAW";
        case CAP_AUDIT_SETID:  return "SETID";
        case CAP_AUDIT_DRIVER: return "DRIVER";
        default:           return "?";
    }
}

int cap_allowed(const process_t* p, uint32_t cap) {
    if (!p) return 1;                 /* kernel context */
    if (!p->cap_enforce) return 1;    /* ambient: signature = full trust */
    return (p->cap_mask & cap) ? 1 : 0;
}

static void copy_name(char* dst, const char* src) {
    int i = 0;
    if (src) for (; i < 15 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

void cap_audit(uint32_t pid, uint32_t cap, uint64_t syscall_num, const char* name, int allowed) {
    uint64_t seq = __atomic_fetch_add(&g_audit_head, 1, __ATOMIC_RELAXED);
    cap_audit_rec_t* r = &g_audit[seq % AUDIT_RING];
    r->pid = pid; r->cap = cap; r->syscall_num = syscall_num;
    r->allowed = allowed;
    copy_name(r->name, name);
    __atomic_store_n(&r->seq, seq, __ATOMIC_RELEASE);
    /* Mirror to debugcon: denials always, grants only when verbose. */
    if (!allowed || g_kverbose) {
        debugcon_writestring("[AUDIT] pid=");
        debugcon_print_hex(pid);
        debugcon_writestring(allowed ? " ALLOW " : " DENY  ");
        debugcon_writestring(cap_name(cap));
        debugcon_writestring(" (");
        debugcon_writestring(name ? name : "?");
        debugcon_writestring(")\n");
    }
}

int cap_check(const process_t* p, uint32_t cap, uint64_t syscall_num, const char* name) {
    int ok = cap_allowed(p, cap);
    /* Audit only confined processes (ambient access is the unsurprising default;
     * auditing every syscall of every process would be noise). Denials are always
     * meaningful, so record them regardless. */
    if (p && p->cap_enforce)
        cap_audit(p->pid, cap, syscall_num, name, ok);
    else if (!ok)
        cap_audit(p ? p->pid : 0, cap, syscall_num, name, ok);
    return ok;
}

int cap_check_explicit(const process_t* p, uint32_t cap, uint64_t syscall_num, const char* name) {
    if (!p || p->pid == 0) return 1;                 /* kernel / idle-shell context */
    int ok = (p->cap_mask & cap) ? 1 : 0;
    if (!ok || p->cap_enforce) cap_audit(p->pid, cap, syscall_num, name, ok);
    return ok;
}

int cap_audit_dump(cap_audit_rec_t* out, int max) {
    uint64_t head = __atomic_load_n(&g_audit_head, __ATOMIC_ACQUIRE);   /* one snapshot */
    int n = (head < (uint64_t)AUDIT_RING) ? (int)head : AUDIT_RING;
    if (n > max) n = max;
    /* copy oldest..newest of the last n records */
    uint64_t start = head - (uint64_t)n;
    for (int i = 0; i < n; i++)
        out[i] = g_audit[(start + i) % AUDIT_RING];
    return n;
}

uint64_t cap_audit_total(void) { return __atomic_load_n(&g_audit_head, __ATOMIC_ACQUIRE); }

/* ---- [M46] Exec confinement ------------------------------------------------
 * Rule: once a process has no_new_privs (automatic for every confined program,
 * or set voluntarily with SYS_PRCTL), no spawn, fork or execve can widen its
 * effective capability set: the new image gets
 *     effective(new) = effective(old) & effective(new manifest)
 * and is confined if the old one was. no_new_privs is sticky. Without it (an
 * ambient program, a session gateway, kernel context) the new image gets its
 * own signed manifest set, as before. Set-id and Driver Space are refused under
 * no_new_privs by the callers (kernel/syscall.c, kernel/process.c). */
uint32_t cap_effective(const process_t* p) {
    if (!p || p->pid == 0) return CAP_IMPLICIT_SET | CAP_EXPLICIT_SET;
    if (p->cap_enforce) {
        uint32_t m = p->cap_mask & (CAP_IMPLICIT_SET | CAP_EXPLICIT_SET);
        if (!p->cap_net) m &= ~(uint32_t)CAP_NET;
        return m;
    }
    uint32_t m = CAP_IMPLICIT_SET | (p->cap_mask & (CAP_INPUT | CAP_DISPLAY | CAP_NET_RAW));
    if (p->cap_net) m |= CAP_NET;
    return m;
}

cap_exec_bound_t cap_exec_bound(const process_t* p) {
    cap_exec_bound_t b;
    b.valid = (p && p->pid != 0) ? 1 : 0;
    b.enforce = b.valid ? p->cap_enforce : 0;
    b.nnp = b.valid ? p->no_new_privs : 0;
    b.eff = cap_effective(p);
    return b;
}

void cap_exec_transition(process_t* p, cap_exec_bound_t b, const char* what) {
    if (!p) return;
    const elf_manifest_t* mf = (const elf_manifest_t*)p->manifest;
    int gateway = mf && (mf->flags & MANIFEST_FLAG_SESSION_GATEWAY);
    if (b.valid && b.nnp) {
        uint32_t want = cap_effective(p);
        uint32_t got  = want & b.eff;
        p->cap_enforce = p->cap_enforce || b.enforce;
        p->cap_mask = got;
        p->cap_net = (got & CAP_NET) ? 1 : 0;
        p->no_new_privs = 1;
        /* Audit every capability the new image's manifest asked for but the
         * bound withheld: that is exactly an escalation attempt. */
        uint32_t lost = want & ~got;
        for (uint32_t bit = 1; bit; bit <<= 1)
            if (lost & bit) cap_audit(p->pid, bit, 0, what, 0);
        debugcon_writestring("[CAPX] pid=");
        debugcon_print_hex(p->pid);
        debugcon_writestring(" no_new_privs exec (");
        debugcon_writestring(what);
        debugcon_writestring(") caps=");
        debugcon_print_hex(cap_effective(p));
        debugcon_writestring(p->cap_enforce ? " confined\n" : " ambient\n");
        return;
    }
    /* No bound: the image's own manifest set. A confined image is bound from
     * now on, unless it is a signed session gateway. */
    p->no_new_privs = (p->cap_enforce && !gateway) ? 1 : 0;
}
