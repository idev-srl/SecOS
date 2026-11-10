/*
 * anonfd.c — [U10] anonymous-inode file descriptors (see anonfd.h).
 * SPDX-License-Identifier: MIT
 *
 * No lock of its own: the reference count is atomic, and every object kind
 * guards its state with its own lock. Nothing here runs with a lock held, so
 * anonfd_wake (proc_lock inside sched_wake_pipe) is safe from any caller that
 * holds none.
 */
#include "anonfd.h"
#include "process.h"
#include "user_copy.h"

extern void sched_wake_pipe(void* pp);
extern int  ksys_fd_install(process_t* c, void* ino, uint32_t flags, int cloexec);

#define A_EAGAIN  11
#define A_EBADF   9
#define A_EINVAL  22
#define A_ENOTTY  25
#define A_EMFILE  24
#define A_EFAULT  14
#define O_NONBLOCK_ 0x800
#define WOULDBLOCK (-2)

static int an_stub_read(vfs_inode_t* i, size_t o, void* b, size_t l) { (void)i; (void)o; (void)b; (void)l; return -1; }
static int an_stub_write(vfs_inode_t* i, size_t o, const void* b, size_t l) { (void)i; (void)o; (void)b; (void)l; return -1; }
/* Generic VFS paths that ever reach an anon inode find only harmless stubs. */
static const vfs_fs_ops_t g_anon_vfs_ops = { .flags = VFS_FS_NOCACHE, .read = an_stub_read, .write = an_stub_write };

static uint64_t g_anon_ino = 0x414E0000u;     /* st_ino of the next object */

void anonfd_init(anonfd_t* f, const anonfd_ops_t* ops) {
    uint8_t* p = (uint8_t*)&f->ino;
    for (size_t i = 0; i < sizeof(f->ino); i++) p[i] = 0;
    const char* nm = ops->name ? ops->name : "anon_inode:[?]";
    size_t k = 0;
    for (; nm[k] && k < sizeof(f->ino.path) - 1; k++) f->ino.path[k] = nm[k];
    f->ino.path[k] = 0;
    f->ino.type = VFS_NODE_FILE;
    f->ino.mode = ops->mode;
    f->ino.nlink = 1;
    f->ino.ino = __atomic_add_fetch(&g_anon_ino, 1, __ATOMIC_RELAXED);
    f->ino.ops = &g_anon_vfs_ops;
    f->ino.fs_data = f;
    f->ops = ops;
    f->refs = 1;
}

anonfd_t* anonfd_of(const void* ino) {
    const vfs_inode_t* v = (const vfs_inode_t*)ino;
    if (!v || v->ops != &g_anon_vfs_ops) return 0;
    return (anonfd_t*)v->fs_data;
}

void anonfd_ref(void* ino) {
    anonfd_t* f = anonfd_of(ino);
    if (f) __atomic_add_fetch(&f->refs, 1, __ATOMIC_SEQ_CST);
}

void anonfd_unref(void* ino) {
    anonfd_t* f = anonfd_of(ino);
    if (!f) return;
    if (__atomic_sub_fetch(&f->refs, 1, __ATOMIC_SEQ_CST) == 0 && f->ops->release)
        f->ops->release(f);
}

void anonfd_wake(anonfd_t* f) { sched_wake_pipe(f); }

static anonfd_t* fd_anon(process_t* c, int fd) {
    if (!c || fd < 0 || fd >= 32 || !c->fds[fd].used || c->fds[fd].is_pipe || c->fds[fd].is_sock) return 0;
    return anonfd_of(c->fds[fd].inode);
}

int anonfd_read(process_t* c, int fd, void* ubuf, int len) {
    anonfd_t* f = fd_anon(c, fd);
    if (!f) return -A_EBADF;
    if (!f->ops->read) return -A_EINVAL;
    int nb = (c->fds[fd].flags & O_NONBLOCK_) != 0;
    int r = f->ops->read(c, f, ubuf, len, nb);
    if (r == -A_EAGAIN && !nb) { c->wait_pipe = f; return WOULDBLOCK; }
    return r;
}

int anonfd_write(process_t* c, int fd, const void* ubuf, int len) {
    anonfd_t* f = fd_anon(c, fd);
    if (!f) return -A_EBADF;
    if (!f->ops->write) return -A_EINVAL;
    int nb = (c->fds[fd].flags & O_NONBLOCK_) != 0;
    int r = f->ops->write(c, f, ubuf, len, nb);
    if (r == -A_EAGAIN && !nb) { c->wait_pipe = f; return WOULDBLOCK; }
    return r;
}

int anonfd_poll(void* ino) {
    anonfd_t* f = anonfd_of(ino);
    if (!f) return 0x20;                        /* POLLNVAL */
    return f->ops->poll ? f->ops->poll(f) : (0x001 | 0x004);
}

#define FIONBIO_  0x5421
long anonfd_ioctl(process_t* c, int fd, uint64_t cmd, uint64_t arg) {
    anonfd_t* f = fd_anon(c, fd);
    if (!f) return -A_EBADF;
    if (cmd == FIONBIO_) {
        int v;
        if (copy_from_user(&v, (const void*)arg, sizeof(v)) != 0) return -A_EFAULT;
        if (v) c->fds[fd].flags |= O_NONBLOCK_; else c->fds[fd].flags &= ~(uint32_t)O_NONBLOCK_;
        return 0;
    }
    if (!f->ops->ioctl) return -A_ENOTTY;
    return f->ops->ioctl(c, f, cmd, arg);
}

int anonfd_install(process_t* c, anonfd_t* f, uint32_t flags, int cloexec) {
    int fd = ksys_fd_install(c, &f->ino, (flags & O_NONBLOCK_) | 2 /* O_RDWR */, cloexec);
    return fd < 0 ? -A_EMFILE : fd;
}
