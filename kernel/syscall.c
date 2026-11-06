/*
 * M3: Direct user pointer dereference is forbidden.
 * All user-supplied pointers must be validated with user_range_valid()
 * before kernel access.  Structured data (e.g. driver_call_t) must be
 * copied into a kernel-side buffer via copy_from_user() before use.
 * Results written back to user space must go through copy_to_user().
 */
#include "syscall.h"
#include "terminal.h"
#include "process.h"
#include "sched.h"
#include "vfs.h"
#include "driver_if.h"
#include "ipc.h"
#include "debugcon.h"
#include "../mm/user_copy.h"
#include "../mm/pagecache.h"  // [M20] unified file page cache
#include "socket.h"           // [M24] kernel-side socket layer (-Inet)
#include "pipe.h"             // [M25] anonymous pipes
#include "cap.h"              // [M35] generalized capability checks
#include "heap.h"             // kmalloc for read()/write() bounce buffers
#include "pty.h"              // pseudo-terminals, sessions, controlling terminal
#include "tty.h"
#include "input.h"            // [M40] /dev/input/eventN clients
#include "fbdev.h"            // [M40] /dev/fb0

extern uint64_t timer_get_ticks(void);
void proc_resolve_path(process_t* c, const char* in, char* out, int outsz);

// [M35] Capability gate: deny (EPERM=-1) + audit if a confined process lacks the
// capability. Ambient (unconfined) processes always pass. `num` is the in-scope
// syscall number in syscall_dispatch().
#define CAP_GATE(capbit, nm) do { \
    if (!cap_check(sched_get_current(), (capbit), num, (nm))) \
        return (uint64_t)(int64_t)-1; \
} while (0)

// [M16] Bounds for SYS_SPAWN argv copy-in (kept small; argstore is static).
#define SPAWN_MAX_ARGS 8
#define SPAWN_ARG_LEN  128

static int fd_alloc(process_t* p){ for(int i=0;i<32;i++){ if(!p->fds[i].used){ p->fds[i].used=1; p->fds[i].offset=0; p->fds[i].flags=0; p->fds[i].inode=NULL; p->fds[i].is_pipe=0; p->fds[i].pipe_w=0; p->fds[i].cloexec=0; p->fds[i].is_sock=0; return i; } } return -1; }

// Take / drop the extra reference a duplicated fd entry holds on its object
// (pipe end, socket, pty end or evdev client; pty_ref/pty_unref and
// input_obj_ref/unref ignore any inode that is not theirs).
// Used by dup/dup2/fcntl(F_DUPFD)/spawn stdio inheritance and close.
static void fd_entry_ref(struct proc_fd_entry* e){
    if(e->is_pipe) pipe_ref((pipe_t*)e->inode, e->pipe_w);
    else if(e->is_sock) socket_ref(e->is_sock - 1);
    else { pty_ref(e->inode); input_obj_ref(e->inode); }
}
static void fd_entry_unref(struct proc_fd_entry* e){
    if(e->is_pipe){ pipe_unref((pipe_t*)e->inode, e->pipe_w); e->is_pipe=0; }
    else if(e->is_sock){ int sx=e->is_sock-1; e->is_sock=0; socket_unref(sx); }
    else { void* ino=e->inode; e->inode=NULL; pty_unref(ino); input_obj_unref(ino); }   // a pty end may hang up
}

// [M25] pipe(): allocate a pipe object and two fds (read end + write end).
// pipe2(O_CLOEXEC) marks both ends close-on-exec (ksys_pipe2).
int ksys_pipe(int kfds[2]){
    extern process_t* sched_get_current(void); process_t* c=sched_get_current(); if(!c) return -1;
    pipe_t* p = pipe_alloc(); if(!p) return -1;
    int rfd=fd_alloc(c); if(rfd<0){ pipe_unref(p,0); pipe_unref(p,1); return -1; }
    int wfd=fd_alloc(c); if(wfd<0){ c->fds[rfd].used=0; pipe_unref(p,0); pipe_unref(p,1); return -1; }
    c->fds[rfd].inode=p; c->fds[rfd].is_pipe=1; c->fds[rfd].pipe_w=0;  // read end
    c->fds[wfd].inode=p; c->fds[wfd].is_pipe=1; c->fds[wfd].pipe_w=1;  // write end
    kfds[0]=rfd; kfds[1]=wfd;
    return 0;
}
#define K_O_CLOEXEC 0x80000   // Linux value (user/include/fcntl.h)
static int ksys_pipe2(int kfds[2], int flags){
    if(flags & ~K_O_CLOEXEC & ~0x800) return -22;                                  // -EINVAL
    int r=ksys_pipe(kfds); if(r!=0) return r;
    process_t* c=sched_get_current();
    if(flags & K_O_CLOEXEC){ c->fds[kfds[0]].cloexec=1; c->fds[kfds[1]].cloexec=1; }
    if(flags & 0x800){ c->fds[kfds[0]].flags|=0x800; c->fds[kfds[1]].flags|=0x800; }  // O_NONBLOCK -> -EAGAIN
    return 0;
}

int ksys_getpid(void){ extern process_t* sched_get_current(void); process_t* c=sched_get_current(); return c? (int)c->pid : 0; }

// [M10] Load + signature-verify a signed ELF from a VFS path and create a
// process for it (PROC_READY so the scheduler can pick it up). The signing
// gate inside process_create_from_elf refuses unsigned/tampered images.
static uint8_t g_spawn_buf[2*1024*1024]; // 2 MB: GNU bash with readline is ~1.1 MB
// [M16] Spawn with argv. The single static load buffer is safe because kernel
// code is non-preemptible (preemption fires only on a ring-3 timer tick) and
// process_create_from_elf_args copies/pins the image before returning.
// [M39] A spawned child inherits the caller's stdio (fds 0/1/2), like a
// fork+execve child would, so a spawned command whose caller redirected stdout
// into a pipe or a file writes there and not to the console. Only 0/1/2 are
// passed (spawn is not fork: the caller's other fds stay private), always without
// FD_CLOEXEC. Pipe ends gain a reference.
static void spawn_inherit_stdio(process_t* parent, process_t* child){
    if(!parent || !child) return;
    for(int i=0;i<3;i++){
        if(!parent->fds[i].used) continue;
        child->fds[i]=parent->fds[i];
        child->fds[i].cloexec=0;
        fd_entry_ref(&child->fds[i]);
    }
}

int ksys_spawn_argv(const char* path, int argc, const char* const argv[]){
    extern int vfs_read_all(const char*, void*, size_t);
    if(!path) return -1;
    int n = vfs_read_all(path, g_spawn_buf, sizeof(g_spawn_buf));
    if(n <= 0) return -1;
    process_t* p = process_create_from_elf_args(g_spawn_buf, (size_t)n, argc, argv);
    if(!p) return -1;
    spawn_inherit_stdio(sched_get_current(), p);   // before READY: never runs without them
    p->spawner_pid = sched_get_current() ? sched_get_current()->pid : 0; // SYS_WAIT ownership
    // Job control: a spawned program joins its spawner's process group, so a
    // job started through SYS_SPAWN (libc spawn(), the kernel shell) is reached
    // by `kill -STOP %1`, Ctrl-C/Ctrl-Z (foreground pgid) and SIGTTIN. (libc's
    // execve is the real in-place SYS_EXECVE, which keeps pid and pgid anyway.)
    // Same for the session and controlling terminal (a program run by a shell
    // on a pty must see that pty as /dev/tty and its job-control terminal).
    { process_t* sp = sched_get_current();
      if (sp && sp->pid != 0) { p->pgid = sp->pgid; pty_inherit_ctty(p, sp); } }
    // A spawned program starts in its spawner's cwd and umask, as a fork+exec
    // child would: without this `cd /tmp; tar cf a.tar f1` ran tar in "/".
    { process_t* sp = sched_get_current();
      if (sp && sp->pid != 0) p->ki.umask = sp->ki.umask;
      if (sp && sp->pid != 0 && sp->cwd[0]) {
          int i = 0; for (; sp->cwd[i] && i < (int)sizeof(p->cwd) - 1; i++) p->cwd[i] = sp->cwd[i];
          p->cwd[i] = 0;
      } }
    p->state = PROC_READY;
    return (int)p->pid;
}
int ksys_spawn(const char* path){ return ksys_spawn_argv(path, 0, (const char* const*)0); }

// ---- execve: in-place image replacement ----
// Bounds: all argv+envp strings together (incl. NULs) <= EXEC_ARG_MAX, at most
// EXEC_MAX_STRS strings, image <= EXEC_MAX_IMAGE. Over-limit -> -E2BIG / -ENOMEM.
#define EXEC_ARG_MAX   (128 * 1024)
#define EXEC_MAX_STRS  2048
#define EXEC_MAX_IMAGE (16u * 1024u * 1024u)

// Append the strings of a NULL-terminated user char* array to blob. Every user
// access goes through copy_from_user / strncpy_from_user (fault-tolerant).
static int exec_copy_strv(uint64_t uvec, char* blob, uint64_t* used, int* count){
    *count = 0;
    if(!uvec) return 0;                                   // NULL array == empty
    for(int i=0;;i++){
        uint64_t us = 0;
        if(copy_from_user(&us, (const void*)(uvec + (uint64_t)i*8u), sizeof(us)) != 0) return -EFAULT;
        if(!us) return 0;
        if(i >= EXEC_MAX_STRS) return -E2BIG;
        uint64_t room = EXEC_ARG_MAX - *used;
        if(room == 0) return -E2BIG;
        long l = strncpy_from_user(blob + *used, (const char*)us, (size_t)room);
        if(l == -ENAMETOOLONG) return -E2BIG;
        if(l < 0) return -EFAULT;
        *used += (uint64_t)l + 1u;
        (*count)++;
    }
}

