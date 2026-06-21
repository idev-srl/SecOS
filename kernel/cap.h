/*
 * SecOS Kernel - [M35] Generalized capability model + audit subsystem.
 *
 * The signed manifest is the trust root. By default a signed process has ambient
 * access (signature = trust). A manifest may opt into least-privilege confinement
 * (MANIFEST_FLAG_CAP_ENFORCE): the kernel then restricts the process to exactly
 * the capabilities its signed manifest declares. Capability checks are audited to
 * a queryable ring buffer (+ debugcon mirror), so denials/grants are traceable.
 */
#ifndef SECOS_CAP_H
#define SECOS_CAP_H

#include <stdint.h>

struct process; /* fwd (kernel/process.h) */

/* Capability identifiers (mirror MANIFEST_FLAG_CAP_* bit positions). */
typedef enum {
    CAP_NET     = (1u<<4),
    CAP_FS_READ = (1u<<6),
    CAP_FS_WRITE= (1u<<7),
    CAP_PROC    = (1u<<8),
    CAP_IPC     = (1u<<9),
    CAP_SIGNAL  = (1u<<10),
    CAP_TIME    = (1u<<11),
} cap_t;

/* True if `p` is allowed capability `cap`. Ambient (not enforcing) => always
 * allowed. Confined => allowed iff the bit is in the granted mask. */
int cap_allowed(const struct process* p, uint32_t cap);

/* Check + audit a capability for a syscall. Returns 1 if allowed (and audits the
 * grant only when confined), 0 if denied (always audited). `syscall_num` and
 * `name` are recorded for the audit trail. */
int cap_check(const struct process* p, uint32_t cap, uint64_t syscall_num, const char* name);

/* Audit record + query API (a queryable [AUDIT] log). */
typedef struct {
    uint32_t pid;
    uint32_t cap;
    uint64_t syscall_num;
    uint64_t seq;
    int      allowed;
    char     name[16];
} cap_audit_rec_t;

void cap_audit(uint32_t pid, uint32_t cap, uint64_t syscall_num, const char* name, int allowed);

/* Copy up to `max` most-recent audit records into `out` (newest last). Returns
 * the number copied. Used by the shell `audit` command / a procfs node. */
int  cap_audit_dump(cap_audit_rec_t* out, int max);
uint64_t cap_audit_total(void);

/* Human-readable capability name for a single CAP_* bit. */
const char* cap_name(uint32_t cap);

#endif
