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

extern uint64_t timer_get_ticks(void);

// [M16] Bounds for SYS_SPAWN argv copy-in (kept small; argstore is static).
#define SPAWN_MAX_ARGS 8
#define SPAWN_ARG_LEN  128

static int fd_alloc(process_t* p){ for(int i=0;i<32;i++){ if(!p->fds[i].used){ p->fds[i].used=1; p->fds[i].offset=0; p->fds[i].flags=0; p->fds[i].inode=NULL; p->fds[i].is_pipe=0; p->fds[i].pipe_w=0; return i; } } return -1; }

// [M25] pipe(): allocate a pipe object and two fds (read end + write end).
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

int ksys_getpid(void){ extern process_t* sched_get_current(void); process_t* c=sched_get_current(); return c? (int)c->pid : 0; }

// [M10] Load + signature-verify a signed ELF from a VFS path and create a
// process for it (PROC_READY so the scheduler can pick it up). The signing
// gate inside process_create_from_elf refuses unsigned/tampered images.
static uint8_t g_spawn_buf[65536];
// [M16] Spawn with argv. The single static load buffer is safe because kernel
// code is non-preemptible (preemption fires only on a ring-3 timer tick) and
// process_create_from_elf_args copies/pins the image before returning.
int ksys_spawn_argv(const char* path, int argc, const char* const argv[]){
    extern int vfs_read_all(const char*, void*, size_t);
    if(!path) return -1;
    int n = vfs_read_all(path, g_spawn_buf, sizeof(g_spawn_buf));
    if(n <= 0) return -1;
    process_t* p = process_create_from_elf_args(g_spawn_buf, (size_t)n, argc, argv);
    if(!p) return -1;
    p->state = PROC_READY;
    return (int)p->pid;
}
int ksys_spawn(const char* path){ return ksys_spawn_argv(path, 0, (const char* const*)0); }

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
    uint64_t f = VMM_FLAG_USER;
    if (prot & PROT_WRITE) f |= VMM_FLAG_RW;
    if (!(prot & PROT_EXEC)) f |= VMM_FLAG_NOEXEC; // NX unless EXEC requested
    return f;
}
static inline uint64_t pgup(uint64_t x){ return (x + 0xFFFULL) & ~0xFFFULL; }

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
        vma_t* h = vma_find_mut(&c->vmas, c->brk_start);
        if (h) h->end = new_brk;
        else if (vma_add(&c->vmas, c->brk_start, new_brk,
                         VMM_FLAG_USER|VMM_FLAG_RW|VMM_FLAG_NOEXEC, VMA_TYPE_ANON,0,0,0,0) != 0)
            return old;
    } else {                                        // shrink
        for (uint64_t va = new_brk; va < old; va += 0x1000) vmm_unmap_in_space(c->space, va);
        vma_t* h = vma_find_mut(&c->vmas, c->brk_start);
        if (h) { if (new_brk <= c->brk_start) vma_remove(&c->vmas, h); else h->end = new_brk; }
    }
    c->brk_cur = new_brk;
    return new_brk;
}

