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
#include "udrv.h"             // [M57] ring-3 driver claims (SYS_UDRV, claim fds)
#include "percpu.h"           // SMP_MAX_CPUS, smp_cpu_count (sched_getaffinity)

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

// [M51 C2] The owner is the only writer of its fd table and cwd, but procfs
// (another process's /proc/<pid>/fd, cwd) and the procfs/sysfs inode-pool
// busy checks read them from other CPUs. Every store that changes what an fd
// is (used/inode/is_pipe/pipe_w/is_sock) or the cwd is made under the owner's
// mm_lock, which those readers take; the object references (pipe/socket/pty/
// evdev) are taken and dropped outside it (their locks rank below S35).
// offset/flags/cloexec are read by nobody else and stay unlocked.
static inline uint64_t fdt_lock(process_t* p){ return mm_lock(p); }
static inline void fdt_unlock(process_t* p, uint64_t fl){ mm_unlock(p, fl); }
static int fd_alloc(process_t* p){
    uint64_t fl = fdt_lock(p);
    for(int i=0;i<32;i++){ if(!p->fds[i].used){ p->fds[i].used=1; p->fds[i].offset=0; p->fds[i].flags=0; p->fds[i].inode=NULL; p->fds[i].is_pipe=0; p->fds[i].pipe_w=0; p->fds[i].cloexec=0; p->fds[i].is_sock=0; fdt_unlock(p, fl); return i; } }
    fdt_unlock(p, fl);
    return -1;
}
// [M57] Install a kernel object (an inode-tagged fd kind: a udrv claim fd) as
// a new fd of c. Returns the fd or -1 (table full).
int ksys_fd_install(process_t* c, void* ino, uint32_t flags, int cloexec){
    int fd = fd_alloc(c);
    if (fd < 0) return -1;
    uint64_t fl = fdt_lock(c);
    c->fds[fd].inode = ino; c->fds[fd].flags = flags; c->fds[fd].cloexec = cloexec ? 1 : 0;
    fdt_unlock(c, fl);
    return fd;
}
// Release an fd slot fd_alloc reserved (error paths).
static void fd_release(process_t* p, int fd){ uint64_t fl = fdt_lock(p); p->fds[fd].used=0; p->fds[fd].inode=NULL; fdt_unlock(p, fl); }

// Take / drop the extra reference a duplicated fd entry holds on its object
// (pipe end, socket, pty end or evdev client; pty_ref/pty_unref and
// input_obj_ref/unref ignore any inode that is not theirs).
// Used by dup/dup2/fcntl(F_DUPFD)/spawn stdio inheritance and close.
static void fd_entry_ref(struct proc_fd_entry* e){
    if(e->is_pipe) pipe_ref((pipe_t*)e->inode, e->pipe_w);
    else if(e->is_sock) socket_ref(e->is_sock - 1);
    else { pty_ref(e->inode); input_obj_ref(e->inode); udrv_fd_ref(e->inode); }
}
static void fd_entry_unref(struct proc_fd_entry* e){
    if(e->is_pipe){ pipe_unref((pipe_t*)e->inode, e->pipe_w); e->is_pipe=0; }
    else if(e->is_sock){ int sx=e->is_sock-1; e->is_sock=0; socket_unref(sx); }
    else { void* ino=e->inode; e->inode=NULL; pty_unref(ino); input_obj_unref(ino); udrv_fd_unref(ino); }   // a pty end may hang up
}

