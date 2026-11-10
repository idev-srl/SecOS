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
    CAP_POWER   = (1u<<12),   /* SYS_REBOOT: a confined program must declare it */
    CAP_INPUT   = (1u<<13),   /* [M40] explicit: open /dev/input/eventN */
    CAP_DISPLAY = (1u<<14),   /* [M40] explicit: open /dev/fb0, KD_GRAPHICS */
    CAP_NET_RAW = (1u<<17),   /* [U10] explicit: SOCK_RAW without being root */
    /* [M46] Audit-only pseudo capabilities (never granted through cap_mask):
     * used to log privileges a no_new_privs exec refused. */
    CAP_AUDIT_SETID  = (1u<<15),   /* set-id execution (MANIFEST_FLAG_SETID) */
    CAP_AUDIT_DRIVER = (1u<<31),   /* a Driver Space binding */
} cap_t;

/* [M46] Capabilities an ambient (non-CAP_ENFORCE) process holds implicitly, and
 * the explicit ones that even an ambient process holds only when its own signed
 * manifest declares them. */
#define CAP_IMPLICIT_SET (CAP_FS_READ | CAP_FS_WRITE | CAP_PROC | CAP_IPC | \
                          CAP_SIGNAL | CAP_TIME | CAP_POWER)
#define CAP_EXPLICIT_SET (CAP_NET | CAP_INPUT | CAP_DISPLAY | CAP_NET_RAW)

/* [U10] Capability ceiling of the D9 local signing key (programs SecOS
 * compiles and signs itself, U12a): manifest bits a machine-key signature must
 * never grant on its own. The keyring/signer that enforces it does not exist
 * yet; this is the list it must use (plus DRIVER, SETID, SESSION_GATEWAY,
 * which are not capabilities). */
#define CAP_LOCAL_SIGN_CEILING_DENY (CAP_NET_RAW | CAP_INPUT | CAP_DISPLAY | CAP_POWER)

/* [M46] The effective capability set of `p`: what cap_check / cap_check_explicit
 * / the CAP_NET gate would allow. Confined: cap_mask. Ambient: every implicit
 * cap plus the explicit caps its manifest declares. Kernel context: all. */
uint32_t cap_effective(const struct process* p);

/* [M46] Exec confinement: the capability state of the process that starts a new
 * image (the spawner for SYS_SPAWN, the old image for execve). */
typedef struct {
    int      valid;      /* 0 = kernel context: no bound, the child keeps its manifest set */
    int      enforce;
    int      nnp;
    uint32_t eff;
} cap_exec_bound_t;

/* Snapshot `p` (NULL or pid 0 = kernel context => no bound). */
cap_exec_bound_t cap_exec_bound(const struct process* p);

/* Apply the exec rule to `p`, whose caps were just loaded from its new signed
 * manifest. If the bound has no_new_privs set, the effective set can only
 * shrink: p->cap_mask = bound.eff & cap_effective(p), p becomes confined if the
 * bound was, and no_new_privs stays set. Lost manifest caps are audited. Also
 * sets no_new_privs automatically for a confined image (unless its manifest is
 * a signed session gateway, see MANIFEST_FLAG_SESSION_GATEWAY). */
void cap_exec_transition(struct process* p, cap_exec_bound_t bound, const char* what);

/* True if `p` is allowed capability `cap`. Ambient (not enforcing) => always
 * allowed. Confined => allowed iff the bit is in the granted mask. */
int cap_allowed(const struct process* p, uint32_t cap);

/* Check + audit a capability for a syscall. Returns 1 if allowed (and audits the
 * grant only when confined), 0 if denied (always audited). `syscall_num` and
 * `name` are recorded for the audit trail. */
int cap_check(const struct process* p, uint32_t cap, uint64_t syscall_num, const char* name);

/* [M40] Explicit capability (CAP_INPUT, CAP_DISPLAY): allowed only when the
 * signed manifest declares the bit, even for an ambient (unconfined) process.
 * Kernel context (no process / pid 0) is always allowed. Denials are audited. */
int cap_check_explicit(const struct process* p, uint32_t cap, uint64_t syscall_num, const char* name);

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