long ksys_execve(struct trapframe* tf, uint64_t upath, uint64_t uargv, uint64_t uenvp){
    process_t* c = sched_get_current();
    if(!c || !tf) return -1;
    char kpath[256];
    long sl = strncpy_from_user(kpath, (const char*)upath, sizeof(kpath));
    if(sl < 0) return sl;                                 // -EFAULT / -ENAMETOOLONG
    if(sl == 0) return -ENOENT;
    char* blob = (char*)kmalloc(EXEC_ARG_MAX);
    if(!blob) return -ENOMEM;
    uint64_t used = 0; int argc = 0, envc = 0;
    long r = exec_copy_strv(uargv, blob, &used, &argc);
    if(r == 0) r = exec_copy_strv(uenvp, blob, &used, &envc);
    if(r != 0){ kfree(blob); return r; }

    char rp[256]; proc_resolve_path(c, kpath, rp, sizeof(rp));   // cwd-relative
    extern vfs_inode_t* vfs_lookup_follow(const char*);
    vfs_inode_t* ino = vfs_lookup_follow(rp);
    if(!ino){ kfree(blob); return -ENOENT; }
    if(ino->type != VFS_NODE_FILE || !ino->ops || !ino->ops->read){ kfree(blob); return -EACCES; }
    size_t size = ino->size;
    if(size == 0){ kfree(blob); return -ENOEXEC; }
    if(size > EXEC_MAX_IMAGE){ kfree(blob); return -ENOMEM; }
    // Private buffer (not the shared static spawn buffer: another CPU may be
    // spawning concurrently). It becomes the pinned image on success.
    uint8_t* img = (uint8_t*)kmalloc(size);
    if(!img){ kfree(blob); return -ENOMEM; }
    if(ino->ops->read(ino, 0, img, size) < 0){ kfree(img); kfree(blob); return -EACCES; }
    debugcon_writestring("[EXEC] execve pid="); debugcon_print_hex(c->pid);
    debugcon_writestring(" path="); debugcon_writestring(rp); debugcon_writestring("\n");
    r = process_exec(c, tf, img, size, blob, used, argc, envc);   // owns img
    if(r == 0){
        // /proc/<pid>/{cmdline,comm,stat}: the new image's argv (the blob holds
        // argc NUL-terminated strings first; cmdline keeps at most 256 bytes).
        extern void kiface_proc_set_cmdline(process_t*, int, const char* const*, const char*);
        const char* av[32]; int n = 0; uint64_t o = 0;
        while(n < argc && n < 32 && o < used){ av[n++] = blob + o; while(o < used && blob[o]) o++; o++; }
        kiface_proc_set_cmdline(c, n, av, rp);
    }
    kfree(blob);
    if(r != 0){
        debugcon_writestring("[EXEC] execve failed rc="); debugcon_print_hex((uint64_t)(-r));
        debugcon_writestring(" (caller continues)\n");
    }
    return r;
}

// Poll variant (used by the kernel shell): 0 if the pid has exited (ZOMBIE) or
// is gone; 1 if still running. User processes use the blocking SYS_WAIT instead.
int ksys_wait(int pid){
    extern process_t* process_find_by_pid(uint32_t);
    process_t* t = process_find_by_pid((uint32_t)pid);
    if(!t) return 0;
    return (t->state == PROC_ZOMBIE) ? 0 : 1;
}

// [M17] Non-blocking recv attempt used by the blocking SYS_MSG_RECV path in
// syscall_trap.c. Validates + copies to user. Returns: n>0 (bytes delivered),
// 0 (channel empty -> caller should block), or <0 (fault/error).
int ksys_msg_recv_try(int chan, void* ubuf, int len){
    if (len <= 0 || len > 256 || !user_range_valid(ubuf, (size_t)len)) return -EFAULT;
    uint8_t kbuf[256];
    int n = ipc_recv(chan, kbuf, len);
    if (n > 0 && copy_to_user(ubuf, kbuf, (size_t)n) != 0) return -EFAULT;
    return n;
}

// ---- [M18] Dynamic memory: brk / mmap / munmap / mprotect ----
// All operate on the current process's VMA set; pages are demand-faulted (the
// M14 #PF path), so these calls only manipulate VMAs + already-present PTEs.
static uint64_t m18_prot_to_flags(int prot){
    // PROT_NONE: reserved but inaccessible. Never demand-faulted; present pages
    // are parked not-present (vmm_protect_in_space) so neither ring 3 nor the
    // kernel's uaccess can touch them (user -> SIGSEGV, syscall -> -EFAULT).
    if (!(prot & (PROT_READ | PROT_WRITE | PROT_EXEC)))
        return VMM_FLAG_PROTNONE | VMM_FLAG_NOEXEC;
    uint64_t f = VMM_FLAG_USER;
    if (prot & PROT_WRITE) f |= VMM_FLAG_RW;
    if (!(prot & PROT_EXEC)) f |= VMM_FLAG_NOEXEC; // NX unless EXEC requested
    return f;
}
static inline uint64_t pgup(uint64_t x){ return (x + 0xFFFULL) & ~0xFFFULL; }
// A page-aligned [addr, addr+len) that lies wholly in the user half (no wrap).
// munmap/mprotect walk page tables over the range: an unchecked len let
// munmap(0, 2^63) free KERNEL frames and mprotect(.., 2^63) set USER on kernel
// PTEs (and loop ~2^35 times). Returns the rounded length, or 0 if invalid.
static uint64_t m18_user_span(uint64_t addr, uint64_t len){
    if ((addr & 0xFFF) || len == 0) return 0;
    if (addr < USER_CODE_BASE || addr >= VMM_USER_TOP) return 0; // low addrs alias
                                                                 // the identity map
    if (len > VMM_USER_TOP - addr) return 0;
    len = pgup(len);
    if (len > VMM_USER_TOP - addr) return 0;
    return len;
}

uint64_t ksys_brk(uint64_t new_brk){
    process_t* c = sched_get_current();
    if (!c) return 0;
    if (new_brk == 0) return c->brk_cur;            // query current break
    new_brk = pgup(new_brk);
    if (new_brk < c->brk_start || new_brk >= USER_MMAP_BASE) return c->brk_cur;
    uint64_t old = c->brk_cur;
    if (new_brk == old) return old;
    if (new_brk > old) {                            // grow
        if (c->mem_limit && vma_total_bytes(&c->vmas) + (new_brk - old) > c->mem_limit)
            return old;                             // would exceed signed mem limit
        // Extend the heap's LAST piece (mprotect may have split the heap) when
        // it is plain RW anon; otherwise add a fresh RW piece for [old, new).
        const uint64_t hf = VMM_FLAG_USER|VMM_FLAG_RW|VMM_FLAG_NOEXEC;
        vma_t* h = (old > c->brk_start) ? vma_find_mut(&c->vmas, old - 1) : 0;
        if (h && h->end == old && h->type == VMA_TYPE_ANON && h->flags == hf) h->end = new_brk;
        else if (vma_add(&c->vmas, old, new_brk, hf, VMA_TYPE_ANON,0,0,0,0) != 0)
            return old;
    } else {                                        // shrink
        for (uint64_t va = new_brk; va < old; va += 0x1000) vmm_unmap_in_space(c->space, va);
        for (uint32_t i = 0; i < c->vmas.count; i++) {   // trim every heap piece
            vma_t* v = &c->vmas.v[i];
            if (v->type == VMA_TYPE_NONE || v->end <= new_brk || v->start >= old) continue;
            if (v->start < c->brk_start) continue;       // not a heap piece
            if (v->start >= new_brk) vma_remove(&c->vmas, v); else v->end = new_brk;
        }
    }
    c->brk_cur = new_brk;
    return new_brk;
}

uint64_t ksys_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd){
    return ksys_mmap6(addr, len, prot, flags, fd, 0);
}
// [M40] mmap with a file offset (6th argument, r9). Only device mappings
// (/dev/fb0, MAP_SHARED) honour it; file mappings stay offset 0 (M20).
uint64_t ksys_mmap6(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off){
    (void)addr;                                     // hint ignored (bump allocator)
    process_t* c = sched_get_current();
    if (!c || len == 0) return (uint64_t)-1;
    if (!(flags & MAP_ANONYMOUS) && fd >= 0 && fd < 32 && c->fds[fd].used &&
        !c->fds[fd].is_pipe && !c->fds[fd].is_sock && c->fds[fd].inode) {
        extern int devfs_is_fb(void* inode);
        if (devfs_is_fb(c->fds[fd].inode))
            return (uint64_t)fbdev_mmap(c, len, prot, flags, off);
        if (input_is_client(c->fds[fd].inode)) return (uint64_t)(int64_t)-19;   // -ENODEV
    }
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) return (uint64_t)-1; // W^X
    uint64_t base = c->mmap_next;
    if (base >= USER_MMAP_END || len > USER_MMAP_END - base) return (uint64_t)-1; // arena exhausted (no wrap)
    len = pgup(len);
    if (len > USER_MMAP_END - base) return (uint64_t)-1;
    if (c->mem_limit && vma_total_bytes(&c->vmas) + len > c->mem_limit) return (uint64_t)-1;
    if (flags & MAP_ANONYMOUS) {
        if (vma_add(&c->vmas, base, base + len, m18_prot_to_flags(prot), VMA_TYPE_ANON,0,0,0,0) != 0)
            return (uint64_t)-1;
    } else {
        // [M20] File-backed mmap (MAP_PRIVATE) via the page cache, from offset 0.
        if (fd < 0 || fd >= 32 || !c->fds[fd].used) return (uint64_t)-1;
        if (c->fds[fd].is_pipe || c->fds[fd].is_sock) return (uint64_t)-1;  // not a file
        vfs_inode_t* ino = (vfs_inode_t*)c->fds[fd].inode;
        if (!ino) return (uint64_t)-1;
        if (vma_add_file(&c->vmas, base, base + len, m18_prot_to_flags(prot), ino, 0) != 0)
            return (uint64_t)-1;
    }
    c->mmap_next = base + len;
    return base;
}

int ksys_munmap(uint64_t addr, uint64_t len){
    process_t* c = sched_get_current();
    if (!c) return -1;
    len = m18_user_span(addr, len);
    if (!len) return -1;                                     // EINVAL
    uint64_t end = addr + len;
    // Every user page is VMA-backed (M14), so only VMA intersections can hold
    // present PTEs: unmap those instead of walking the whole (maybe huge) range.
    for (uint32_t i = 0; i < c->vmas.count; i++) {
        const vma_t* v = &c->vmas.v[i];
        if (v->type == VMA_TYPE_NONE || !(addr < v->end && v->start < end)) continue;
        uint64_t lo = addr > v->start ? addr : v->start;
        uint64_t hi = end  < v->end   ? end  : v->end;
        for (uint64_t va = lo & ~0xFFFULL; va < hi; va += 0x1000) vmm_unmap_in_space(c->space, va);
    }
    for (uint32_t i = 0; i < c->vmas.count; i++) {
        vma_t* v = &c->vmas.v[i];
        if (v->type == VMA_TYPE_NONE) continue;
        if (!(addr < v->end && v->start < end)) continue;        // no overlap
        if (addr <= v->start && end >= v->end)      vma_remove(&c->vmas, v);   // fully covered
        else if (addr <= v->start)                  v->start = end;            // trim front
        else if (end >= v->end)                     v->end = addr;             // trim back
        else if (v->type == VMA_TYPE_PHYS) {                                   // hole in a device map:
            if (vma_split(&c->vmas, v, end) != 0) { /* set full: the tail is dropped */ }
            v->end = addr;                                                     // tail kept, rebased
        }
        else {                                                                 // hole: split (ANON)
            uint64_t old_end = v->end; v->end = addr;
            if (v->type == VMA_TYPE_ANON)
                vma_add(&c->vmas, end, old_end, v->flags, VMA_TYPE_ANON,0,0,0,0);
        }
    }
    return 0;
}