// [M25] pipe(): allocate a pipe object and two fds (read end + write end).
// pipe2(O_CLOEXEC) marks both ends close-on-exec (ksys_pipe2).
int ksys_pipe(int kfds[2]){
    extern process_t* sched_get_current(void); process_t* c=sched_get_current(); if(!c) return -1;
    pipe_t* p = pipe_alloc(); if(!p) return -1;
    int rfd=fd_alloc(c); if(rfd<0){ pipe_unref(p,0); pipe_unref(p,1); return -1; }
    int wfd=fd_alloc(c); if(wfd<0){ fd_release(c, rfd); pipe_unref(p,0); pipe_unref(p,1); return -1; }
    uint64_t fl = fdt_lock(c);
    c->fds[rfd].inode=p; c->fds[rfd].is_pipe=1; c->fds[rfd].pipe_w=0;  // read end
    c->fds[wfd].inode=p; c->fds[wfd].is_pipe=1; c->fds[wfd].pipe_w=1;  // write end
    fdt_unlock(c, fl);
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
// [M16] Spawn with argv.
// [M51-C6] Each spawn reads the file into its own kmalloc'd buffer, which the
// new process takes over as its pinned image (process_create_from_elf_owned).
// This replaces the static 2 MB g_spawn_buf (B5): with a shared buffer two
// CPUs spawning at once would overwrite each other's image between the
// signature check and the pin, and serializing them on an exec_lock would also
// serialize the Ed25519 verify. Memory: a spawn holds at most one image of at
// most SPAWN_MAX_IMAGE, never across a block (kernel code is not preemptible),
// so at most one staging image per CPU is in flight, and it IS the pinned copy
// the old code made anyway; the 2 MB of BSS is gone.
#define SPAWN_MAX_IMAGE (16u * 1024u * 1024u)    // = EXEC_MAX_IMAGE (execve)

// Read the whole regular file behind 'ino' into a new kmalloc'd buffer.
// Returns the byte count (> 0) and the buffer in *out, or -1 (nothing
// allocated). The caller pins the inode's mount.
static int spawn_read_image(vfs_inode_t* ino, uint8_t** out){
    *out = 0;
    if(!ino || ino->type != VFS_NODE_FILE || !ino->ops || !ino->ops->read) return -1;
    size_t size = ino->size;
    if(size == 0 || size > SPAWN_MAX_IMAGE) return -1;
    uint8_t* buf = (uint8_t*)kmalloc(size);
    if(!buf) return -1;
    int r = ino->ops->read(ino, 0, buf, size);
    // A short read (the file shrank under us) loads what was read: the
    // signature check decides whether that is a valid image.
    if(r <= 0 || (size_t)r > size){ kfree(buf); return -1; }
    *out = buf;
    return r;
}
// [M39] A spawned child inherits the caller's stdio (fds 0/1/2), like a
// fork+execve child would, so a spawned command whose caller redirected stdout
// into a pipe or a file writes there and not to the console. Only 0/1/2 are
// passed (spawn is not fork: the caller's other fds stay private), always without
// FD_CLOEXEC. Pipe ends gain a reference.
static void spawn_inherit_stdio(process_t* parent, process_t* child){
    if(!parent || !child) return;
    for(int i=0;i<3;i++){
        if(!parent->fds[i].used) continue;
        uint64_t fl = fdt_lock(child);     // the child is already in the process table
        child->fds[i]=parent->fds[i];
        child->fds[i].cloexec=0;
        fdt_unlock(child, fl);
        fd_entry_ref(&child->fds[i]);
    }
}

// [M43/M46] Honour a set-id bit only on the root fs, for a SETID manifest, and
// never under no_new_privs (`nnp` = the exec'ing process's bit before the new
// image was loaded, like Linux). A refused set-id exec is audited.
static int setid_allowed(process_t* p, uint32_t fmode, int setid_fs, int nnp){
    int base = setid_fs && process_manifest_setid(p);
    int want = (fmode & 04000u) || ((fmode & 02000u) && (fmode & 0010u));
    if(want && base && nnp){
        cap_audit(p->pid, CAP_AUDIT_SETID, SYS_EXECVE, "setid-nnp", 0);
        return 0;
    }
    return base;
}

int ksys_spawn_argv(const char* path, int argc, const char* const argv[]){
    if(!path) return -1;
    // Follow symlinks like execve does: on a persistent root the multi-call
    // aliases are links (/bin/ls -> busybox); argv[0] keeps the name used.
    // [M43] A user process needs search permission on the way and x on the
    // file (root: at least one x bit); kernel-context spawns are not checked.
    process_t* caller = sched_get_current();
    char rp[256];
    vfs_inode_t* xi = 0;
    if(path[0]=='/'){
        int e = cred_check_path(caller, path, 1, MAY_EXEC, rp, sizeof(rp), &xi);
        if(e) return e == -CRED_ENOENT ? -1 : e;
        if(xi->type != VFS_NODE_FILE) return -CRED_EACCES;
        path = rp;
    }
    // [M52] One pinned inode for the set-id bits, the owner and the image:
    // three lookups by name let a rename on another CPU pair file A's
    // set-id mode with file B's contents. A different inode than the one
    // checked above (renamed meanwhile) is checked again.
    vfs_pin_t pin = VFS_PIN_INIT;
    vfs_inode_t* fi = vfs_lookup_pin(path, &pin);
    if(!fi){ vfs_unpin(&pin); return -1; }
    if(xi && fi != xi){
        int e = cred_inode_perm(caller, fi, MAY_EXEC);
        if(!e && fi->type != VFS_NODE_FILE) e = -CRED_EACCES;
        if(e){ vfs_unpin(&pin); return e; }
    }
    uint32_t fmode = cred_inode_mode(fi), fuid = fi->uid, fgid = fi->gid;
    int setid_fs = cred_on_root_mount(path);
    uint8_t* img = 0;
    int n = spawn_read_image(fi, &img);
    vfs_unpin(&pin);
    if(n <= 0) return -1;
    process_t* p = process_create_from_elf_owned(img, (size_t)n, argc, argv);   // owns img
    if(!p) return -1;
    // [M46] Exec confinement: the child's caps are bounded by the spawner's
    // (no_new_privs spawner); such a child never gains a Driver Space binding.
    cap_exec_bound_t bound = cap_exec_bound(caller);
    cap_exec_transition(p, bound, "spawn");
    if(bound.valid && bound.nnp && p->proc_type == PROC_TYPE_DRIVER){
        driver_remove_all_bindings(p);
        p->proc_type = PROC_TYPE_USER; p->drv_dev_id = -1; p->drv_caps = 0;
        cap_audit(p->pid, CAP_AUDIT_DRIVER, SYS_SPAWN, "spawn-nnp", 0);
    }
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
      if (sp && sp->pid != 0) process_set_cred(p, &sp->cred);   // [M43] same user as the spawner
      cred_exec_setid(p, fmode, fuid, fgid, setid_allowed(p, fmode, setid_fs, bound.valid && bound.nnp));
      process_auxv_set_creds(p); }                 // [U1] the child's final ids in its auxv
    { process_t* sp = sched_get_current();
      if (sp && sp->pid != 0 && sp->cwd[0]) {
          uint64_t fl = mm_lock(p);    // [M52] cwd writers publish under mm_lock (procfs reads it)
          int i = 0; for (; sp->cwd[i] && i < (int)sizeof(p->cwd) - 1; i++) p->cwd[i] = sp->cwd[i];
          p->cwd[i] = 0;
          mm_unlock(p, fl);
      } }
    int pid = (int)p->pid;            // read before publishing: it may exit and be reaped at once
    sched_make_ready(p);              // [M48] publish only now: fully wired
    return pid;
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

    char ap[256]; proc_resolve_path(c, kpath, ap, sizeof(ap));   // cwd-relative
    // [M43] search permission on the way + x on the file (root: any x bit).
    char rp[256]; vfs_inode_t* ino = 0;
    { int e = cred_check_path(c, ap, 1, MAY_EXEC, rp, sizeof(rp), &ino);
      if(e){ kfree(blob); return e; } }
    // [M52] Pin the mount while the image is read (an unmount drains it), and
    // check again if the name now leads to another inode (renamed meanwhile).
    vfs_pin_t pin = VFS_PIN_INIT;
    { vfs_inode_t* pi = vfs_lookup_pin(rp, &pin);
      int e = !pi ? -ENOENT : (pi != ino ? cred_inode_perm(c, pi, MAY_EXEC) : 0);
      if(e){ vfs_unpin(&pin); kfree(blob); return e; }
      ino = pi; }
    if(ino->type != VFS_NODE_FILE || !ino->ops || !ino->ops->read){ vfs_unpin(&pin); kfree(blob); return -EACCES; }
    uint32_t fmode = cred_inode_mode(ino), fuid = ino->uid, fgid = ino->gid;
    int setid_fs = cred_on_root_mount(rp);
    size_t size = ino->size;
    if(size == 0){ vfs_unpin(&pin); kfree(blob); return -ENOEXEC; }
    if(size > EXEC_MAX_IMAGE){ vfs_unpin(&pin); kfree(blob); return -ENOMEM; }
    // Private buffer (not the shared static spawn buffer: another CPU may be
    // spawning concurrently). It becomes the pinned image on success.
    uint8_t* img = (uint8_t*)kmalloc(size);
    if(!img){ vfs_unpin(&pin); kfree(blob); return -ENOMEM; }
    int rd = ino->ops->read(ino, 0, img, size);
    vfs_unpin(&pin);
    if(rd < 0){ kfree(img); kfree(blob); return -EACCES; }
    debugcon_writestring("[EXEC] execve pid="); debugcon_print_hex(c->pid);
    debugcon_writestring(" path="); debugcon_writestring(rp); debugcon_writestring("\n");
    int nnp_before = c->no_new_privs;                             // [M46]
    r = process_exec(c, tf, img, size, blob, used, argc, envc, rp);   // owns img
    if(r == 0){
        cred_exec_setid(c, fmode, fuid, fgid, setid_allowed(c, fmode, setid_fs, nnp_before));
        process_auxv_set_creds(c);                                // [U1] AT_UID..AT_SECURE
    }
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
    int st = process_pid_state((uint32_t)pid);   // [M50-B1] no pointer escapes
    if(st < 0) return 0;
    return (st == PROC_ZOMBIE) ? 0 : 1;
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

// [M50-B1] brk/mmap/munmap/mprotect edit the caller's VMAs and page tables
// under its mm_lock (cross-process readers: procfs, umount). Long PTE loops
// mm_relax() every MM_RELAX_PAGES pages so interrupts get in; that is safe
// because only the owner edits its own VMA set. Nothing in these sections
// reaches the page cache, a filesystem or user memory.
#define MM_RELAX_PAGES 256
static void mm_unmap_range(process_t* c, uint64_t lo, uint64_t hi, uint64_t* fl){
    uint32_t n = 0;
    for (uint64_t va = lo & ~0xFFFULL; va < hi; va += 0x1000) {
        vmm_unmap_in_space(c->space, va);
        if ((++n % MM_RELAX_PAGES) == 0) mm_relax(c, fl);
    }
}

uint64_t ksys_brk(uint64_t new_brk){
    process_t* c = sched_get_current();
    if (!c) return 0;
    if (new_brk == 0) return c->brk_cur;            // query current break
    new_brk = pgup(new_brk);
    if (new_brk < c->brk_start || new_brk >= USER_MMAP_BASE) return c->brk_cur;
    uint64_t fl = mm_lock(c);
    uint64_t old = c->brk_cur;
    if (new_brk == old) { mm_unlock(c, fl); return old; }
    if (new_brk > old) {                            // grow
        if (c->mem_limit && vma_total_bytes(&c->vmas) + (new_brk - old) > c->mem_limit) {
            mm_unlock(c, fl);
            return old;                             // would exceed signed mem limit
        }
        // Extend the heap's LAST piece (mprotect may have split the heap) when
        // it is plain RW anon; otherwise add a fresh RW piece for [old, new).
        const uint64_t hf = VMM_FLAG_USER|VMM_FLAG_RW|VMM_FLAG_NOEXEC;
        vma_t* h = (old > c->brk_start) ? vma_find_mut(&c->vmas, old - 1) : 0;
        if (h && h->end == old && h->type == VMA_TYPE_ANON && h->flags == hf) h->end = new_brk;
        else if (vma_add(&c->vmas, old, new_brk, hf, VMA_TYPE_ANON,0,0,0,0) != 0) {
            mm_unlock(c, fl);
            return old;
        }
    } else {                                        // shrink
        mm_unmap_range(c, new_brk, old, &fl);
        for (uint32_t i = 0; i < c->vmas.count; i++) {   // trim every heap piece
            vma_t* v = &c->vmas.v[i];
            if (v->type == VMA_TYPE_NONE || v->end <= new_brk || v->start >= old) continue;
            if (v->start < c->brk_start) continue;       // not a heap piece
            if (v->start >= new_brk) vma_remove(&c->vmas, v); else v->end = new_brk;
        }
    }
    c->brk_cur = new_brk;
    mm_unlock(c, fl);
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
        // [M43] A mapping never grants more than the open did: readable fd
        // required, and a writable shared (device) map needs O_RDWR.
        if ((c->fds[fd].flags & 3) == 1) return (uint64_t)(int64_t)-13;              // -EACCES: O_WRONLY
        if ((flags & 0x01 /*MAP_SHARED*/) && (prot & PROT_WRITE) && (c->fds[fd].flags & 3) != 2)
            return (uint64_t)(int64_t)-13;
        if (devfs_is_fb(c->fds[fd].inode))
            return (uint64_t)fbdev_mmap(c, len, prot, flags, off);
        if (input_is_client(c->fds[fd].inode)) return (uint64_t)(int64_t)-19;   // -ENODEV
        if (udrv_is_fd(c->fds[fd].inode)) return (uint64_t)(int64_t)-19;        // [M57] MAP_BAR instead
    }
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) return (uint64_t)-1; // W^X
    vfs_inode_t* ino = 0;
    if (!(flags & MAP_ANONYMOUS)) {
        // [M20] File-backed mmap (MAP_PRIVATE) via the page cache, from offset 0.
        if (fd < 0 || fd >= 32 || !c->fds[fd].used) return (uint64_t)-1;
        if (c->fds[fd].is_pipe || c->fds[fd].is_sock) return (uint64_t)-1;  // not a file
        ino = (vfs_inode_t*)c->fds[fd].inode;
        if (!ino) return (uint64_t)-1;
        // devfs/procfs/sysfs contents are generated and their inode slots are
        // recycled per lookup: cached pages keyed by such a slot would later be
        // served as another node's contents. Not mappable (Linux: -ENODEV).
        if (!ino->ops || (ino->ops->flags & VFS_FS_NOCACHE)) return (uint64_t)(int64_t)-19;
    }
    uint64_t fl = mm_lock(c);
    uint64_t base = c->mmap_next;
    uint64_t r = (uint64_t)-1;
    if (base >= USER_MMAP_END || len > USER_MMAP_END - base) goto out;  // arena exhausted (no wrap)
    len = pgup(len);
    if (len > USER_MMAP_END - base) goto out;
    if (c->mem_limit && vma_total_bytes(&c->vmas) + len > c->mem_limit) goto out;
    if (!ino) {
        if (vma_add(&c->vmas, base, base + len, m18_prot_to_flags(prot), VMA_TYPE_ANON,0,0,0,0) != 0)
            goto out;
    } else {
        if (vma_add_file(&c->vmas, base, base + len, m18_prot_to_flags(prot), ino, 0) != 0)
            goto out;
    }
    c->mmap_next = base + len;
    r = base;