uint64_t ksys_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd){
    (void)addr;                                     // hint ignored (bump allocator)
    process_t* c = sched_get_current();
    if (!c || len == 0) return (uint64_t)-1;
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) return (uint64_t)-1; // W^X
    len = pgup(len);
    uint64_t base = c->mmap_next;
    if (base + len > USER_MMAP_END) return (uint64_t)-1;      // arena exhausted
    if (c->mem_limit && vma_total_bytes(&c->vmas) + len > c->mem_limit) return (uint64_t)-1;
    if (flags & MAP_ANONYMOUS) {
        if (vma_add(&c->vmas, base, base + len, m18_prot_to_flags(prot), VMA_TYPE_ANON,0,0,0,0) != 0)
            return (uint64_t)-1;
    } else {
        // [M20] File-backed mmap (MAP_PRIVATE) via the page cache, from offset 0.
        if (fd < 0 || fd >= 32 || !c->fds[fd].used) return (uint64_t)-1;
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
    if (!c || (addr & 0xFFF) || len == 0) return -1;
    len = pgup(len);
    uint64_t end = addr + len;
    for (uint64_t va = addr; va < end; va += 0x1000) vmm_unmap_in_space(c->space, va);
    for (uint32_t i = 0; i < c->vmas.count; i++) {
        vma_t* v = &c->vmas.v[i];
        if (v->type == VMA_TYPE_NONE) continue;
        if (!(addr < v->end && v->start < end)) continue;        // no overlap
        if (addr <= v->start && end >= v->end)      vma_remove(&c->vmas, v);   // fully covered
        else if (addr <= v->start)                  v->start = end;            // trim front
        else if (end >= v->end)                     v->end = addr;             // trim back
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
    if (!c || (addr & 0xFFF) || len == 0) return -1;
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) return -1;   // W^X
    len = pgup(len);
    uint64_t end = addr + len;
    if (!vma_overlaps(&c->vmas, addr, end)) return -1;          // must be backed
    uint64_t newf = m18_prot_to_flags(prot);
    for (uint32_t i = 0; i < c->vmas.count; i++) {
        vma_t* v = &c->vmas.v[i];
        if (v->type == VMA_TYPE_NONE) continue;
        if (addr <= v->start && end >= v->end && addr < v->end && v->start < end)
            v->flags = newf;                                     // whole VMA in range
    }
    for (uint64_t va = addr; va < end; va += 0x1000) vmm_protect_in_space(c->space, va, newf);
    return 0;
}
void ksys_exit(int status){ (void)status; extern process_t* sched_get_current(void); extern int process_destroy(process_t*); process_t* c=sched_get_current(); if(c){ process_destroy(c); } }
int ksys_open(const char* path, int flags){ (void)flags; extern vfs_inode_t* vfs_lookup(const char*); extern process_t* sched_get_current(void); process_t* c=sched_get_current(); if(!c) return -1; vfs_inode_t* ino=vfs_lookup(path); if(!ino) return -1; int fd=fd_alloc(c); if(fd<0) return -1; c->fds[fd].inode=ino; c->fds[fd].flags=flags; return fd; }
int ksys_close(int fd){ extern process_t* sched_get_current(void); process_t* c=sched_get_current(); if(!c) return -1; if(fd<0||fd>=32) return -1; if(!c->fds[fd].used) return -1;
    if(c->fds[fd].is_pipe){ pipe_unref((pipe_t*)c->fds[fd].inode, c->fds[fd].pipe_w); c->fds[fd].is_pipe=0; }  // [M25] drop end, may signal EOF/EPIPE
    c->fds[fd].used=0; c->fds[fd].inode=NULL; return 0; }
int ksys_read(int fd, void* buf, int len){ extern process_t* sched_get_current(void); process_t* c=sched_get_current(); if(!c) return -1;
    // [M25] fd 0 (stdin) -> the console TTY cooked line discipline (ring-3 input).
    if(fd==0){ extern int tty_read(void* buf, int len); return tty_read(buf, len); }
    if(fd<0||fd>=32||!c->fds[fd].used) return -1;
    if(c->fds[fd].is_pipe){ if(c->fds[fd].pipe_w) return -1; return pipe_read((pipe_t*)c->fds[fd].inode, buf, len); } // [M25] read end only
    vfs_inode_t* ino=(vfs_inode_t*)c->fds[fd].inode; if(!ino) return -1; if(!ino->ops||!ino->ops->read) return -1;
    // [M20] Read through the unified page cache so file read() and file-backed
    // mmap see the same pages (coherent). [M23] Virtual FSes (devfs/procfs/sysfs)
    // are dynamic — bypass the cache and read live via ->read().
    size_t off=c->fds[fd].offset; int r;
    if(ino->ops->flags & VFS_FS_NOCACHE) r=ino->ops->read(ino, off, buf, (size_t)len);
    else r=pagecache_read(ino, off, buf, (uint64_t)len);
    if(r>0) c->fds[fd].offset += (uint64_t)r; return r; }