int ksys_mprotect(uint64_t addr, uint64_t len, int prot){
    process_t* c = sched_get_current();
    if (!c) return -1;
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) return -1;   // W^X
    len = m18_user_span(addr, len);
    if (!len) return -1;                                        // EINVAL
    uint64_t end = addr + len;
    if (!vma_overlaps(&c->vmas, addr, end)) return -1;          // must be backed
    // [M40] Device memory (the framebuffer) is never executable.
    if (prot & PROT_EXEC) {
        for (uint32_t i = 0; i < c->vmas.count; i++) {
            const vma_t* v = &c->vmas.v[i];
            if (v->type == VMA_TYPE_PHYS && addr < v->end && v->start < end) return -1;
        }
    }
    uint64_t newf = m18_prot_to_flags(prot);
    // Split VMAs straddling either boundary so the new protection applies to
    // exactly [addr, end): pages demand-faulted later take the VMA's flags, so a
    // partial mprotect must not leave them with the old protection (e.g. a
    // PROT_NONE guard page inside a larger mapping).
    for (uint32_t i = 0; i < c->vmas.count; i++) {
        vma_t* v = &c->vmas.v[i];
        if (v->type == VMA_TYPE_NONE) continue;
        if (v->start < addr && addr < v->end && vma_split(&c->vmas, v, addr) != 0) return -1;
    }
    for (uint32_t i = 0; i < c->vmas.count; i++) {
        vma_t* v = &c->vmas.v[i];
        if (v->type == VMA_TYPE_NONE) continue;
        if (v->start < end && end < v->end && vma_split(&c->vmas, v, end) != 0) return -1;
    }
    for (uint32_t i = 0; i < c->vmas.count; i++) {
        vma_t* v = &c->vmas.v[i];
        if (v->type == VMA_TYPE_NONE) continue;
        if (addr <= v->start && v->end <= end) v->flags = newf;  // now wholly in range
    }
    // Re-protect present PTEs, VMA intersections only (see ksys_munmap).
    for (uint32_t i = 0; i < c->vmas.count; i++) {
        const vma_t* v = &c->vmas.v[i];
        if (v->type == VMA_TYPE_NONE || !(addr < v->end && v->start < end)) continue;
        uint64_t lo = addr > v->start ? addr : v->start;
        uint64_t hi = end  < v->end   ? end  : v->end;
        for (uint64_t va = lo & ~0xFFFULL; va < hi; va += 0x1000) vmm_protect_in_space(c->space, va, newf);
    }
    return 0;
}
void ksys_exit(int status){ (void)status; extern process_t* sched_get_current(void); extern int process_destroy(process_t*); process_t* c=sched_get_current(); if(c){ process_destroy(c); } }
// [M38] open() now honours O_CREAT (0x40) / O_TRUNC (0x200) / O_APPEND (0x400),
// so user programs (and the shell's `>`/`>>` redirection) can create+write files.
// [M39] Resolve a (possibly relative) path against the current process cwd into
// an absolute, normalized kernel path ("." / ".." / "//" collapsed). Bounded.
void proc_resolve_path(process_t* c, const char* in, char* out, int outsz){
    char tmp[512]; int t=0;
    if(in && in[0]=='/'){
        for(int i=0; in[i] && t<511; i++) tmp[t++]=in[i];
    } else {
        const char* cw = (c && c->cwd[0]) ? c->cwd : "/";
        for(int i=0; cw[i] && t<511; i++) tmp[t++]=cw[i];
        if(t==0 || tmp[t-1]!='/'){ if(t<511) tmp[t++]='/'; }
        for(int i=0; in && in[i] && t<511; i++) tmp[t++]=in[i];
    }
    tmp[t]=0;
    char* parts[64]; int np=0; int i=0;
    while(tmp[i]){
        while(tmp[i]=='/') i++;
        if(!tmp[i]) break;
        char* seg=&tmp[i];
        while(tmp[i] && tmp[i]!='/') i++;
        if(tmp[i]){ tmp[i]=0; i++; }
        if(seg[0]=='.'&&seg[1]==0) continue;
        if(seg[0]=='.'&&seg[1]=='.'&&seg[2]==0){ if(np>0) np--; continue; }
        if(np<64) parts[np++]=seg;
    }
    int o=0;
    if(np==0){ if(outsz>1){ out[0]='/'; out[1]=0; } return; }
    for(int p=0;p<np;p++){ if(o<outsz-1) out[o++]='/'; for(int k=0;parts[p][k]&&o<outsz-1;k++) out[o++]=parts[p][k]; }
    out[o]=0;
}

// open() follows symlinks in every path component (final one included), honours
// O_EXCL (0x80) with O_CREAT, and creates with mode & ~umask (0 = 0666).
int ksys_open_mode(const char* path, int flags, uint32_t mode){
    extern int vfs_create(const char*, const void*, size_t);
    extern int vfs_truncate(const char*, size_t);
    extern process_t* sched_get_current(void);
    extern void kiface_apply_create_mode(const char*, uint32_t, int);
    process_t* c=sched_get_current(); if(!c) return -1;
    char rp[256]; proc_resolve_path(c, path, rp, sizeof(rp)); path = rp;  // [M39] cwd-relative
    int lerr=0;
    vfs_inode_t* ino=vfs_lookup_ex(path, 1, &lerr);
    if(ino && (flags & 0x40) && (flags & 0x80)) return -17;                  // O_EXCL: -EEXIST
    if(!ino && (flags & 0x40) && (lerr==0 || lerr==-2)){                     // O_CREAT
        if(vfs_create(path,"",0)==0){ kiface_apply_create_mode(path, mode, 0); ino=vfs_lookup_follow(path); }
    }
    if(!ino) return lerr ? lerr : -2;   // [M39] -ENOENT so libc can set errno (bash rcfile probing)
    if((flags & 0x200) && ino->size){                                        // O_TRUNC
        vfs_truncate(path,0); ino=vfs_lookup_follow(path); if(!ino) return -1;
        pagecache_invalidate(ino, 0, 1ULL<<62);
    }
    // /dev/ptmx -> a new pty master, /dev/tty -> the controlling terminal,
    // /dev/pts/N -> that slave (may become the session's controlling tty).
    int pr = pty_open_hook(c, &ino, flags); if(pr<0) return pr;
    // [M40] /dev/fb0 and /dev/input/eventN: capability gate (CAP_DISPLAY /
    // CAP_INPUT), and a per-open evdev client substituted into the fd.
    { extern int devfs_open_hook(void*, vfs_inode_t**, int);
      int gr = devfs_open_hook(c, &ino, flags); if(gr<0) return gr; }
    int fd=fd_alloc(c); if(fd<0){ pty_unref(ino); input_obj_unref(ino); return -1; }
    c->fds[fd].inode=ino; c->fds[fd].flags=flags;
    c->fds[fd].offset = (flags & 0x400) ? ino->size : 0;    // O_APPEND -> seek to end
    c->fds[fd].cloexec = (flags & K_O_CLOEXEC) ? 1 : 0;     // O_CLOEXEC
    c->fds[fd].flags = flags & ~K_O_CLOEXEC;                // F_GETFL: status flags only
    return fd;
}
int ksys_open(const char* path, int flags){ return ksys_open_mode(path, flags, 0); }
int ksys_close(int fd){ extern process_t* sched_get_current(void); process_t* c=sched_get_current(); if(!c) return -1; if(fd<0||fd>=32) return -1; if(!c->fds[fd].used) return -1;
    fd_entry_unref(&c->fds[fd]);   // drop pipe end (EOF/EPIPE) / socket ref (FIN on the last) / pty end (hangup)
    c->fds[fd].used=0; c->fds[fd].inode=NULL; return 0; }
// [uaccess] read()/write() never touch the user buffer directly: data moves
// through a kernel bounce buffer and copy_to/from_user, so an unmapped or
// read-only user page yields -EFAULT (or a short count) instead of a ring-0
// fault. Small transfers bounce on the stack; larger ones through one kmalloc'd
// chunk (UIO_CHUNK), looping. Progress already made is reported as a short count.
#define UIO_SMALL 512
#define ENOMEM_K 12
#define UIO_CHUNK 65536
typedef struct { uint8_t* p; uint8_t small[UIO_SMALL]; int heap; int cap; } uio_buf_t;
static int uio_get(uio_buf_t* u, int len){
    if(len <= UIO_SMALL){ u->p=u->small; u->heap=0; u->cap=UIO_SMALL; return 0; }
    u->cap = len < UIO_CHUNK ? len : UIO_CHUNK;
    u->p = (uint8_t*)kmalloc((size_t)u->cap); u->heap=1;
    return u->p ? 0 : -1;
}
static void uio_put(uio_buf_t* u){ if(u->heap && u->p) kfree(u->p); u->p=0; }

// Socket fds: one bounce chunk per call (a short read/write is POSIX-legal).
// Returns bytes, 0 = EOF, -EAGAIN = would block (kernel/syscall_trap.c sleeps
// and re-runs the syscall unless the fd is O_NONBLOCK), other -errno.
#define SOCK_IO_CHUNK 16384
static int ksys_sock_read(process_t* c, int fd, void* ubuf, int len){
    uio_buf_t u; int want = len < SOCK_IO_CHUNK ? len : SOCK_IO_CHUNK;
    if(uio_get(&u, want)!=0) return -ENOMEM_K;
    int r = socket_recv(c->fds[fd].is_sock - 1, u.p, want, 0, 0);
    if(r>0 && copy_to_user(ubuf, u.p, (size_t)r)!=0) r = -EFAULT;
    uio_put(&u);
    return r;
}
static int ksys_sock_write(process_t* c, int fd, const void* ubuf, int len){
    uio_buf_t u; int want = len < SOCK_IO_CHUNK ? len : SOCK_IO_CHUNK;
    if(uio_get(&u, want)!=0) return -ENOMEM_K;
    int r;
    if(copy_from_user(u.p, ubuf, (size_t)want)!=0) r = -EFAULT;
    else r = socket_send(c->fds[fd].is_sock - 1, u.p, want);
    uio_put(&u);
    return r;
}