out:
    mm_unlock(c, fl);
    return r;
}

int ksys_munmap(uint64_t addr, uint64_t len){
    process_t* c = sched_get_current();
    if (!c) return -1;
    len = m18_user_span(addr, len);
    if (!len) return -1;                                     // EINVAL
    uint64_t end = addr + len;
    uint64_t fl = mm_lock(c);
    if (vma_pinned_overlap(&c->vmas, addr, end)) { mm_unlock(c, fl); return -22; }  // [M57] -EINVAL
    // Every user page is VMA-backed (M14), so only VMA intersections can hold
    // present PTEs: unmap those instead of walking the whole (maybe huge) range.
    for (uint32_t i = 0; i < c->vmas.count; i++) {
        const vma_t* v = &c->vmas.v[i];
        if (v->type == VMA_TYPE_NONE || !(addr < v->end && v->start < end)) continue;
        uint64_t lo = addr > v->start ? addr : v->start;
        uint64_t hi = end  < v->end   ? end  : v->end;
        mm_unmap_range(c, lo, hi, &fl);
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
    mm_unlock(c, fl);
    return 0;
}

int ksys_mprotect(uint64_t addr, uint64_t len, int prot){
    process_t* c = sched_get_current();
    if (!c) return -1;
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) return -1;   // W^X
    len = m18_user_span(addr, len);
    if (!len) return -1;                                        // EINVAL
    uint64_t end = addr + len;
    uint64_t fl = mm_lock(c);
    int ret = -1;
    if (!vma_overlaps(&c->vmas, addr, end)) goto out;           // must be backed
    if (vma_pinned_overlap(&c->vmas, addr, end)) { ret = -22; goto out; }  // [M57] driver map: -EINVAL
    // [M40] Device memory (the framebuffer) is never executable.
    if (prot & PROT_EXEC) {
        for (uint32_t i = 0; i < c->vmas.count; i++) {
            const vma_t* v = &c->vmas.v[i];
            if (v->type == VMA_TYPE_PHYS && addr < v->end && v->start < end) goto out;
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
        if (v->start < addr && addr < v->end && vma_split(&c->vmas, v, addr) != 0) goto out;
    }
    for (uint32_t i = 0; i < c->vmas.count; i++) {
        vma_t* v = &c->vmas.v[i];
        if (v->type == VMA_TYPE_NONE) continue;
        if (v->start < end && end < v->end && vma_split(&c->vmas, v, end) != 0) goto out;
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
        uint32_t n = 0;
        for (uint64_t va = lo & ~0xFFFULL; va < hi; va += 0x1000) {
            vmm_protect_in_space(c->space, va, newf);
            if ((++n % MM_RELAX_PAGES) == 0) mm_relax(c, &fl);
        }
    }
    ret = 0;
out:
    mm_unlock(c, fl);
    return ret;
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

// [M43] The inode of an open file, for fchmod/fchown/futimes. [M52] The
// change is applied to this inode (cred_*_ino -> vfs_fsetattr), not to the
// path it was opened by, which a rename may since have given to another
// file. The fd pins its mount. 0 or -errno (-EBADF / -EINVAL).
static int fd_attr_inode(int fd, vfs_inode_t** out){
    process_t* c=sched_get_current(); if(!c) return -9;
    if(fd<0||fd>=32||!c->fds[fd].used||c->fds[fd].is_pipe||c->fds[fd].is_sock||!c->fds[fd].inode) return -9;
    vfs_inode_t* ino=(vfs_inode_t*)c->fds[fd].inode;
    if(!ino->ops) return -22;
    *out = ino;
    return 0;
}

// open() follows symlinks in every path component (final one included), honours
// O_EXCL (0x80) with O_CREAT, and creates with mode & ~umask (0 = 0666).
// [M52] Runs without the kernel lock, so nothing here may look the path up
// again after the permission check: the inode that was checked is the one
// truncated and installed. The mount stays pinned from the lookup to the fd
// install, so an unmount on another CPU either waits for the open or makes
// it fail, and never detaches a filesystem an fd is about to point into.
static int open_check_existing(process_t* c, vfs_inode_t* ino, int flags){
    int acc = flags & 3, mask = 0;
    if(acc != 1) mask |= MAY_READ;                                           // O_RDONLY / O_RDWR
    if(acc != 0 || (flags & 0x200)) mask |= MAY_WRITE;                       // O_WRONLY / O_RDWR / O_TRUNC
    if(ino->type == VFS_NODE_DIR && (mask & MAY_WRITE)) return -21;          // -EISDIR
    // [M42] Read-only filesystem: no write access, no O_TRUNC (-EROFS,
    // also for root and before the permission bits, as on Linux).
    int pe = cred_inode_perm(c, ino, mask); if(pe) return pe;
    if(ino->type==VFS_NODE_FILE && ino->ops && (ino->ops->flags & VFS_FS_RDONLY) && (mask & MAY_WRITE))
        return -30;                                                          // kernel callers too
    return 0;
}
// Look `path` up again with the pin moved to the new result.
static vfs_inode_t* open_repin(const char* path, vfs_pin_t* pin){
    vfs_unpin(pin);
    return vfs_lookup_pin(path, pin);
}
static int ksys_open_pinned(process_t* c, const char* path, int flags, uint32_t mode, int lerr,
                            vfs_pin_t* pin){
    vfs_inode_t* ino=vfs_lookup_pin(path, pin);
    if(ino && (flags & 0x40) && (flags & 0x80)) return -17;                  // O_EXCL: -EEXIST
    int created = 0;
    if(!ino && (flags & 0x40) && (lerr==0 || lerr==-2)){                     // O_CREAT
        int ce = cred_may_create(c, path); if(ce) return ce;                 // -EROFS before -EACCES
        // [M52] Created with its final mode/owner in the driver's critical
        // section (vfs_create_attr): never visible root-owned 0644.
        vfs_attr_t na; unsigned nv = cred_new_attr(c, path, mode, 0, &na);
        int cr = vfs_create_attr(path, &na, nv);
        if(cr==0){ ino=open_repin(path, pin); created = ino != NULL; }
        else if(cr==-30) return -30;                                         // -EROFS
        else {
            // Lost a race with another creator: open what is there now,
            // like an open of an existing file (O_EXCL: -EEXIST).
            ino=open_repin(path, pin);
            if(ino && (flags & 0x80)) return -17;
        }
    }
    if(!ino) return lerr ? lerr : -2;   // [M39] -ENOENT so libc can set errno (bash rcfile probing)
    if(!created){ int e = open_check_existing(c, ino, flags); if(e) return e; }
    // O_TRUNC truncates regular files only: on a device node it is ignored,
    // as on Linux (devfs block nodes report the disk size; dd needs this).
    uint32_t ift = ino->mode & 0170000u;
    if((flags & 0x200) && ino->type==VFS_NODE_FILE && (ift==0 || ift==0100000u) && ino->size){
        if(vfs_ftruncate(ino, 0) != 0) return -1;                            // this inode, not the name
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
    { uint64_t fl = fdt_lock(c); c->fds[fd].inode=ino; fdt_unlock(c, fl); }
    c->fds[fd].flags=flags;
    c->fds[fd].offset = (flags & 0x400) ? ino->size : 0;    // O_APPEND -> seek to end
    c->fds[fd].cloexec = (flags & K_O_CLOEXEC) ? 1 : 0;     // O_CLOEXEC
    c->fds[fd].flags = flags & ~K_O_CLOEXEC;                // F_GETFL: status flags only
    return fd;
}
int ksys_open_mode(const char* path, int flags, uint32_t mode){
    extern process_t* sched_get_current(void);
    process_t* c=sched_get_current(); if(!c) return -1;
    char ap[256]; proc_resolve_path(c, path, ap, sizeof(ap));             // [M39] cwd-relative
    // [M43] Resolve with search permission on every component, then check
    // the access the open asks for (read/write by owner/group/other; O_TRUNC
    // needs write), or, to create, write+search on the parent directory.
    char rp[256];
    int lerr = cred_resolve(c, ap, 1, rp, sizeof(rp));
    if(lerr) return lerr;                                                    // -EACCES/-ENOENT/-ENOTDIR/-ELOOP
    // The pin is dropped here, after the fd is installed (or the open failed).
    vfs_pin_t pin = VFS_PIN_INIT;
    int r = ksys_open_pinned(c, rp, flags, mode, lerr, &pin);
    vfs_unpin(&pin);
    return r;
}
int ksys_open(const char* path, int flags){ return ksys_open_mode(path, flags, 0); }
int ksys_close(int fd){ extern process_t* sched_get_current(void); process_t* c=sched_get_current(); if(!c) return -1; if(fd<0||fd>=32) return -1; if(!c->fds[fd].used) return -1;
    // Unhook the entry first (under the fd-table lock), then drop its object:
    // pipe end (EOF/EPIPE) / socket ref (FIN on the last) / pty end (hangup).
    uint64_t fl = fdt_lock(c);
    struct proc_fd_entry e = c->fds[fd];
    c->fds[fd].used=0; c->fds[fd].inode=NULL; c->fds[fd].is_pipe=0; c->fds[fd].is_sock=0;
    fdt_unlock(c, fl);
    fd_entry_unref(&e);
    return 0; }
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
    if(c->fds[fd].used && c->fds[fd].inode && udrv_is_fd(c->fds[fd].inode))   // [M57] claim fd: events
        return udrv_fd_read(c, c->fds[fd].inode, ubuf, len, (c->fds[fd].flags & 0x800) != 0);   // -2: blocks
    int is_pipe = c->fds[fd].used && c->fds[fd].is_pipe;
    int is_file = c->fds[fd].used && c->fds[fd].inode && !is_pipe;
    if(!is_pipe && !is_file && fd!=0) return -1;
    if(is_file && (c->fds[fd].flags & 3) == 1) return -9;   // [M43] -EBADF: opened O_WRONLY
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
    if(c->fds[fd].used && c->fds[fd].inode && udrv_is_fd(c->fds[fd].inode))   // [M57] claim fd: ack + unmask
        return udrv_fd_write(c, c->fds[fd].inode, ubuf, len);
    int is_pipe = c->fds[fd].used && c->fds[fd].is_pipe;
    int is_file = c->fds[fd].used && c->fds[fd].inode && !is_pipe;
    if(!is_pipe && !is_file && fd!=1 && fd!=2) return -1;
    if(is_pipe && !c->fds[fd].pipe_w) return -1;            // [M25] write end only
    if(is_file && (c->fds[fd].flags & 3) == 0) return -9;   // [M43] -EBADF: opened O_RDONLY
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
            // [M51 C1] The end of file is resolved and written in one
            // critical section (vfs_append: the driver's ->append under its
            // instance lock, or the VFS fallback under vfs_append_lock), so
            // two appenders can never both pick the same offset. The fd
            // table and its offset are per process and a process runs on
            // one CPU at a time (no threads), so they need no lock.
            size_t off;
            if(c->fds[fd].flags & 0x400){
                uint64_t at = 0;
                r = vfs_append(ino, u.p, (size_t)want, &at);
                off = (size_t)at;
                if(r>0) c->fds[fd].offset = at;
            } else {
                off = c->fds[fd].offset;
                r = ino->ops->write(ino, off, u.p, (size_t)want);
            }
            if(r>0){ c->fds[fd].offset += (uint64_t)r; pagecache_invalidate(ino, off, (uint64_t)r); }
        } else {
            // [M9] fd 1 (stdout) / 2 (stderr) with no redirection -> console,
            // through the TTY's termios output processing (OPOST/ONLCR).
            extern int tty_write(const void* buf, int len);
            tty_write(u.p, want);
            // Not a kernel message: skip klog. Mirrored in whole lines
            // (kernel/klog.c), so a scheduler/kernel line printed between two
            // write() calls of one user line cannot land in the middle of it.
            { extern void debugcon_user_write(const char*, int);
              debugcon_user_write((const char*)u.p, want); }
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
    // Per-mount st_dev (the block device's dev_t, or an anonymous 0:N for a
    // virtual FS), so df <path>, find -xdev, du -x and cp -a can tell mounts
    // apart; it used to be one constant for every file. st_ino is the FS's own
    // id where it has one (ext2 inode number, FAT/exFAT entry location), else
    // the inode pointer — unique, which userland tree-walks (bash's getcwd
    // compares (st_dev,st_ino) up the path) depend on.
    st->st_dev = vfs_dev_of_ops(ino->ops);
    if(!st->st_dev) st->st_dev = 1;
    st->st_ino = ino->ino ? ino->ino : (uint64_t)(uintptr_t)ino;
    st->st_rdev = 0;
    st->st_blksize = 512;
    st->st_blocks = (int64_t)((ino->size + 511) / 512);
}

int ksys_stat(const char* path, struct secos_stat* st){
    char rp[256]; proc_resolve_path(sched_get_current(), path, rp, sizeof(rp)); path = rp; // [M39] cwd-relative
    vfs_inode_t* ino=0;                                     // [M43] search permission on the way
    int e = cred_check_path(sched_get_current(), path, 1, 0, 0, 0, &ino);
    if(!ino) return e ? e : -2;                             // -ENOENT / -ENOTDIR / -ELOOP / -EACCES
    stat_fill(ino, st);
    return 0;
}

// [M26] lstat: like stat but does not follow a final symlink. vfs_lookup() does
// not auto-follow symlinks (the inode keeps type VFS_NODE_SYMLINK), so lstat is
// the plain lookup; stat() resolution of symlinks is layered in vfs (M26-3).
int ksys_lstat(const char* path, struct secos_stat* st){
    char rp[256]; proc_resolve_path(sched_get_current(), path, rp, sizeof(rp)); path = rp; // [M39] cwd-relative
    vfs_inode_t* ino=0;                                     // intermediate symlinks followed, final not
    int e = cred_check_path(sched_get_current(), path, 0, 0, 0, 0, &ino);
    if(!ino) return e ? e : -2;
    stat_fill(ino, st);
    return 0;
}

// [M26] chmod/chown/utimes, [M43] with the owner/root rules (kernel/cred.c).
// `path` is absolute; follow = 0 for lchown. The fd forms (fchmod/fchown/
// futimes) pass the fd's absolute path, already resolved.
static int attr_target(const char* path, int follow, char* cp, size_t cap){
    vfs_inode_t* ino=0;
    int e = cred_check_path(sched_get_current(), path, follow, 0, cp, cap, &ino);
    return ino ? 0 : (e ? e : -2);
}
int ksys_chmod(const char* path, uint32_t mode){
    char cp[256]; int e=attr_target(path, 1, cp, sizeof(cp)); if(e) return e;
    return cred_chmod(sched_get_current(), cp, mode);
}
int ksys_chown_ex(const char* path, uint32_t uid, uint32_t gid, int follow){
    char cp[256]; int e=attr_target(path, follow, cp, sizeof(cp)); if(e) return e;
    return cred_chown(sched_get_current(), cp, uid, gid);
}
int ksys_chown(const char* path, uint32_t uid, uint32_t gid){ return ksys_chown_ex(path, uid, gid, 1); }
int ksys_utimes(const char* path, uint64_t atime, uint64_t mtime){
    char cp[256]; int e=attr_target(path, 1, cp, sizeof(cp)); if(e) return e;
    return cred_utimes(sched_get_current(), cp, atime, mtime);
}
int ksys_readlink(const char* path, char* buf, int len){
    extern int vfs_readlink(const char*, char*, size_t);
    if(len<=0) return -1;
    char cp[256]; int e=attr_target(path, 0, cp, sizeof(cp)); if(e) return e;   // [M43] search perm
    return vfs_readlink(cp, buf, (size_t)len);
}
int ksys_symlink(const char* target, const char* linkpath){
    extern int vfs_symlink(const char*, const char*);
    process_t* c=sched_get_current();
    char cp[256]; int e=cred_resolve(c, linkpath, 0, cp, sizeof(cp)); if(e) return e;
    if(vfs_lookup(cp)) return -17;                                               // -EEXIST
    if((e=cred_may_create(c, cp))) return e;
    int r = vfs_symlink(target, cp);
    if(r==0 && !cred_is_kernel(c)){                                              // owned by the creator
        uint32_t uid, gid; char par[256]; size_t ls=0;
        for(size_t i=0;cp[i];i++) if(cp[i]=='/') ls=i;
        if(ls==0){ par[0]='/'; par[1]=0; } else { for(size_t i=0;i<ls;i++) par[i]=cp[i]; par[ls]=0; }
        cred_new_owner(c, par, &uid, &gid, 0);
        if(uid || gid){ vfs_attr_t a; a.uid=uid; a.gid=gid; a.mode=0; a.atime=a.mtime=0;
                        vfs_lsetattr(cp, &a, VFS_ATTR_UID|VFS_ATTR_GID); }
    }
    return r;
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
    // As many records as the caller's buffer holds (up to 1 MiB = 4096
    // entries): BusyBox's opendir retries with a bigger buffer when one comes
    // back full, so large directories (e.g. Windows folders on NTFS) list whole.
    if(buflen <= 0) return -1;
    // cwd-relative + normalized: bash's glob and readline's filename completion
    // open "." and "dir/" (trailing slash), which vfs_readdir does not accept.
    char rp[256]; proc_resolve_path(sched_get_current(), path, rp, sizeof(rp)); path = rp;
    char cp[256]; vfs_inode_t* di=0;                          // [M43] listing needs r on the dir
    int e = cred_check_path(sched_get_current(), path, 1, MAY_READ, cp, sizeof(cp), &di);
    if(e) return e == -CRED_ENOENT ? -1 : e;               // (kbuf not allocated yet)
    path = cp;
    int cap = buflen > (1 << 20) ? (1 << 20) : buflen;
    char* kbuf = (char*)kmalloc((size_t)cap);
    if(!kbuf) return -12;                    // -ENOMEM
    struct gd_ctx g = { kbuf, cap, 0 };
    if(vfs_readdir(path, gd_cb, &g) != 0){ kfree(kbuf); return -1; }
    int n = g.len < buflen ? g.len : buflen;
    if(n > 0 && copy_to_user(ubuf, kbuf, (size_t)n) != 0){ kfree(kbuf); return -1; }
    kfree(kbuf);
    return n;
}
int ksys_create(const char* path){
    process_t* c=sched_get_current();
    char cp[256]; int e=cred_resolve(c, path, 0, cp, sizeof(cp)); if(e) return e;   // [M43]
    if(vfs_lookup(cp)) return -17;                                                   // -EEXIST
    if((e=cred_may_create(c, cp))) return e;
    vfs_attr_t na; unsigned nv = cred_new_attr(c, cp, 0, 0, &na);   // [M52] one driver section
    return vfs_create_attr(cp, &na, nv);
}
// mkdir(path, mode): w+x on the parent; owned by the creator, mode & ~umask.
int ksys_mkdir_mode(const char* path, uint32_t mode){
    process_t* c=sched_get_current();
    char cp[256]; int e=cred_resolve(c, path, 0, cp, sizeof(cp)); if(e) return e;   // [M43]
    if(vfs_lookup(cp)) return -17;                                                   // -EEXIST
    if((e=cred_may_create(c, cp))) return e;
    vfs_attr_t na; unsigned nv = cred_new_attr(c, cp, mode, 1, &na);   // [M52] one driver section
    return vfs_mkdir_attr(cp, &na, nv);
}
int ksys_mkdir(const char* path){ return ksys_mkdir_mode(path, 0); }
int ksys_unlink(const char* path){
    extern int vfs_remove(const char*);
    process_t* c=sched_get_current();
    char cp[256]; int e=cred_resolve(c, path, 0, cp, sizeof(cp)); if(e) return e;   // [M43]
    if(!vfs_lookup(cp)) return -2;
    if((e=cred_may_delete(c, cp))) return e;                     // w+x on the parent, sticky dirs
    // Drop the cached pages first: the FS may hand this inode slot (or inode
    // number) to the next file created.
    { vfs_inode_t* ino = vfs_lookup(cp); if(ino && ino->type == VFS_NODE_FILE) pagecache_invalidate(ino, 0, 1ULL<<62); }
    return vfs_remove(cp);
}

// [M26] mount/umount: map a block-device name + fstype to the existing FS mount
// helpers (the same path the boot mount and shell `mountdev` use). fstype:
// "fat32"/"vfat" -> FAT32; "ext2"/"ext4"/"extN" -> ext driver; "exfat";
// "ntfs" (read-only); "auto"/"" -> try them in that order. Root only [M43].
// `opts` = mount(2) data: uid=,gid=,umask=,dmask=,fmask= for the FSes that
// keep no owners (fs/mntopts.h); "ro"/"rw" for every FS (NTFS is always ro);
// ext2/ext4 also "noload" (ro without journal replay). [M46] A "remount"
// token switches the volume already mounted at `target` between ro and rw
// (FAT32, ext2/ext4: other FSes have no remount hook -> EINVAL).
static int str_eq2(const char* a, const char* b){ int i=0; while(a[i]&&b[i]){ if(a[i]!=b[i]) return 0; i++; } return a[i]==b[i]; }
// Remove every whole-word `tok` from the comma list `s` (in place); 1 if found.
static int mnt_take_opt(char* s, const char* tok){
    int found = 0; char* w = s; const char* r = s;
    while(*r){
        const char* e = r; while(*e && *e != ',') e++;
        int n = (int)(e - r), k = 0;
        while(tok[k] && k < n && r[k] == tok[k]) k++;
        if(!tok[k] && k == n) found = 1;
        else { if(w != s) *w++ = ','; for(int i = 0; i < n; i++) *w++ = r[i]; }
        r = *e ? e + 1 : e;
    }
    *w = 0;
    return found;
}
static int ksys_remount(const char* target, const char* opts){
    char abs[256];
    proc_resolve_path(sched_get_current(), target, abs, sizeof(abs));
    const vfs_fs_ops_t* ops = NULL;
    int n = vfs_mount_count();
    for(int i=0;i<n;i++){
        const char* mp=0; const char* fsn=0;
        if(vfs_mount_info(i,&mp,&fsn)==0 && mp && str_eq2(mp,abs)){ ops = vfs_mount_ops_at(i); break; }
    }
    if(!ops) return -CRED_EINVAL;                         // not a mount point
    char o[128]; int i = 0;
    for(; opts[i] && i < (int)sizeof(o) - 1; i++) o[i] = opts[i];
    o[i] = 0;
    int ro = mnt_take_opt(o, "ro"), rw = mnt_take_opt(o, "rw");
    mnt_take_opt(o, "defaults");
    (void)rw;                                             // the other options are left as they are
    if(!ops->remount) return -CRED_EINVAL;
    return ops->remount(ro);
}
static int ksys_mount_opts_locked(const char* dev, const char* target, const char* fstype, const char* opts);
// [M51 C1] The whole mount — the driver's probe, its vfs_mount, a remount —
// runs under vfs_ns_lock, so two mounts (or a mount and a umount) never
// interleave their filesystem I/O or their "already mounted?" checks.
int ksys_mount_opts(const char* dev, const char* target, const char* fstype, const char* opts){
    vfs_ns_lock();
    int r = ksys_mount_opts_locked(dev, target, fstype, opts);
    vfs_ns_unlock();
    return r;
}
static int ksys_mount_opts_locked(const char* dev, const char* target, const char* fstype, const char* opts){
    extern void* block_find(const char* name);
    extern int fat32_mount(const char*, const char*);
    extern int ext2_mount(const char*, const char*);
    extern int exfat_mount(const char*, const char*);
    extern int ntfs_mount(const char*, const char*);
    if(!dev||!target||!fstype) return -1;
    if(!cred_is_root(sched_get_current())) return -CRED_EPERM;   // [M43] root only
    {   // [M46] mount -o remount,...: no device / fstype needed.
        char o[128]; int i = 0;
        for(; opts && opts[i] && i < (int)sizeof(o) - 1; i++) o[i] = opts[i];
        o[i] = 0;
        if(mnt_take_opt(o, "remount")) return ksys_remount(target, o);
    }
    // Accept the devfs spelling ("/dev/vda") as well as the bare block name.
    if(dev[0]=='/'&&dev[1]=='d'&&dev[2]=='e'&&dev[3]=='v'&&dev[4]=='/') dev += 5;
    if(!block_find(dev)) return -1;                       // no such device
    // Normalize the mount point like umount does ("/x/", relative paths), so
    // the two agree on its spelling. The VFS copies it; `abs` may be a local.
    char abs[256];
    proc_resolve_path(sched_get_current(), target, abs, sizeof(abs));
    if(abs[0]=='/' && abs[1]==0) return -1;               // never over the root
    target = abs;
    int isfat = str_eq2(fstype,"fat32")||str_eq2(fstype,"vfat");
    int isext = str_eq2(fstype,"ext2")||str_eq2(fstype,"ext4")||str_eq2(fstype,"extN");
    int isexf = str_eq2(fstype,"exfat");
    int isntfs= str_eq2(fstype,"ntfs")||str_eq2(fstype,"ntfs3")||str_eq2(fstype,"ntfs-3g");  // [M42] read-only
    int isauto= str_eq2(fstype,"auto")||fstype[0]==0;
    extern int fat32_set_mount_options(const char* opts);
    extern int ext2_set_mount_options(const char* opts);
    if(isext){                                            // [M46] ro / noload
        if(ext2_set_mount_options(opts)!=0) return -CRED_EINVAL;
        int er = ext2_mount(dev,target);
        ext2_set_mount_options("");
        return er;
    }
    // Owner/mode options of FAT32/exFAT/NTFS (fs/mntopts.h); ext2/ext4 keep
    // real owners and ignore them. A malformed option list is -EINVAL.
    extern int exfat_set_mount_options(const char* opts);
    extern int ntfs_set_mount_options(const char* opts);
    int r = -1;
    if(isfat){ if(fat32_set_mount_options(opts)!=0) return -CRED_EINVAL; r = fat32_mount(dev,target); }
    else if(isexf){ if(exfat_set_mount_options(opts)!=0) return -CRED_EINVAL; r = exfat_mount(dev,target); }
    else if(isntfs){ if(ntfs_set_mount_options(opts)!=0) return -CRED_EINVAL; r = ntfs_mount(dev,target); }
    else if(isauto){
        if(fat32_set_mount_options(opts)==0 && fat32_mount(dev,target)==0) r = 0;
        else if(ext2_set_mount_options(opts)==0 && ext2_mount(dev,target)==0) r = 0;
        else if(exfat_set_mount_options(opts)==0 && exfat_mount(dev,target)==0) r = 0;
        else if(ntfs_set_mount_options(opts)==0) r = ntfs_mount(dev,target);
    }
    // A refused mount must not leave its options for the next one.
    fat32_set_mount_options(""); exfat_set_mount_options(""); ntfs_set_mount_options("");
    ext2_set_mount_options("");
    if(isfat || isexf || isntfs || isauto) return r;
    return -1;                                            // unknown fstype
}
int ksys_mount(const char* dev, const char* target, const char* fstype){ return ksys_mount_opts(dev, target, fstype, ""); }
void proc_resolve_path(process_t* c, const char* in, char* out, int outsz);
// Is any process still holding a file of the filesystem behind `ops` (an open
// fd or a file-backed mapping)? Detaching it would leave that vfs_inode_t*
// bound to a slot the next mount of the same driver hands to another file.
struct umount_busy { const vfs_fs_ops_t* ops; int busy; };
static void umount_busy_cb(process_t* p, void* u){
    struct umount_busy* b = (struct umount_busy*)u;
    if(b->busy || p->state == PROC_ZOMBIE) return;
    // [M50-B1] S30 (process_foreach) -> S35. [M52] The fd table too: its
    // owner rewrites entries under mm_lock (dup2/close/pipe on another CPU),
    // and an entry seen mid-change could be a pipe read as a vfs_inode_t.
    uint64_t fl = mm_lock(p);
    for(int i=0;i<32;i++){
        struct proc_fd_entry* e = &p->fds[i];
        if(!e->used || e->is_pipe || e->is_sock || !e->inode) continue;
        if(((const vfs_inode_t*)e->inode)->ops == b->ops){ b->busy = 1; mm_unlock(p, fl); return; }
    }
    for(uint32_t i=0;i<p->vmas.count;i++){
        const vma_t* v = &p->vmas.v[i];
        if(v->type != VMA_TYPE_NONE && v->file_inode && v->file_inode->ops == b->ops){ b->busy = 1; break; }
    }
    mm_unlock(p, fl);
}
static int umount_is_busy(const vfs_fs_ops_t* ops, void* ctx){
    (void)ctx;
    struct umount_busy b = { ops, 0 };
    process_foreach(umount_busy_cb, &b);
    return b.busy;
}
int ksys_umount(const char* target){
    extern int vfs_unmount(const char*);
    // Normalize like every other path syscall ("/mnt/", "/mnt/.", relative
    // paths, "" -> cwd), so "/" cannot be reached under another spelling.
    char abs[256];
    if(!cred_is_root(sched_get_current())) return -CRED_EPERM;   // [M43] root only
    proc_resolve_path(sched_get_current(), target, abs, sizeof(abs));
    const vfs_fs_ops_t* ops = NULL;
    {
        int n = vfs_mount_count();
        for(int i=0;i<n;i++){
            const char* mp=0; const char* fsn=0;
            if(vfs_mount_info(i,&mp,&fsn)==0 && mp && str_eq2(mp,abs)){ ops = vfs_mount_ops_at(i); break; }
        }
    }
    if(!ops) return -22;                                  // -EINVAL: not a mount point
    // [M51 C1] Under vfs_ns_lock: drain the operations in flight on the
    // mount, then the open-fd / file-mapping check (-EBUSY, like Linux
    // without MNT_DETACH; only for the FS's last mount), then sync (exFAT
    // clears VolumeDirty, ext2/ext4 closes any open transaction) and detach.
    vfs_ns_lock();
    int r = vfs_unmount_ex(abs, umount_is_busy, NULL, VFS_UMOUNT_SYNC);
    vfs_ns_unlock();
    return r == -1 ? -22 : r;                             // raced another umount: EINVAL
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
    uint64_t fl = fdt_lock(c);
    struct proc_fd_entry old = c->fds[newfd];
    c->fds[newfd]=c->fds[oldfd]; c->fds[newfd].used=1; c->fds[newfd].cloexec=0; // POSIX: dup clears FD_CLOEXEC
    fdt_unlock(c, fl);
    if(old.used) fd_entry_unref(&old);                         // close the old newfd target
    return newfd;
}
int ksys_dup(int oldfd){
    process_t* c=sched_get_current(); if(!c) return -1;
    if(oldfd<0||oldfd>=32||!c->fds[oldfd].used) return -1;
    int n=fd_alloc(c); if(n<0) return -1;
    uint64_t fl = fdt_lock(c);
    c->fds[n]=c->fds[oldfd]; c->fds[n].used=1; c->fds[n].cloexec=0;
    fdt_unlock(c, fl);
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
        uint64_t fl = fdt_lock(c);
        c->fds[n]=c->fds[fd]; c->fds[n].used=1;
        c->fds[n].cloexec = (cmd==K_F_DUPFD_CLOEXEC) ? 1 : 0;
        fdt_unlock(c, fl);
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
    char ab0[256]; proc_resolve_path(c, k, ab0, sizeof(ab0));
    char abs[256]; vfs_inode_t* ino=0;                          // [M43] x on every component
    int e = cred_check_path(c, ab0, 1, 0, abs, sizeof(abs), &ino);
    if(!ino) return e ? e : -1;
    if(ino->type != VFS_NODE_DIR) return -20;                   // -ENOTDIR
    if((e = cred_inode_perm(c, ino, MAY_EXEC))) return e;       // -EACCES
    // Keep the path the caller used (symlinks in it intact), as before.
    { int i=0; for(; ab0[i] && i<255; i++) abs[i]=ab0[i]; abs[i]=0; }
    uint64_t fl = fdt_lock(c);                                  // [M51 C2] procfs reads it
    int i=0; for(; abs[i] && i<255; i++) c->cwd[i]=abs[i]; c->cwd[i]=0;
    fdt_unlock(c, fl);
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
        char argstore[SPAWN_MAX_ARGS][SPAWN_ARG_LEN];   /* [M52] per call (1 KiB): was static, shared by every CPU */
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
    // [M43] path == NULL: the fd form (fchmod / fchown / futimes); the fd is
    // the last argument. chown a3 bit 0: do not follow a final symlink (lchown).
    case SYS_CHMOD: {
        CAP_GATE(CAP_FS_WRITE, "chmod");  // [M35]
        char kp[256];
        if (!a0) { vfs_inode_t* fi; int e = fd_attr_inode((int)a2, &fi); if (e) return (uint64_t)(int64_t)e;
                   return (uint64_t)(int64_t)cred_chmod_ino(sched_get_current(), fi, (uint32_t)a1); }
        else { char kq[256]; if (copy_user_str((const char*)a0, kq, sizeof(kq)) != 0) return (uint64_t)(int64_t)-EFAULT;
               proc_resolve_path(sched_get_current(), kq, kp, sizeof(kp)); }  // cwd-relative
        return (uint64_t)(int64_t)ksys_chmod(kp, (uint32_t)a1);
    }
    case SYS_CHOWN: {
        CAP_GATE(CAP_FS_WRITE, "chown");  // [M35]
        char kp[256];
        if (!a0) { vfs_inode_t* fi; int e = fd_attr_inode((int)a3, &fi); if (e) return (uint64_t)(int64_t)e;
                   return (uint64_t)(int64_t)cred_chown_ino(sched_get_current(), fi, (uint32_t)a1, (uint32_t)a2); }
        else { char kq[256]; if (copy_user_str((const char*)a0, kq, sizeof(kq)) != 0) return (uint64_t)(int64_t)-EFAULT;
               proc_resolve_path(sched_get_current(), kq, kp, sizeof(kp)); }  // cwd-relative
        return (uint64_t)(int64_t)ksys_chown_ex(kp, (uint32_t)a1, (uint32_t)a2, a0 && (a4 & 1) ? 0 : 1);
    }
    case SYS_UTIMES: {
        CAP_GATE(CAP_FS_WRITE, "utimes"); // [M35]
        char kp[256];
        if (!a0) { vfs_inode_t* fi; int e = fd_attr_inode((int)a3, &fi); if (e) return (uint64_t)(int64_t)e;
                   return (uint64_t)(int64_t)cred_utimes_ino(sched_get_current(), fi, (uint64_t)a1, (uint64_t)a2); }
        else { char kq[256]; if (copy_user_str((const char*)a0, kq, sizeof(kq)) != 0) return (uint64_t)(int64_t)-EFAULT;
               proc_resolve_path(sched_get_current(), kq, kp, sizeof(kp)); }  // cwd-relative
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
        char kdev[64], ktgt[256], kfs[32], kopt[128];
        if (copy_user_str((const char*)a0, kdev, sizeof(kdev)) != 0) return (uint64_t)(int64_t)-EFAULT;
        if (copy_user_str((const char*)a1, ktgt, sizeof(ktgt)) != 0) return (uint64_t)(int64_t)-EFAULT;
        if (copy_user_str((const char*)a2, kfs,  sizeof(kfs))  != 0) return (uint64_t)(int64_t)-EFAULT;
        kopt[0] = 0;   // [M43] a3: mount options (FAT32/exFAT/NTFS: uid=,gid=,umask=,dmask=,fmask=)
        if (a3 && copy_user_str((const char*)a3, kopt, sizeof(kopt)) != 0) return (uint64_t)(int64_t)-EFAULT;
        return (uint64_t)(int64_t)ksys_mount_opts(kdev, ktgt, kfs, kopt);
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
        (void)kiface_apply_create_mode;
        return (uint64_t)(int64_t)ksys_mkdir_mode(ab, (uint32_t)a1);   // [M43] owner + mode & ~umask
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
     * kernel/syscall_trap.c (no ring-0 spinning); so are connect()'s
     * -EINPROGRESS/-EALREADY while the TCP handshake runs [M47]. */
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
            { uint64_t fl = fdt_lock(c); c->fds[fd].is_sock = sx + 1; fdt_unlock(c, fl); }
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
            if ((uint16_t)a1 != 0 && (uint16_t)a1 < 1024 && !cred_is_root(c))
                return (uint64_t)(int64_t)-13;                          // [M43] -EACCES: privileged port
            return (uint64_t)(int64_t)socket_bind(sx, (uint16_t)a1);
        case SYS_LISTEN:
            return (uint64_t)(int64_t)socket_listen(sx, (int)a1);
        case SYS_ACCEPT: {
            int nfd = fd_alloc(c);
            if (nfd < 0) return (uint64_t)(int64_t)-24;                                  /* -EMFILE */
            int nsx = socket_accept(sx);
            if (nsx < 0) { fd_release(c, nfd); return (uint64_t)(int64_t)nsx; }        /* -EAGAIN: re-run */
            { uint64_t fl = fdt_lock(c); c->fds[nfd].is_sock = nsx + 1; fdt_unlock(c, fl); }
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

    case SYS_DRIVER:
        /* [M57] The M11 Driver Space mock (synthetic devices, shadow registers)
         * is retired: SYS_UDRV (92) drives real PCI functions from ring 3. */
        return (uint64_t)(int64_t)-38;    /* -ENOSYS */

    case SYS_UDRV:
        return (uint64_t)(int64_t)udrv_syscall(a0, a1, a2, a3);

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
    case SYS_SIGVERIFY: {                     // [U6] Ed25519 over a digest, kernel trust root
        uint8_t dg[32], sg[64];
        if (!user_range_valid((const void*)a0, sizeof(dg)) || copy_from_user(dg, (const void*)a0, sizeof(dg)) != 0 ||
            !user_range_valid((const void*)a1, sizeof(sg)) || copy_from_user(sg, (const void*)a1, sizeof(sg)) != 0)
            return (uint64_t)(int64_t)-EFAULT;
        extern int secos_sig_verify_digest(const uint8_t*, const uint8_t*, uint32_t);
        return secos_sig_verify_digest(dg, sg, (uint32_t)a2) == 0 ? 0 : (uint64_t)(int64_t)-74;   // -EBADMSG
    }
    case SYS_PRCTL: {                         // [M46] no_new_privs + capability query
        process_t* c = sched_get_current();
        if (!c) return (uint64_t)(int64_t)-22;
        if (a0 == PR_SET_NO_NEW_PRIVS) {
            if (a1 != 1) return (uint64_t)(int64_t)-22;   // can never be cleared
            c->no_new_privs = 1;
            debugcon_writestring("[CAPX] pid="); debugcon_print_hex(c->pid);
            debugcon_writestring(" set no_new_privs\n");
            return 0;
        }
        if (a0 == PR_GET_NO_NEW_PRIVS) return (uint64_t)(c->no_new_privs ? 1 : 0);
        if (a0 == PR_SECOS_GET_CAPS)
            return (uint64_t)(cap_effective(c) | (c->cap_enforce ? (1u << 30) : 0)
                              | (c->no_new_privs ? (1u << 29) : 0));
        return (uint64_t)(int64_t)-22;
    }
    case SYS_SCHED_GETAFFINITY: {
        uint64_t mask[SMP_MAX_CPUS / 64];
        uint32_t words = (smp_cpu_count() + 63) / 64;
        if (words == 0) words = 1;
        if (words > SMP_MAX_CPUS / 64) words = SMP_MAX_CPUS / 64;
        uint64_t need = (uint64_t)words * 8;
        if (a1 < need || (a1 & 7)) return (uint64_t)(int64_t)-22;   // -EINVAL
        uint32_t pid = (uint32_t)a0;
        if ((int)a0 < 0) return (uint64_t)(int64_t)-3;    // -ESRCH
        if (pid == 0) { process_t* c = sched_get_current(); if (!c) return (uint64_t)(int64_t)-3; pid = c->pid; }
        if (process_get_affinity(pid, mask, words) != 0) return (uint64_t)(int64_t)-3;
        if (!user_range_valid((void*)a2, need) || copy_to_user((void*)a2, mask, need) != 0)
            return (uint64_t)(int64_t)-EFAULT;
        return need;
    }
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

    case SYS_GETUID: case SYS_GETEUID: case SYS_GETGID: case SYS_GETEGID:
    case SYS_SETUID: case SYS_SETGID: case SYS_SETRESUID: case SYS_SETRESGID:
    case SYS_GETGROUPS: case SYS_SETGROUPS:                 // [M43] kernel/cred.c
        return (uint64_t)(int64_t)cred_syscall(num, a0, a1, a2);

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
