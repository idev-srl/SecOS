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
#include "debugcon.h"
#include "../mm/user_copy.h"

static int fd_alloc(process_t* p){ for(int i=0;i<32;i++){ if(!p->fds[i].used){ p->fds[i].used=1; p->fds[i].offset=0; p->fds[i].flags=0; p->fds[i].inode=NULL; return i; } } return -1; }

int ksys_getpid(void){ extern process_t* sched_get_current(void); process_t* c=sched_get_current(); return c? (int)c->pid : 0; }

// [M10] Load + signature-verify a signed ELF from a VFS path and create a
// process for it (PROC_READY so the scheduler can pick it up). The signing
// gate inside process_create_from_elf refuses unsigned/tampered images.
static uint8_t g_spawn_buf[65536];
int ksys_spawn(const char* path){
    extern int vfs_read_all(const char*, void*, size_t);
    extern process_t* process_create_from_elf(const void*, size_t);
    if(!path) return -1;
    int n = vfs_read_all(path, g_spawn_buf, sizeof(g_spawn_buf));
    if(n <= 0) return -1;
    process_t* p = process_create_from_elf(g_spawn_buf, (size_t)n);
    if(!p) return -1;
    p->state = PROC_READY;
    return (int)p->pid;
}
// Returns 0 if the pid has exited (ZOMBIE) or is gone; 1 if still running.
int ksys_wait(int pid){
    extern process_t* process_find_by_pid(uint32_t);
    process_t* t = process_find_by_pid((uint32_t)pid);
    if(!t) return 0;
    return (t->state == PROC_ZOMBIE) ? 0 : 1;
}
void ksys_exit(int status){ (void)status; extern process_t* sched_get_current(void); extern int process_destroy(process_t*); process_t* c=sched_get_current(); if(c){ process_destroy(c); } }
int ksys_open(const char* path, int flags){ (void)flags; extern vfs_inode_t* vfs_lookup(const char*); extern process_t* sched_get_current(void); process_t* c=sched_get_current(); if(!c) return -1; vfs_inode_t* ino=vfs_lookup(path); if(!ino) return -1; int fd=fd_alloc(c); if(fd<0) return -1; c->fds[fd].inode=ino; c->fds[fd].flags=flags; return fd; }
int ksys_close(int fd){ extern process_t* sched_get_current(void); process_t* c=sched_get_current(); if(!c) return -1; if(fd<0||fd>=32) return -1; if(!c->fds[fd].used) return -1; c->fds[fd].used=0; c->fds[fd].inode=NULL; return 0; }
int ksys_read(int fd, void* buf, int len){ extern process_t* sched_get_current(void); process_t* c=sched_get_current(); if(!c) return -1; if(fd<0||fd>=32||!c->fds[fd].used) return -1; vfs_inode_t* ino=(vfs_inode_t*)c->fds[fd].inode; if(!ino) return -1; if(!ino->ops||!ino->ops->read) return -1; size_t off=c->fds[fd].offset; int r=ino->ops->read(ino, off, buf, (size_t)len); if(r>0) c->fds[fd].offset += (uint64_t)r; return r; }
int ksys_write(int fd, const void* buf, int len){ extern process_t* sched_get_current(void); process_t* c=sched_get_current(); if(!c) return -1;
    // [M9] fd 1 (stdout) / 2 (stderr) -> console. The buffer was already
    // user_range_valid()'d by the SYS_WRITE dispatcher; CR3 is the caller's
    // address space during the syscall, so reading it directly is safe.
    if(fd==1||fd==2){ const char* p=(const char*)buf; for(int i=0;i<len;i++){ terminal_putchar(p[i]); debugcon_putchar(p[i]); } return len; }
    if(fd<0||fd>=32||!c->fds[fd].used) return -1; vfs_inode_t* ino=(vfs_inode_t*)c->fds[fd].inode; if(!ino) return -1; if(!ino->ops||!ino->ops->write) return -1; size_t off=c->fds[fd].offset; int r=ino->ops->write(ino, off, buf, (size_t)len); if(r>0) c->fds[fd].offset += (uint64_t)r; return r; }

uint64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4){
    (void)a3; (void)a4;
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
        return (uint64_t)(int64_t)ksys_spawn(kpath);
    }

    case SYS_WAIT:
        return (uint64_t)(int64_t)ksys_wait((int)a0);

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