int ksys_read(int fd, void* ubuf, int len){ extern process_t* sched_get_current(void); process_t* c=sched_get_current(); if(!c) return -1;
    if(fd<0||fd>=32) return -1;
    if(len<=0) return -1;
    if(c->fds[fd].used && c->fds[fd].is_sock) return ksys_sock_read(c, fd, ubuf, len);
    int is_pipe = c->fds[fd].used && c->fds[fd].is_pipe;
    int is_file = c->fds[fd].used && c->fds[fd].inode && !is_pipe;
    if(!is_pipe && !is_file && fd!=0) return -1;
    uio_buf_t u;
    // [M30] A redirected pipe/file end takes precedence over the console
    // special-case, so a pipeline can wire its read end onto fd 0 (stdin).
    if(is_pipe){                                            // [M25] read end only
        if(c->fds[fd].pipe_w) return -1;
        int want = len < 4096 ? len : 4096;                 // the pipe holds <= 4 KB: one chunk
        if(uio_get(&u, want)!=0) return -1;
        int r = pipe_read((pipe_t*)c->fds[fd].inode, u.p, want);
        if(r>0 && copy_to_user(ubuf, u.p, (size_t)r)!=0) r = -EFAULT;  // data consumed (as Linux)
        uio_put(&u);
        return r;
    }
    if(!is_file){                                           // [M25] fd 0 -> console TTY
        extern int tty_read_try(void* buf, int len);   // -2: block (wait_tty)
        int want = len < UIO_SMALL ? len : UIO_SMALL;       // a TTY line is <= 256 bytes
        int r = tty_read_try(u.small, want);
        if(r>0 && copy_to_user(ubuf, u.small, (size_t)r)!=0) return -EFAULT;
        return r;
    }
    vfs_inode_t* ino=(vfs_inode_t*)c->fds[fd].inode;
    if(input_is_client(ino)){                               // [M40] evdev: whole input_events
        int want = len < 4096 ? len : 4096;
        if(uio_get(&u, want)!=0) return -1;
        int r = input_read(ino, u.p, want, (c->fds[fd].flags & 0x800) != 0);  // O_NONBLOCK
        if(r>0 && copy_to_user(ubuf, u.p, (size_t)r)!=0) r = -EFAULT;
        uio_put(&u);
        return r;                                           // -2: blocks (timed re-poll)
    }
    if(pty_is_inode(ino)){                                  // pty master/slave: one chunk
        int want = len < 4096 ? len : 4096;
        if(uio_get(&u, want)!=0) return -1;
        int r = pty_read(ino, u.p, want, (c->fds[fd].flags & 0x800) != 0);  // O_NONBLOCK
        if(r>0 && copy_to_user(ubuf, u.p, (size_t)r)!=0) r = -EFAULT;
        uio_put(&u);
        return r;                                           // -2: blocks (wait_pty)
    }
    if(!ino->ops||!ino->ops->read) return -1;
    if(uio_get(&u, len)!=0) return -1;
    // [M20] Read through the unified page cache so file read() and file-backed
    // mmap see the same pages (coherent). [M23] Virtual FSes (devfs/procfs/sysfs)
    // are dynamic — bypass the cache and read live via ->read().
    int done=0, err=0;
    while(done < len){
        int want = (len-done) < u.cap ? (len-done) : u.cap;
        size_t off=c->fds[fd].offset; int r;
        if(ino->ops->flags & VFS_FS_NOCACHE) r=ino->ops->read(ino, off, u.p, (size_t)want);
        else r=pagecache_read(ino, off, u.p, (uint64_t)want);
        if(r<=0){ if(done==0) err=r; break; }
        if(copy_to_user((uint8_t*)ubuf+done, u.p, (size_t)r)!=0){ if(done==0) err=-EFAULT; break; }
        c->fds[fd].offset += (uint64_t)r; done += r;
        if(r < want) break;                                 // short read: EOF / device end
    }
    uio_put(&u);
    return done ? done : err; }
int ksys_write(int fd, const void* ubuf, int len){ extern process_t* sched_get_current(void); process_t* c=sched_get_current(); if(!c) return -1;
    if(fd<0||fd>=32) return -1;
    if(len<=0) return -1;
    if(c->fds[fd].used && c->fds[fd].is_sock) return ksys_sock_write(c, fd, ubuf, len);
    int is_pipe = c->fds[fd].used && c->fds[fd].is_pipe;
    int is_file = c->fds[fd].used && c->fds[fd].inode && !is_pipe;
    if(!is_pipe && !is_file && fd!=1 && fd!=2) return -1;
    if(is_pipe && !c->fds[fd].pipe_w) return -1;            // [M25] write end only
    vfs_inode_t* ino = is_file ? (vfs_inode_t*)c->fds[fd].inode : 0;
    uio_buf_t u;
    if(ino && pty_is_inode(ino)){
        // A pty end: as much as fits, chunk by chunk. Once something was
        // written, stop at the first "full" instead of blocking (short write).
        int nb = (c->fds[fd].flags & 0x800) != 0;          // O_NONBLOCK
        if(uio_get(&u, len)!=0) return -1;
        int done=0, err=0;
        while(done < len){
            int want = (len-done) < u.cap ? (len-done) : u.cap;
            if(copy_from_user(u.p, (const uint8_t*)ubuf+done, (size_t)want)!=0){ if(done==0) err=-EFAULT; break; }
            int r = pty_write(ino, u.p, want, nb || done>0);
            if(r<=0){ if(done==0) err=r; break; }       // -2: blocks (wait_pty)
            done += r;
            if(r < want) break;
        }
        uio_put(&u);
        return done ? done : err;
    }
    if(ino && (!ino->ops||!ino->ops->write)) return -1;
    if(is_pipe){
        // [M30] A redirected pipe end takes precedence over the console.
        // Blocking-pipe POSIX semantics: the whole buffer is written before
        // returning, and a write of <= PIPE_BUF bytes atomically (pipe_write).
        // When the pipe fills part-way we return -2 (would block); the trap
        // layer re-runs the syscall on wake, and c->pipe_wdone carries the
        // bytes already queued so the re-run resumes after them. A signal
        // makes the trap return pipe_wdone (short count), or -EINTR if 0.
        pipe_t* pp = (pipe_t*)c->fds[fd].inode;
        int done = c->pipe_wdone, err = 0;
        c->pipe_wdone = 0;
        if(done < 0 || done >= len) done = 0;
        if(uio_get(&u, len < PIPE_BUF ? len : PIPE_BUF)!=0) return done ? done : -1;
        while(done < len){
            int want = (len-done) < PIPE_BUF ? (len-done) : PIPE_BUF;
            if(copy_from_user(u.p, (const uint8_t*)ubuf+done, (size_t)want)!=0){ err = -EFAULT; break; }
            int r = (len <= PIPE_BUF) ? pipe_write(pp, u.p, want)        // atomic
                                      : pipe_write_some(pp, u.p, want);  // may split
            if(r == -2){ c->pipe_wdone = done; err = -2; break; }       // block; resume at done
            if(r <= 0){ err = (r == -1) ? -EPIPE : r; break; }           // no reader left
            done += r;
        }
        uio_put(&u);
        if(err == -2) return -2;
        return done ? done : err;
    }
    if(uio_get(&u, len)!=0) return -1;
    int done=0, err=0;
    while(done < len){
        int want = (len-done) < u.cap ? (len-done) : u.cap;
        if(copy_from_user(u.p, (const uint8_t*)ubuf+done, (size_t)want)!=0){ if(done==0) err=-EFAULT; break; }
        int r;
        if(ino){
            // O_APPEND: every write goes to the current end of file (POSIX).
            // The offset is per-fd, so writers sharing a log through fork
            // (a daemon and its children) overwrote each other's lines.
            // The fd's inode does not track writes made through other fds
            // (ramfs keeps the size in its entry), so look the size up fresh.
            if(c->fds[fd].flags & 0x400){
                vfs_inode_t* cur = vfs_lookup(ino->path);
                c->fds[fd].offset = (cur == ino || (cur && cur->fs_data == ino->fs_data)) ? cur->size : ino->size;
            }
            size_t off=c->fds[fd].offset;
            r=ino->ops->write(ino, off, u.p, (size_t)want);
            if(r>0){ c->fds[fd].offset += (uint64_t)r; pagecache_invalidate(ino, off, (uint64_t)r); }
        } else {
            // [M9] fd 1 (stdout) / 2 (stderr) with no redirection -> console,
            // through the TTY's termios output processing (OPOST/ONLCR).
            extern int tty_write(const void* buf, int len);
            tty_write(u.p, want);
            // Not a kernel message: skip klog. One chunk at a time under the
            // debugcon line lock (irqs off), so a scheduler/kernel line printed
            // from an interrupt cannot land in the middle of the user's line.
            { extern uint64_t debugcon_line_lock(void); extern void debugcon_line_unlock(uint64_t);
              uint64_t df = debugcon_line_lock();
              for(int i=0;i<want;i++) debugcon_putchar_raw((char)u.p[i]);
              debugcon_line_unlock(df); }
            r = want;
        }
        if(r<=0){ if(done==0) err=r; break; }
        done += r;
        if(r < want) break;                                 // short write (disk full, ...)
    }
    uio_put(&u);
    return done ? done : err; }

long ksys_lseek(int fd, long offset, int whence){
    extern process_t* sched_get_current(void);
    process_t* c=sched_get_current(); if(!c) return -1;
    if(fd<0||fd>=32||!c->fds[fd].used) return -1;
    if(c->fds[fd].is_pipe || c->fds[fd].is_sock) return -29;   // -ESPIPE: not a seekable file
    vfs_inode_t* ino=(vfs_inode_t*)c->fds[fd].inode; if(!ino) return -1;
    if(c->fds[fd].is_pipe || pty_is_inode(ino)) return -29;   // -ESPIPE: a stream
    long base;
    switch(whence){
        case SEEK_SET: base=0; break;
        case SEEK_CUR: base=(long)c->fds[fd].offset; break;
        case SEEK_END: base=(long)ino->size; break;
        default: return -1;
    }
    long np=base+offset; if(np<0) return -1;
    c->fds[fd].offset=(uint64_t)np;
    return np;
}

