/*
 * SecOS Kernel - [M35] Generalized capability model + audit subsystem.
 */
#include "cap.h"
#include "process.h"
#include "debugcon.h"
#include "kverbose.h"

#define AUDIT_RING 64
static cap_audit_rec_t g_audit[AUDIT_RING];
static uint64_t        g_audit_head;   /* total records ever written */

const char* cap_name(uint32_t cap) {
    switch (cap) {
        case CAP_NET:      return "NET";
        case CAP_FS_READ:  return "FS_READ";
        case CAP_FS_WRITE: return "FS_WRITE";
        case CAP_PROC:     return "PROC";
        case CAP_IPC:      return "IPC";
        case CAP_SIGNAL:   return "SIGNAL";
        case CAP_TIME:     return "TIME";
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
    cap_audit_rec_t* r = &g_audit[g_audit_head % AUDIT_RING];
    r->pid = pid; r->cap = cap; r->syscall_num = syscall_num;
    r->allowed = allowed; r->seq = g_audit_head;
    copy_name(r->name, name);
    g_audit_head++;
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

int cap_audit_dump(cap_audit_rec_t* out, int max) {
    int n = (g_audit_head < (uint64_t)AUDIT_RING) ? (int)g_audit_head : AUDIT_RING;
    if (n > max) n = max;
    /* copy oldest..newest of the last n records */
    uint64_t start = g_audit_head - (uint64_t)n;
    for (int i = 0; i < n; i++)
        out[i] = g_audit[(start + i) % AUDIT_RING];
    return n;
}

uint64_t cap_audit_total(void) { return g_audit_head; }