int ksys_write(int fd, const void* buf, int len){ extern process_t* sched_get_current(void); process_t* c=sched_get_current(); if(!c) return -1;
    // [M9] fd 1 (stdout) / 2 (stderr) -> console. The buffer was already
    // user_range_valid()'d by the SYS_WRITE dispatcher; CR3 is the caller's
    // address space during the syscall, so reading it directly is safe.
    if(fd==1||fd==2){ const char* p=(const char*)buf; for(int i=0;i<len;i++){ terminal_putchar(p[i]); debugcon_putchar(p[i]); } return len; }
    if(fd<0||fd>=32||!c->fds[fd].used) return -1;
    if(c->fds[fd].is_pipe){ if(!c->fds[fd].pipe_w) return -1; return pipe_write((pipe_t*)c->fds[fd].inode, buf, len); } // [M25] write end only
    vfs_inode_t* ino=(vfs_inode_t*)c->fds[fd].inode; if(!ino) return -1; if(!ino->ops||!ino->ops->write) return -1; size_t off=c->fds[fd].offset; int r=ino->ops->write(ino, off, buf, (size_t)len); if(r>0){ c->fds[fd].offset += (uint64_t)r; pagecache_invalidate(ino, off, (uint64_t)r); } return r; }

long ksys_lseek(int fd, long offset, int whence){
    extern process_t* sched_get_current(void);
    process_t* c=sched_get_current(); if(!c) return -1;
    if(fd<0||fd>=32||!c->fds[fd].used) return -1;
    vfs_inode_t* ino=(vfs_inode_t*)c->fds[fd].inode; if(!ino) return -1;
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

int ksys_stat(const char* path, struct secos_stat* st){
    extern vfs_inode_t* vfs_lookup(const char*);
    vfs_inode_t* ino=vfs_lookup(path); if(!ino) return -1;
    st->st_size=ino->size;
    st->st_mode=(ino->type==VFS_NODE_DIR)?S_IFDIR:S_IFREG;
    st->st_pad=0;
    return 0;
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
        /* [M3] Validate path pointer is in user range (at least 1 byte). */
        const char* path = (const char*)a0;
        if (!user_range_valid(path, 1)) {
            terminal_writestring("[SYSCALL][M3] SYS_OPEN: invalid user path pointer\n");
            return (uint64_t)(int64_t)-EFAULT;
        }
        return (uint64_t)ksys_open(path, (int)a1);
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
        /* [M3] Validate and copy the user path string into the kernel. */
        const char* upath = (const char*)a0;
        char kpath[256]; int i = 0;
        for (; i < 255; i++) {
            if (!user_range_valid(upath + i, 1)) { return (uint64_t)(int64_t)-EFAULT; }
            char ch = upath[i];
            kpath[i] = ch;
            if (ch == 0) break;
        }
        kpath[i] = 0;
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
                int j = 0;
                for (; j < SPAWN_ARG_LEN - 1; j++) {
                    char ch;
                    if (copy_from_user(&ch, ustr + j, 1) != 0)
                        return (uint64_t)(int64_t)-EFAULT;
                    argstore[argc][j] = ch;
                    if (!ch) break;
                }
                argstore[argc][j] = 0;
                kargv[argc] = argstore[argc];
            }
        }
        return (uint64_t)(int64_t)ksys_spawn_argv(kpath, argc, kargv);
    }

    case SYS_GETTICKS:
        /* [M13] Uptime in timer ticks. No user pointers involved. */
        return timer_get_ticks();

    case SYS_MSG_SEND: {
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
        const char* path = (const char*)a0;
        if (!user_range_valid(path, 1)) return (uint64_t)(int64_t)-EFAULT;
        struct secos_stat st;
        int r = ksys_stat(path, &st);
        if (r != 0) return (uint64_t)(int64_t)r;
        if (copy_to_user((void*)a1, &st, sizeof(st)) != 0) return (uint64_t)(int64_t)-EFAULT;
        return 0;
    }

    case SYS_PIPE: {
        /* [M25] pipe(int fds[2]): kernel-side alloc, then copy the two fds out. */
        int kfds[2];
        int r = ksys_pipe(kfds);
        if (r != 0) return (uint64_t)(int64_t)r;
        if (copy_to_user((void*)a0, kfds, sizeof(kfds)) != 0) return (uint64_t)(int64_t)-EFAULT;
        return 0;
    }

    /* [M24] Sockets — gated by CAP_NET in the signed manifest. A staging buffer
     * bounces user payloads; blocking calls (connect/accept/recv) spin in the
     * socket layer with RX driven by the timer tick. */
    case SYS_SOCKET: case SYS_CONNECT: case SYS_BIND: case SYS_LISTEN:
    case SYS_ACCEPT: case SYS_SEND: case SYS_RECV: case SYS_SENDTO:
    case SYS_RECVFROM: case SYS_SOCKCLOSE: {
        process_t* c = sched_get_current();
        if (!c || !c->cap_net) {
            debugcon_writestring("[NET] socket syscall denied: no CAP_NET\n");
            return (uint64_t)(int64_t)-1;            /* EPERM-ish */
        }
        uint32_t pid = c->pid;
        static uint8_t netbuf[2048];
        switch (num) {
        case SYS_SOCKET:
            return (uint64_t)(int64_t)socket_create(pid, (int)a0);
        case SYS_SOCKCLOSE:
            return (uint64_t)(int64_t)socket_close(pid, (int)a0);
        case SYS_CONNECT:
            return (uint64_t)(int64_t)socket_connect(pid, (int)a0, (uint32_t)a1, (uint16_t)a2);
        case SYS_BIND:
            return (uint64_t)(int64_t)socket_bind(pid, (int)a0, (uint16_t)a1);
        case SYS_LISTEN:
            return (uint64_t)(int64_t)socket_listen(pid, (int)a0, (int)a1);
        case SYS_ACCEPT:
            return (uint64_t)(int64_t)socket_accept(pid, (int)a0);
        case SYS_SEND: {
            int len = (int)a2;
            if (len < 0 || len > (int)sizeof(netbuf) || !user_range_valid((void*)a1, (size_t)len))
                return (uint64_t)(int64_t)-EFAULT;
            if (copy_from_user(netbuf, (void*)a1, (size_t)len) != 0) return (uint64_t)(int64_t)-EFAULT;
            return (uint64_t)(int64_t)socket_send(pid, (int)a0, netbuf, len);
        }
        case SYS_SENDTO: {
            int len = (int)a2;
            if (len < 0 || len > (int)sizeof(netbuf) || !user_range_valid((void*)a1, (size_t)len))
                return (uint64_t)(int64_t)-EFAULT;
            struct secos_sockaddr sa;
            if (copy_from_user(&sa, (void*)a3, sizeof(sa)) != 0) return (uint64_t)(int64_t)-EFAULT;
            if (copy_from_user(netbuf, (void*)a1, (size_t)len) != 0) return (uint64_t)(int64_t)-EFAULT;
            return (uint64_t)(int64_t)socket_sendto(pid, (int)a0, netbuf, len, sa.ip, sa.port);
        }
        case SYS_RECV: {
            int len = (int)a2;
            if (len <= 0 || len > (int)sizeof(netbuf) || !user_range_valid((void*)a1, (size_t)len))
                return (uint64_t)(int64_t)-EFAULT;
            int n = socket_recv(pid, (int)a0, netbuf, len, 0, 0);
            if (n > 0 && copy_to_user((void*)a1, netbuf, (size_t)n) != 0) return (uint64_t)(int64_t)-EFAULT;
            return (uint64_t)(int64_t)n;
        }
        case SYS_RECVFROM: {
            int len = (int)a2;
            if (len <= 0 || len > (int)sizeof(netbuf) || !user_range_valid((void*)a1, (size_t)len))
                return (uint64_t)(int64_t)-EFAULT;
            uint32_t sip = 0; uint16_t sport = 0;
            int n = socket_recv(pid, (int)a0, netbuf, len, &sip, &sport);
            if (n > 0 && copy_to_user((void*)a1, netbuf, (size_t)n) != 0) return (uint64_t)(int64_t)-EFAULT;
            if (a3) {
                struct secos_sockaddr sa; sa.ip = sip; sa.port = sport; sa._pad = 0;
                copy_to_user((void*)a3, &sa, sizeof(sa));
            }
            return (uint64_t)(int64_t)n;
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

    default:
        terminal_writestring("[SYSCALL] sconosciuta\n");
        return (uint64_t)(int64_t)-1;
    }
}