// [M26] Fill a stat buffer from an inode, synthesizing POSIX metadata when the
// underlying FS did not populate it (mode==0). Shared by stat and lstat.
static void stat_fill(const vfs_inode_t* ino, struct secos_stat* st){
    st->st_size = ino->size;
    if(ino->mode){
        st->st_mode = ino->mode;                 // FS provided the real mode
    } else {                                     // synthesize from the node type
        uint32_t type = (ino->type==VFS_NODE_DIR) ? S_IFDIR :
                        (ino->type==VFS_NODE_SYMLINK) ? S_IFLNK : S_IFREG;
        st->st_mode = type | (ino->type==VFS_NODE_DIR ? 0755 : 0644);
    }
    st->st_nlink = ino->nlink ? ino->nlink : 1;
    st->st_uid = ino->uid; st->st_gid = ino->gid;
    st->st_atime = ino->atime; st->st_mtime = ino->mtime; st->st_ctime = ino->ctime;
    // [M39] A stable unique inode id (the inode pointer) so userland tree-walks —
    // bash's getcwd compares (st_dev,st_ino) up the path — work. st_dev is a
    // single constant (one logical FS namespace to userland).
    st->st_dev = 1;
    st->st_ino = (uint64_t)(uintptr_t)ino;
    st->st_rdev = 0;
    st->st_blksize = 512;
    st->st_blocks = (int64_t)((ino->size + 511) / 512);
}

int ksys_stat(const char* path, struct secos_stat* st){
    char rp[256]; proc_resolve_path(sched_get_current(), path, rp, sizeof(rp)); path = rp; // [M39] cwd-relative
    int e=0; vfs_inode_t* ino=vfs_lookup_ex(path, 1, &e);   // symlinks followed in every component
    if(!ino) return e ? e : -2;                             // -ENOENT / -ENOTDIR / -ELOOP
    stat_fill(ino, st);
    return 0;
}

// [M26] lstat: like stat but does not follow a final symlink. vfs_lookup() does
// not auto-follow symlinks (the inode keeps type VFS_NODE_SYMLINK), so lstat is
// the plain lookup; stat() resolution of symlinks is layered in vfs (M26-3).
int ksys_lstat(const char* path, struct secos_stat* st){
    char rp[256]; proc_resolve_path(sched_get_current(), path, rp, sizeof(rp)); path = rp; // [M39] cwd-relative
    int e=0; vfs_inode_t* ino=vfs_lookup_ex(path, 0, &e);   // intermediate symlinks followed, final not
    if(!ino) return e ? e : -2;
    stat_fill(ino, st);
    return 0;
}

// [M26] chmod/chown/utimes — store-and-expose metadata (no multi-user enforce).
int ksys_chmod(const char* path, uint32_t mode){
    extern int vfs_setattr(const char*, const vfs_attr_t*, unsigned);
    vfs_attr_t a; a.mode=mode; return vfs_setattr(path, &a, VFS_ATTR_MODE);
}
int ksys_chown(const char* path, uint32_t uid, uint32_t gid){
    extern int vfs_setattr(const char*, const vfs_attr_t*, unsigned);
    vfs_attr_t a; a.uid=uid; a.gid=gid; return vfs_setattr(path, &a, VFS_ATTR_UID|VFS_ATTR_GID);
}
int ksys_utimes(const char* path, uint64_t atime, uint64_t mtime){
    extern int vfs_setattr(const char*, const vfs_attr_t*, unsigned);
    vfs_attr_t a; a.atime=atime; a.mtime=mtime; return vfs_setattr(path, &a, VFS_ATTR_ATIME|VFS_ATTR_MTIME);
}
int ksys_readlink(const char* path, char* buf, int len){
    extern int vfs_readlink(const char*, char*, size_t);
    if(len<=0) return -1; return vfs_readlink(path, buf, (size_t)len);
}
int ksys_symlink(const char* target, const char* linkpath){
    extern int vfs_symlink(const char*, const char*);
    return vfs_symlink(target, linkpath);
}

// [M31] Directory enumeration for a real userland (ls/opendir). Packs entries as
// fixed 256-byte records {char d_type; char d_name[255]} so the libc can iterate
// without parsing. d_type: 'd' dir, 'l' symlink, 'f' file.
struct gd_ctx { char* buf; int cap; int len; };
static void gd_cb(const vfs_inode_t* child, void* user){
    struct gd_ctx* g = (struct gd_ctx*)user;
    if(g->len + 256 > g->cap) return;
    const char* name = child->path; const char* p = child->path;
    while(*p){ if(*p=='/') name = p+1; p++; }
    char* e = g->buf + g->len;
    e[0] = (child->type==VFS_NODE_DIR) ? 'd' : (child->type==VFS_NODE_SYMLINK ? 'l' : 'f');
    int i=0; while(name[i] && i<254){ e[1+i]=name[i]; i++; } e[1+i]=0;
    g->len += 256;
}
int ksys_getdents(const char* path, void* ubuf, int buflen){
    extern int vfs_readdir(const char*, void(*)(const vfs_inode_t*, void*), void*);
    static char kbuf[65536];                 // up to 256 entries (BusyBox fills /bin)
    struct gd_ctx g = { kbuf, sizeof(kbuf), 0 };
    // cwd-relative + normalized: bash's glob and readline's filename completion
    // open "." and "dir/" (trailing slash), which vfs_readdir does not accept.
    char rp[256]; proc_resolve_path(sched_get_current(), path, rp, sizeof(rp)); path = rp;
    if(vfs_readdir(path, gd_cb, &g) != 0) return -1;
    int n = g.len < buflen ? g.len : buflen;
    if(n > 0 && copy_to_user(ubuf, kbuf, (size_t)n) != 0) return -1;
    return n;
}
int ksys_create(const char* path){
    extern int vfs_create(const char*, const void*, size_t);
    return vfs_create(path, "", 0);
}
int ksys_mkdir(const char* path){
    extern int vfs_mkdir(const char*);
    return vfs_mkdir(path);
}
int ksys_unlink(const char* path){
    extern int vfs_remove(const char*);
    // Drop the cached pages first: the FS may hand this inode slot (or inode
    // number) to the next file created.
    { vfs_inode_t* ino = vfs_lookup(path); if(ino && ino->type == VFS_NODE_FILE) pagecache_invalidate(ino, 0, 1ULL<<62); }
    return vfs_remove(path);
}

// [M26] mount/umount: map a block-device name + fstype to the existing FS mount
// helpers (the same path the boot mount and shell `mountdev` use). fstype:
// "fat32"/"vfat" -> FAT32; "ext2"/"ext4"/"extN" -> ext driver; "auto"/"" -> try
// both. The signature is the trust boundary, so any signed caller may mount.
static int str_eq2(const char* a, const char* b){ int i=0; while(a[i]&&b[i]){ if(a[i]!=b[i]) return 0; i++; } return a[i]==b[i]; }
int ksys_mount(const char* dev, const char* target, const char* fstype){
    extern void* block_find(const char* name);
    extern int fat32_mount(const char*, const char*);
    extern int ext2_mount(const char*, const char*);
    extern int exfat_mount(const char*, const char*);
    if(!dev||!target||!fstype) return -1;
    // Accept the devfs spelling ("/dev/vda") as well as the bare block name.
    if(dev[0]=='/'&&dev[1]=='d'&&dev[2]=='e'&&dev[3]=='v'&&dev[4]=='/') dev += 5;
    if(!block_find(dev)) return -1;                       // no such device
    int isfat = str_eq2(fstype,"fat32")||str_eq2(fstype,"vfat");
    int isext = str_eq2(fstype,"ext2")||str_eq2(fstype,"ext4")||str_eq2(fstype,"extN");
    int isexf = str_eq2(fstype,"exfat");
    int isauto= str_eq2(fstype,"auto")||fstype[0]==0;
    if(isfat) return fat32_mount(dev,target);
    if(isext) return ext2_mount(dev,target);
    if(isexf) return exfat_mount(dev,target);
    if(isauto){ if(fat32_mount(dev,target)==0) return 0; if(ext2_mount(dev,target)==0) return 0; return exfat_mount(dev,target); }
    return -1;                                            // unknown fstype
}
void proc_resolve_path(process_t* c, const char* in, char* out, int outsz);
int ksys_umount(const char* target){
    extern int vfs_unmount(const char*);
    // Normalize like every other path syscall ("/mnt/", "/mnt/.", relative
    // paths, "" -> cwd), so "/" cannot be reached under another spelling.
    char abs[256];
    proc_resolve_path(sched_get_current(), target, abs, sizeof(abs));
    // Flush the filesystem before detaching it (exFAT clears VolumeDirty here,
    // ext2/ext4 closes any open transaction), so a umount leaves it clean.
    {
        int n = vfs_mount_count();
        for(int i=0;i<n;i++){
            const char* mp=0; const char* fsn=0;
            if(vfs_mount_info(i,&mp,&fsn)==0 && mp && str_eq2(mp,abs)){ vfs_sync_ops(vfs_mount_ops_at(i)); break; }
        }
    }
    return vfs_unmount(abs);
}

// [M26] Copy a NUL-terminated user string into a kernel buffer (bounded,
// fault-tolerant via strncpy_from_user). 0 on success, -1 on fault/overflow.
static int copy_user_str(const char* u, char* k, int ksz){
    return strncpy_from_user(k, u, (size_t)ksz) < 0 ? -1 : 0;
}

