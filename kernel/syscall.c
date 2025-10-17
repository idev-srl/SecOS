#include "syscall.h"
#include "terminal.h"
#include "process.h"
#include "sched.h"
#include "vfs.h"

static int fd_alloc(process_t* p){ for(int i=0;i<32;i++){ if(!p->fds[i].used){ p->fds[i].used=1; p->fds[i].offset=0; p->fds[i].flags=0; p->fds[i].inode=NULL; return i; } } return -1; }

int ksys_getpid(void){ extern process_t* sched_get_current(void); process_t* c=sched_get_current(); return c? (int)c->pid : 0; }
void ksys_exit(int status){ (void)status; extern process_t* sched_get_current(void); extern int process_destroy(process_t*); process_t* c=sched_get_current(); if(c){ process_destroy(c); } }
int ksys_open(const char* path, int flags){ (void)flags; extern vfs_inode_t* vfs_lookup(const char*); extern process_t* sched_get_current(void); process_t* c=sched_get_current(); if(!c) return -1; vfs_inode_t* ino=vfs_lookup(path); if(!ino) return -1; int fd=fd_alloc(c); if(fd<0) return -1; c->fds[fd].inode=ino; c->fds[fd].flags=flags; return fd; }
int ksys_close(int fd){ extern process_t* sched_get_current(void); process_t* c=sched_get_current(); if(!c) return -1; if(fd<0||fd>=32) return -1; if(!c->fds[fd].used) return -1; c->fds[fd].used=0; c->fds[fd].inode=NULL; return 0; }
int ksys_read(int fd, void* buf, int len){ extern process_t* sched_get_current(void); process_t* c=sched_get_current(); if(!c) return -1; if(fd<0||fd>=32||!c->fds[fd].used) return -1; vfs_inode_t* ino=(vfs_inode_t*)c->fds[fd].inode; if(!ino) return -1; if(!ino->ops||!ino->ops->read) return -1; size_t off=c->fds[fd].offset; int r=ino->ops->read(ino, off, buf, (size_t)len); if(r>0) c->fds[fd].offset += (uint64_t)r; return r; }
int ksys_write(int fd, const void* buf, int len){ extern process_t* sched_get_current(void); process_t* c=sched_get_current(); if(!c) return -1; if(fd<0||fd>=32||!c->fds[fd].used) return -1; vfs_inode_t* ino=(vfs_inode_t*)c->fds[fd].inode; if(!ino) return -1; if(!ino->ops||!ino->ops->write) return -1; size_t off=c->fds[fd].offset; int r=ino->ops->write(ino, off, buf, (size_t)len); if(r>0) c->fds[fd].offset += (uint64_t)r; return r; }

uint64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4){ (void)a3; (void)a4; switch(num){
    case SYS_GETPID: return (uint64_t)ksys_getpid();
    case SYS_EXIT:   ksys_exit((int)a0); return 0;
    case SYS_OPEN:   return (uint64_t)ksys_open((const char*)a0, (int)a1);
    case SYS_CLOSE:  return (uint64_t)ksys_close((int)a0);
    case SYS_READ:   return (uint64_t)ksys_read((int)a0,(void*)a1,(int)a2);
    case SYS_WRITE:  return (uint64_t)ksys_write((int)a0,(const void*)a1,(int)a2);
    case SYS_DRIVER: return (uint64_t)driver_syscall((struct driver_call*)a0);
    default: terminal_writestring("[SYSCALL] sconosciuta\n"); return (uint64_t)-1; }
}