// [M39] --- POSIX shell-from-source foundation ---
int ksys_dup2(int oldfd, int newfd){
    process_t* c=sched_get_current(); if(!c) return -1;
    if(oldfd<0||oldfd>=32||newfd<0||newfd>=32) return -1;
    if(!c->fds[oldfd].used) return -1;
    if(oldfd==newfd) return newfd;
    // Take the new reference before dropping the old target, so dup2 of a pty
    // or socket fd over another fd of the same object never hits zero.
    fd_entry_ref(&c->fds[oldfd]);
    struct proc_fd_entry old = c->fds[newfd];
    c->fds[newfd]=c->fds[oldfd]; c->fds[newfd].used=1; c->fds[newfd].cloexec=0; // POSIX: dup clears FD_CLOEXEC
    if(old.used) fd_entry_unref(&old);                         // close the old newfd target
    return newfd;
}
int ksys_dup(int oldfd){
    process_t* c=sched_get_current(); if(!c) return -1;
    if(oldfd<0||oldfd>=32||!c->fds[oldfd].used) return -1;
    int n=fd_alloc(c); if(n<0) return -1;
    c->fds[n]=c->fds[oldfd]; c->fds[n].used=1; c->fds[n].cloexec=0;
    fd_entry_ref(&c->fds[n]);
    return n;
}
// fcntl(fd, cmd, arg): descriptor flags (FD_CLOEXEC), status flags, and
// F_DUPFD / F_DUPFD_CLOEXEC honouring the minimum fd 'arg' (bash parks saved
// descriptors at fd >= 10 with close-on-exec).
#define K_F_DUPFD 0
#define K_F_GETFD 1
#define K_F_SETFD 2
#define K_F_GETFL 3
#define K_F_SETFL 4
#define K_F_DUPFD_CLOEXEC 1030
int ksys_fcntl(int fd, int cmd, long arg){
    process_t* c=sched_get_current(); if(!c) return -9;
    if(fd<0||fd>=32||!c->fds[fd].used) return -9;            // -EBADF
    switch(cmd){
    case K_F_DUPFD: case K_F_DUPFD_CLOEXEC: {
        if(arg<0||arg>=32) return -22;                       // -EINVAL
        int n=-1; for(int i=(int)arg;i<32;i++) if(!c->fds[i].used){ n=i; break; }
        if(n<0) return -24;                                  // -EMFILE
        c->fds[n]=c->fds[fd]; c->fds[n].used=1;
        c->fds[n].cloexec = (cmd==K_F_DUPFD_CLOEXEC) ? 1 : 0;
        fd_entry_ref(&c->fds[n]);
        return n;
    }
    case K_F_GETFD: return c->fds[fd].cloexec ? 1 : 0;      // FD_CLOEXEC = 1
    case K_F_SETFD: c->fds[fd].cloexec = (arg & 1) ? 1 : 0; return 0;
    case K_F_GETFL:
        if(c->fds[fd].is_pipe) return (c->fds[fd].pipe_w ? 1 /*O_WRONLY*/ : 0 /*O_RDONLY*/) | (int)(c->fds[fd].flags & 0x800);
        if(fd<3 && !c->fds[fd].inode) return 2;              // console stdio: O_RDWR
        return (int)(c->fds[fd].flags & ~K_O_CLOEXEC);
    case K_F_SETFL:                                          // only O_APPEND/O_NONBLOCK change
        c->fds[fd].flags = (c->fds[fd].flags & ~(0x400u|0x800u)) | ((uint32_t)arg & (0x400u|0x800u));
        return 0;
    default: return -22;
    }
}
int ksys_chdir(const char* upath){
    process_t* c=sched_get_current(); if(!c) return -1;
    char k[256]; if(copy_user_str(upath, k, sizeof(k))!=0) return -1;
    char abs[256]; proc_resolve_path(c, k, abs, sizeof(abs));
    extern vfs_inode_t* vfs_lookup_follow(const char*);
    vfs_inode_t* ino=vfs_lookup_follow(abs);
    if(!ino || ino->type != VFS_NODE_DIR) return -1;            // must be an existing directory
    int i=0; for(; abs[i] && i<255; i++) c->cwd[i]=abs[i]; c->cwd[i]=0;
    return 0;
}
int ksys_getcwd(char* ubuf, int len){
    process_t* c=sched_get_current(); if(!c) return -1;
    int n=0; while(c->cwd[n]) n++;
    if(len < n+1 || !user_range_valid(ubuf,(size_t)(n+1))) return -1;
    if(copy_to_user(ubuf, c->cwd, (size_t)(n+1))!=0) return -1;
    return n;
}
int ksys_getppid(void){ process_t* c=sched_get_current(); return c? (int)c->ppid : 0; }

// [M39] fstat: stat an open fd's inode so userland gets the CORRECT file type
// (a libc stub guessing S_IFREG broke bash, which opens "/" and must see S_IFDIR).
int ksys_fstat(int fd, struct secos_stat* st){
    process_t* c=sched_get_current(); if(!c||!st) return -1;
    if(fd<0||fd>=32||!c->fds[fd].used) return -1;
    for(unsigned i=0;i<sizeof(*st);i++) ((char*)st)[i]=0;
    vfs_inode_t* ino=(vfs_inode_t*)c->fds[fd].inode;
    if(c->fds[fd].is_sock){                     // socket
        st->st_mode = 0xC000 /*S_IFSOCK*/ | 0600; st->st_nlink=1; st->st_dev=2; st->st_ino=(uint64_t)c->fds[fd].is_sock;
        return 0;
    }
    if(!ino || c->fds[fd].is_pipe){             // console / pipe end: a char device
        st->st_mode = S_IFCHR | 0620; st->st_nlink=1; st->st_dev=1; st->st_ino=(uint64_t)(uintptr_t)&c->fds[fd];
        return 0;
    }
    stat_fill(ino, st);
    return 0;
}

/* Terminal ioctls. termios, window size and the input-queue requests are the
 * console TTY's (kernel/tty.c, which really applies them: canonical/raw mode,
 * echo, ISIG, VMIN/VTIME, ...); the job-control requests live here, next to
 * the process-group checks. */
#define TIOCGPGRP_  0x540F
#define TIOCSPGRP_  0x5410
#define TIOCGSID_   0x5429
int ksys_ioctl(int fd, uint64_t cmd, uint64_t arg){
    process_t* c=sched_get_current(); if(!c) return -1;
    if(fd<0||fd>=32) return -1;
    // FIONBIO (ioctl form of fcntl O_NONBLOCK) on a terminal fd.
    #define FD_SET_NONBLOCK() do { int v_; \
        if(copy_from_user(&v_,(void*)arg,sizeof(v_))!=0) return -EFAULT; \
        if(v_) c->fds[fd].flags |= 0x800; else c->fds[fd].flags &= ~0x800u; return 0; } while(0)
    // A pty end: everything (termios, winsize, job control, sessions, ptmx
    // requests) goes to kernel/pty.c, against that pty's own line discipline.
    if(c->fds[fd].used && !c->fds[fd].is_pipe && pty_is_inode(c->fds[fd].inode)){
        if(cmd == TTY_FIONBIO) FD_SET_NONBLOCK();
        int r = pty_ioctl(c, c->fds[fd].inode, cmd, arg);
        return r == -515 ? -22 : r;             // unknown request: -EINVAL
    }
    // [M40] evdev client (EVIOCG*) and /dev/fb0 (FBIO*).
    if(c->fds[fd].used && !c->fds[fd].is_pipe && !c->fds[fd].is_sock && c->fds[fd].inode){
        extern int devfs_is_fb(void* inode);
        if(input_is_client(c->fds[fd].inode)){
            if(cmd == TTY_FIONBIO) FD_SET_NONBLOCK();
            return input_ioctl(c->fds[fd].inode, cmd, arg);
        }
        if(devfs_is_fb(c->fds[fd].inode)){
            if(cmd == TTY_FIONBIO) FD_SET_NONBLOCK();
            int r = fbdev_ioctl(cmd, arg);
            return r == -515 ? -25 : r;         // not an fb request: -ENOTTY
        }
    }
    // The console TTY: fds 0/1/2 when not redirected, any dup of a console fd
    // (inode==NULL, not a pipe), and an open /dev/tty or /dev/console. bash
    // keeps its terminal fd as dup(2) moved to a high fd and does
    // tcgetpgrp/tcsetpgrp on it.
    extern int devfs_is_console(void* inode);
    int is_tty = (fd<=2 && !c->fds[fd].used) ||
                 (c->fds[fd].used && !c->fds[fd].inode && !c->fds[fd].is_pipe && !c->fds[fd].is_sock) ||
                 (c->fds[fd].used && !c->fds[fd].is_pipe && !c->fds[fd].is_sock && devfs_is_console(c->fds[fd].inode));
    switch(cmd){
    case TIOCGSID_: {
        if(!is_tty) return -25;                 // -ENOTTY
        int sid = (int)c->sid;
        if(copy_to_user((void*)arg,&sid,sizeof(sid))!=0) return -EFAULT;
        return 0;
    }
    // Sessions: the console is the controlling terminal of every session that
    // has not detached from it (setsid/TIOCNOTTY) or acquired a pty.
    case TTY_TIOCSCTTY:
        if(!is_tty) return -25;
        return pty_console_sctty(c);
    case TTY_TIOCNOTTY:
        if(!is_tty) return -25;
        return pty_console_notty(c);
    case TTY_FIONBIO:
        if(!is_tty) return -25;
        FD_SET_NONBLOCK();
    // Job control: the console's foreground process group is the kernel's
    // foreground_pgid (Ctrl-C/Ctrl-Z target, SIGTTIN check in tty_read). With
    // no foreground group set, the caller's own group is reported (so a shell
    // started outside job control sees itself in the foreground).
    case TIOCGPGRP_: {
        if(!is_tty) return -25;                 // -ENOTTY
        extern uint32_t signal_get_foreground_pgid(void);
        uint32_t fg = signal_get_foreground_pgid();
        int pg = (int)(fg ? fg : c->pgid);
        if(copy_to_user((void*)arg,&pg,sizeof(pg))!=0) return -EFAULT;
        return 0;
    }
    case TIOCSPGRP_: {
        if(!is_tty) return -25;                 // -ENOTTY
        extern void signal_set_foreground_pgid(uint32_t);
        int pg;
        if(copy_from_user(&pg,(void*)arg,sizeof(pg))!=0) return -EFAULT;
        if(pg <= 0) return -22;                // -EINVAL
        extern int signal_pgid_exists(uint32_t);
        if(!signal_pgid_exists((uint32_t)pg)) return -1;   // -EPERM: no such group
        signal_set_foreground_pgid((uint32_t)pg);
        return 0;
    }
    default: {
        if(!is_tty) return -25;                 // -ENOTTY
        extern int tty_ioctl(uint64_t cmd, uint64_t arg);
        int r = tty_ioctl(cmd, arg);
        return r == -515 ? -22 : r;             // unknown request: -EINVAL
    }
    }
}

uint64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4){
    switch(num){
    case SYS_GETPID:
        return (uint64_t)ksys_getpid();

    case SYS_EXIT:
        ksys_exit((int)a0);
        return 0;

    case SYS_CLOSE:
        return (uint64_t)ksys_close((int)a0);

    case SYS_OPEN: {
        /* [M35] A write-intent open needs FS_WRITE; otherwise FS_READ. */
        CAP_GATE(((int)a1 & 0x3) ? CAP_FS_WRITE : CAP_FS_READ, "open");
        /* [M3] Validate path pointer is in user range (at least 1 byte). */
        char kp[256];
        long sl = strncpy_from_user(kp, (const char*)a0, sizeof(kp));
        if (sl < 0) return (uint64_t)(int64_t)sl;      /* -EFAULT / -ENAMETOOLONG */
        return (uint64_t)(int64_t)ksys_open_mode(kp, (int)a1, (uint32_t)a2);
    }

    case SYS_READ: {
        /* [M3] Validate buf + len entirely within user range. */
        void* buf = (void*)a1;
        int   len = (int)a2;
        if (len <= 0 || !user_range_valid(buf, (size_t)len)) {
            terminal_writestring("[SYSCALL][M3] SYS_READ: invalid user buffer\n");
            return (uint64_t)(int64_t)-EFAULT;
        }
        return (uint64_t)ksys_read((int)a0, buf, len);
    }

    case SYS_WRITE: {
        /* [M3] Validate buf + len entirely within user range. */
        const void* buf = (const void*)a1;
        int         len = (int)a2;
        if (len <= 0 || !user_range_valid(buf, (size_t)len)) {
            terminal_writestring("[SYSCALL][M3] SYS_WRITE: invalid user buffer\n");
            return (uint64_t)(int64_t)-EFAULT;
        }
        return (uint64_t)ksys_write((int)a0, buf, len);
    }

    case SYS_SPAWN: {
        CAP_GATE(CAP_PROC, "spawn");     // [M35]
        /* [M3] Validate and copy the user path string into the kernel. */
        char kpath[256];
        long sl = strncpy_from_user(kpath, (const char*)a0, sizeof(kpath));
        if (sl < 0) return (uint64_t)(int64_t)sl;      /* -EFAULT / -ENAMETOOLONG */
        /* [M16] Optional argv: a1 = user char** (NULL-terminated array of user
         * char*), or NULL for no args. Copy both the pointer array and each
         * string into the kernel (all user accesses validated/bounce-buffered). */
        const char* const* uargv = (const char* const*)a1;
        static char argstore[SPAWN_MAX_ARGS][SPAWN_ARG_LEN];
        const char* kargv[SPAWN_MAX_ARGS];
        int argc = 0;
        if (uargv) {
            for (argc = 0; argc < SPAWN_MAX_ARGS; argc++) {
                const char* ustr = 0;
                if (copy_from_user(&ustr, uargv + argc, sizeof(ustr)) != 0)
                    return (uint64_t)(int64_t)-EFAULT;
                if (!ustr) break; /* NULL terminator */
                /* Over-long args are truncated (historical behaviour). */
                long al = strncpy_from_user(argstore[argc], ustr, SPAWN_ARG_LEN);
                if (al == -EFAULT) return (uint64_t)(int64_t)-EFAULT;
                kargv[argc] = argstore[argc];
            }
        }
        return (uint64_t)(int64_t)ksys_spawn_argv(kpath, argc, kargv);
    }

    case SYS_GETTICKS:
        /* [M13] Uptime in timer ticks. No user pointers involved. */
        return timer_get_ticks();

    case SYS_MSG_SEND: {
        CAP_GATE(CAP_IPC, "msg_send");   // [M35]
        /* [M13] (a0=chan, a1=buf, a2=len) -> kernel IPC channel. */
        int chan = (int)a0; const void* ubuf = (const void*)a1; int len = (int)a2;
        if (len <= 0 || len > 256 || !user_range_valid(ubuf, (size_t)len))
            return (uint64_t)(int64_t)-EFAULT;
        uint8_t kbuf[256];
        if (copy_from_user(kbuf, ubuf, (size_t)len) != 0)
            return (uint64_t)(int64_t)-EFAULT;
        int sent = ipc_send(chan, kbuf, len);
        if (sent > 0) { extern void sched_wake_chan(int); sched_wake_chan(chan); } // [M17] wake blocked receivers
        return (uint64_t)(int64_t)sent;
    }

    /* [M16] SYS_WAIT and [M17] SYS_MSG_RECV / SYS_SLEEP are handled in
     * syscall_trap.c (they may block the caller and need the trapframe). */

    case SYS_BRK:
        return ksys_brk(a0);
    case SYS_MMAP:
        return ksys_mmap(a0, a1, (int)a2, (int)a3, (int)a4);
    case SYS_MUNMAP:
        return (uint64_t)(int64_t)ksys_munmap(a0, a1);
    case SYS_MPROTECT:
        return (uint64_t)(int64_t)ksys_mprotect(a0, a1, (int)a2);

    case SYS_LSEEK:
        return (uint64_t)(int64_t)ksys_lseek((int)a0, (long)a1, (int)a2);

    case SYS_STAT: {
        char kp[256];
        long sl = strncpy_from_user(kp, (const char*)a0, sizeof(kp));
        if (sl < 0) return (uint64_t)(int64_t)sl;
        struct secos_stat st;
        int r = ksys_stat(kp, &st);
        if (r != 0) return (uint64_t)(int64_t)r;
        if (copy_to_user((void*)a1, &st, sizeof(st)) != 0) return (uint64_t)(int64_t)-EFAULT;
        return 0;
    }

    case SYS_PIPE: {
        CAP_GATE(CAP_IPC, "pipe");       // [M35]
        /* [M25] pipe(int fds[2]): kernel-side alloc, then copy the two fds out. */
        int kfds[2];
        int r = ksys_pipe2(kfds, (int)a1);   // a1 = pipe2() flags (0 for pipe())
        if (r != 0) return (uint64_t)(int64_t)r;
        if (copy_to_user((void*)a0, kfds, sizeof(kfds)) != 0) return (uint64_t)(int64_t)-EFAULT;
        return 0;
    }

    case SYS_LSTAT: {
        char kp[256]; if (copy_user_str((const char*)a0, kp, sizeof(kp)) != 0) return (uint64_t)(int64_t)-EFAULT;
        struct secos_stat st; int r = ksys_lstat(kp, &st);
        if (r != 0) return (uint64_t)(int64_t)r;
        if (copy_to_user((void*)a1, &st, sizeof(st)) != 0) return (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    case SYS_CHMOD: {
        CAP_GATE(CAP_FS_WRITE, "chmod");  // [M35]
        char kq[256]; if (copy_user_str((const char*)a0, kq, sizeof(kq)) != 0) return (uint64_t)(int64_t)-EFAULT;
        char kp[256]; proc_resolve_path(sched_get_current(), kq, kp, sizeof(kp));   // cwd-relative
        return (uint64_t)(int64_t)ksys_chmod(kp, (uint32_t)a1);
    }
    case SYS_CHOWN: {
        CAP_GATE(CAP_FS_WRITE, "chown");  // [M35]
        char kq[256]; if (copy_user_str((const char*)a0, kq, sizeof(kq)) != 0) return (uint64_t)(int64_t)-EFAULT;
        char kp[256]; proc_resolve_path(sched_get_current(), kq, kp, sizeof(kp));   // cwd-relative
        return (uint64_t)(int64_t)ksys_chown(kp, (uint32_t)a1, (uint32_t)a2);
    }
    case SYS_UTIMES: {
        CAP_GATE(CAP_FS_WRITE, "utimes"); // [M35]
        char kq[256]; if (copy_user_str((const char*)a0, kq, sizeof(kq)) != 0) return (uint64_t)(int64_t)-EFAULT;
        char kp[256]; proc_resolve_path(sched_get_current(), kq, kp, sizeof(kp));   // cwd-relative
        return (uint64_t)(int64_t)ksys_utimes(kp, (uint64_t)a1, (uint64_t)a2);
    }
    case SYS_READLINK: {
        char kq[256]; if (copy_user_str((const char*)a0, kq, sizeof(kq)) != 0) return (uint64_t)(int64_t)-EFAULT;
        char kp[256]; proc_resolve_path(sched_get_current(), kq, kp, sizeof(kp));   // cwd-relative
        int len = (int)a2; if (len <= 0 || len > 4096 || !user_range_valid((void*)a1, (size_t)len)) return (uint64_t)(int64_t)-EFAULT;
        char kbuf[256]; int klen = len < (int)sizeof(kbuf) ? len : (int)sizeof(kbuf);
        int n = ksys_readlink(kp, kbuf, klen);
        if (n < 0) return (uint64_t)(int64_t)n;
        if (copy_to_user((void*)a1, kbuf, (size_t)n) != 0) return (uint64_t)(int64_t)-EFAULT;
        return (uint64_t)(int64_t)n;
    }
    case SYS_SYMLINK: {
        CAP_GATE(CAP_FS_WRITE, "symlink"); // [M35]
        char ktarget[256], klink[256];
        if (copy_user_str((const char*)a0, ktarget, sizeof(ktarget)) != 0) return (uint64_t)(int64_t)-EFAULT;
        if (copy_user_str((const char*)a1, klink, sizeof(klink)) != 0) return (uint64_t)(int64_t)-EFAULT;
        char kl[256]; proc_resolve_path(sched_get_current(), klink, kl, sizeof(kl));   // cwd-relative
        return (uint64_t)(int64_t)ksys_symlink(ktarget, kl);
    }
    case SYS_MOUNT: {
        CAP_GATE(CAP_FS_WRITE, "mount");  // [M35]
        char kdev[64], ktgt[256], kfs[32];
        if (copy_user_str((const char*)a0, kdev, sizeof(kdev)) != 0) return (uint64_t)(int64_t)-EFAULT;
        if (copy_user_str((const char*)a1, ktgt, sizeof(ktgt)) != 0) return (uint64_t)(int64_t)-EFAULT;
        if (copy_user_str((const char*)a2, kfs,  sizeof(kfs))  != 0) return (uint64_t)(int64_t)-EFAULT;
        return (uint64_t)(int64_t)ksys_mount(kdev, ktgt, kfs);
    }
    case SYS_UMOUNT: {
        CAP_GATE(CAP_FS_WRITE, "umount"); // [M35]
        char ktgt[256]; if (copy_user_str((const char*)a0, ktgt, sizeof(ktgt)) != 0) return (uint64_t)(int64_t)-EFAULT;
        return (uint64_t)(int64_t)ksys_umount(ktgt);
    }
    case SYS_GETDENTS: {
        CAP_GATE(CAP_FS_READ, "getdents"); // [M35]
        extern int ksys_getdents(const char*, void*, int);
        char kq[256]; if (copy_user_str((const char*)a0, kq, sizeof(kq)) != 0) return (uint64_t)(int64_t)-EFAULT;
        char kp[256]; proc_resolve_path(sched_get_current(), kq, kp, sizeof(kp));   // cwd-relative
        int len = (int)a2; if (len <= 0 || len > (1<<20) || !user_range_valid((void*)a1, (size_t)len)) return (uint64_t)(int64_t)-EFAULT;
        return (uint64_t)(int64_t)ksys_getdents(kp, (void*)a1, len);
    }
    case SYS_CREATE: {
        CAP_GATE(CAP_FS_WRITE, "create"); // [M35]
        extern int ksys_create(const char*);
        char kq[256]; if (copy_user_str((const char*)a0, kq, sizeof(kq)) != 0) return (uint64_t)(int64_t)-EFAULT;
        char kp[256]; proc_resolve_path(sched_get_current(), kq, kp, sizeof(kp));   // cwd-relative
        return (uint64_t)(int64_t)ksys_create(kp);
    }
    case SYS_MKDIR: {
        CAP_GATE(CAP_FS_WRITE, "mkdir");  // [M35]
        extern int ksys_mkdir(const char*);
        extern void kiface_apply_create_mode(const char*, uint32_t, int);
        char kp[256]; if (copy_user_str((const char*)a0, kp, sizeof(kp)) != 0) return (uint64_t)(int64_t)-EFAULT;
        char ab[256]; proc_resolve_path(sched_get_current(), kp, ab, sizeof(ab));   // cwd-relative
        int r = ksys_mkdir(ab);
        if (r == 0) kiface_apply_create_mode(ab, (uint32_t)a1, 1);   // mode & ~umask
        return (uint64_t)(int64_t)r;
    }
    case SYS_UNLINK: {
        CAP_GATE(CAP_FS_WRITE, "unlink"); // [M35]
        extern int ksys_unlink(const char*);
        char kq[256]; if (copy_user_str((const char*)a0, kq, sizeof(kq)) != 0) return (uint64_t)(int64_t)-EFAULT;
        char kp[256]; proc_resolve_path(sched_get_current(), kq, kp, sizeof(kp));   // cwd-relative
        return (uint64_t)(int64_t)ksys_unlink(kp);
    }

    /* [M24] Sockets — gated by CAP_NET in the signed manifest. A socket is an
     * ordinary fd (fds[fd].is_sock = socket index + 1): read/write/close/dup/
     * fork/poll work on it. The calls below are non-blocking at this level;
     * -EAGAIN on a blocking fd is turned into a sleep + re-run by
     * kernel/syscall_trap.c (no ring-0 spinning). connect() still spins for
     * the TCP handshake. */
    case SYS_SOCKET: case SYS_CONNECT: case SYS_BIND: case SYS_LISTEN:
    case SYS_ACCEPT: case SYS_SEND: case SYS_RECV: case SYS_SENDTO:
    case SYS_RECVFROM: case SYS_SOCKCLOSE: case SYS_SOCKNAME: {
        process_t* c = sched_get_current();
        if (!c || !c->cap_net) {
            debugcon_writestring("[NET] socket syscall denied: no CAP_NET\n");
            return (uint64_t)(int64_t)-1;            /* EPERM */
        }
        if (num == SYS_SOCKET) {
            int type = (int)(a0 & 0xff);
            int sx = socket_create(type);
            if (sx < 0) return (uint64_t)(int64_t)sx;
            int fd = fd_alloc(c);
            if (fd < 0) { socket_unref(sx); return (uint64_t)(int64_t)-24; }   /* -EMFILE */
            c->fds[fd].is_sock = sx + 1;
            c->fds[fd].flags = (uint32_t)(a0 & 0x800);                         /* SOCK_NONBLOCK */
            return (uint64_t)(int64_t)fd;
        }
        int fd = (int)a0;
        if (fd < 0 || fd >= 32 || !c->fds[fd].used) return (uint64_t)(int64_t)-9;       /* -EBADF */
        if (!c->fds[fd].is_sock) return (uint64_t)(int64_t)-88;                           /* -ENOTSOCK */
        int sx = c->fds[fd].is_sock - 1;
        switch (num) {
        case SYS_SOCKCLOSE:
            return (uint64_t)(int64_t)ksys_close(fd);
        case SYS_CONNECT:
            return (uint64_t)(int64_t)socket_connect(sx, (uint32_t)a1, (uint16_t)a2);
        case SYS_BIND:
            return (uint64_t)(int64_t)socket_bind(sx, (uint16_t)a1);
        case SYS_LISTEN:
            return (uint64_t)(int64_t)socket_listen(sx, (int)a1);
        case SYS_ACCEPT: {
            int nfd = fd_alloc(c);
            if (nfd < 0) return (uint64_t)(int64_t)-24;                                  /* -EMFILE */
            int nsx = socket_accept(sx);
            if (nsx < 0) { c->fds[nfd].used = 0; return (uint64_t)(int64_t)nsx; }      /* -EAGAIN: re-run */
            c->fds[nfd].is_sock = nsx + 1;
            return (uint64_t)(int64_t)nfd;
        }
        case SYS_SEND:
            return (uint64_t)(int64_t)ksys_write(fd, (const void*)a1, (int)a2);
        case SYS_RECV:
            return (uint64_t)(int64_t)ksys_read(fd, (void*)a1, (int)a2);
        case SYS_SENDTO: {
            uint8_t netbuf[1024];                     /* per call: no shared static (SMP) */
            int len = (int)a2;
            if (len < 0 || len > (int)sizeof(netbuf)) return (uint64_t)(int64_t)-90;    /* -EMSGSIZE */
            struct secos_sockaddr sa;
            if (copy_from_user(&sa, (void*)a3, sizeof(sa)) != 0) return (uint64_t)(int64_t)-EFAULT;
            if (len && copy_from_user(netbuf, (void*)a1, (size_t)len) != 0) return (uint64_t)(int64_t)-EFAULT;
            return (uint64_t)(int64_t)socket_sendto(sx, netbuf, len, sa.ip, sa.port);
        }
        case SYS_RECVFROM: {
            uint8_t netbuf[1024];
            int len = (int)a2;
            if (len <= 0) return (uint64_t)(int64_t)-22;
            if (len > (int)sizeof(netbuf)) len = (int)sizeof(netbuf);
            uint32_t sip = 0; uint16_t sport = 0;
            int n = socket_recv(sx, netbuf, len, &sip, &sport);
            if (n > 0 && copy_to_user((void*)a1, netbuf, (size_t)n) != 0) return (uint64_t)(int64_t)-EFAULT;
            if (n >= 0 && a3) {
                struct secos_sockaddr sa; sa.ip = sip; sa.port = sport; sa._pad = 0;
                if (copy_to_user((void*)a3, &sa, sizeof(sa)) != 0) return (uint64_t)(int64_t)-EFAULT;
            }
            return (uint64_t)(int64_t)n;
        }
        case SYS_SOCKNAME: {
            struct secos_sockaddr sa; sa._pad = 0;
            int r = socket_name(sx, a2 ? 1 : 0, &sa.ip, &sa.port);
            if (r < 0) return (uint64_t)(int64_t)r;
            if (copy_to_user((void*)a1, &sa, sizeof(sa)) != 0) return (uint64_t)(int64_t)-EFAULT;
            return 0;
        }
        }
        return (uint64_t)(int64_t)-1;
    }

    case SYS_DRIVER: {
        /*
         * [M3] SYS_DRIVER hardening:
         *  1. copy_from_user: copy driver_call_t from user to kernel buffer.
         *  2. Execute with kernel-owned copy (no user pointer in driver_if).
         *  3. copy_to_user: write value field back for READ_REG / GET_INFO.
         */
        driver_call_t kdc;
        if (copy_from_user(&kdc, (const void*)a0, sizeof(kdc)) != 0) {
            terminal_writestring("[SYSCALL][M3] SYS_DRIVER: invalid user struct pointer\n");
            return (uint64_t)(int64_t)-EFAULT;
        }
        int r = driver_syscall(&kdc);
        /* Write value field back for read-type opcodes. */
        if (r == DRV_OK &&
            (kdc.opcode == DRIVER_OP_READ_REG || kdc.opcode == DRIVER_OP_GET_INFO)) {
            /* Pointer was already validated by copy_from_user above. */
            copy_to_user((void*)((uint64_t)a0 + __builtin_offsetof(driver_call_t, value)),
                         &kdc.value, sizeof(kdc.value));
        }
        return (uint64_t)(int64_t)r;
    }

    /* [M30] Signals + job control. SYS_SIGRETURN is handled in syscall_trap.c
     * (it needs the live trapframe); the rest are plain dispatches. */
    case SYS_SIGACTION: {
        extern long ksys_sigaction(int, uint64_t, uint64_t);
        return (uint64_t)(int64_t)ksys_sigaction((int)a0, a1, a2);
    }
    case SYS_KILL: {
        CAP_GATE(CAP_SIGNAL, "kill");     // [M35]
        extern long ksys_kill(int, int);
        return (uint64_t)(int64_t)ksys_kill((int)a0, (int)a1);
    }
    case SYS_SIGPROCMASK: {
        extern long ksys_sigprocmask(int, uint64_t, uint64_t*);
        return (uint64_t)(int64_t)ksys_sigprocmask((int)a0, a1, (uint64_t*)a2);
    }
    case SYS_SETPGID: {
        extern long ksys_setpgid(int, int);
        return (uint64_t)(int64_t)ksys_setpgid((int)a0, (int)a1);
    }

    // [M39] POSIX shell-from-source foundation.
    case SYS_DUP2:    return (uint64_t)(int64_t)ksys_dup2((int)a0, (int)a1);
    case SYS_DUP:     return (uint64_t)(int64_t)ksys_dup((int)a0);
    case SYS_CHDIR:   return (uint64_t)(int64_t)ksys_chdir((const char*)a0);
    case SYS_GETCWD:  return (uint64_t)(int64_t)ksys_getcwd((char*)a0, (int)a1);
    case SYS_IOCTL:   return (uint64_t)(int64_t)ksys_ioctl((int)a0, a1, a2);
    case SYS_GETPPID: return (uint64_t)ksys_getppid();
    case SYS_FCNTL:   return (uint64_t)(int64_t)ksys_fcntl((int)a0, (int)a1, (long)a2);
    case SYS_GETPGID: {                       // job control (bash getpgrp/getpgid)
        extern long ksys_getpgid(int);
        return (uint64_t)(int64_t)ksys_getpgid((int)a0);
    }
    // Sessions (pseudo-terminal support).
    case SYS_SETSID:  return (uint64_t)(int64_t)ksys_setsid();
    case SYS_GETSID:  return (uint64_t)(int64_t)ksys_getsid((int)a0);
    case SYS_NETCTL: {                        // NIC config + DHCP from ring 3 (CAP_NET)
        extern long ksys_netctl(uint64_t, uint64_t, uint64_t);
        return (uint64_t)(int64_t)ksys_netctl(a0, a1, a2);
    }

    case SYS_FSTAT: {
        struct secos_stat kst;
        if(!user_range_valid((void*)a1, sizeof(kst))) return (uint64_t)(int64_t)-EFAULT;
        int r = ksys_fstat((int)a0, &kst);
        if(r != 0) return (uint64_t)(int64_t)r;
        if(copy_to_user((void*)a1, &kst, sizeof(kst)) != 0) return (uint64_t)(int64_t)-EFAULT;
        return 0;
    }

    default:
        terminal_writestring("[SYSCALL] sconosciuta\n");
        return (uint64_t)(int64_t)-1;
    }
}
