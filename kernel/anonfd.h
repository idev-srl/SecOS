/*
 * anonfd.h — [U10] anonymous-inode file descriptors.
 * SPDX-License-Identifier: MIT
 *
 * Kernel objects that live behind an fd but in no filesystem: eventfd,
 * timerfd, epoll instances and AF_UNIX sockets (Linux calls them anon
 * inodes). Each object embeds an anonfd_t as its first member; the fd entry's
 * `inode` points at anonfd_t.ino, and a vfs_inode_t is recognised as one of
 * ours by its ops pointer (anonfd_of). fstat sees the inode's mode/ino, so a
 * socket reports S_IFSOCK, an eventfd a regular anon file, as on Linux.
 *
 * Lifetime: `refs` counts fd-table references (dup, fork, SCM later) and
 * internal ones (an AF_UNIX peer, a pending connection). The last
 * anonfd_unref() calls ops->release, outside every lock; release frees the
 * object.
 *
 * Blocking: ops->read/write return -EAGAIN (-11) when they would block. The
 * syscall layer (anonfd_read/anonfd_write) then returns -EAGAIN for a
 * non-blocking fd, else arms c->wait_pipe = the object (and the op may arm
 * c->sleep_until for a deadline) and returns -2, which kernel/syscall_trap.c
 * turns into block + re-run. A state change calls anonfd_wake(), which wakes
 * those waiters and every poll()/epoll sleeper (sched_wake_pipe).
 */
#ifndef ANONFD_H
#define ANONFD_H
#include <stdint.h>
#include <stddef.h>
#include "../fs/vfs.h"

struct process;
typedef struct anonfd anonfd_t;

typedef struct anonfd_ops {
    const char* name;          /* /proc/<pid>/fd target, e.g. "anon_inode:[eventfd]" */
    uint32_t    mode;          /* S_IF* | permission bits reported by fstat */
    int  (*read)(struct process* c, anonfd_t* f, void* ubuf, int len, int nonblock);
    int  (*write)(struct process* c, anonfd_t* f, const void* ubuf, int len, int nonblock);
    int  (*poll)(anonfd_t* f);                     /* Linux revents bits, never blocks */
    long (*ioctl)(struct process* c, anonfd_t* f, uint64_t cmd, uint64_t arg);  /* NULL: -ENOTTY */
    void (*release)(anonfd_t* f);                  /* last reference: free the object */
} anonfd_ops_t;

struct anonfd {
    vfs_inode_t        ino;    /* must stay first: fds[].inode points here */
    const anonfd_ops_t* ops;
    volatile int       refs;
};

/* Set up an embedded anonfd (refs = 1). */
void       anonfd_init(anonfd_t* f, const anonfd_ops_t* ops);
/* The anonfd behind an fd inode pointer, or NULL for any other inode. */
anonfd_t*  anonfd_of(const void* ino);
static inline int anonfd_is(const void* ino) { return anonfd_of(ino) != 0; }
/* Same, but only for objects of this kind. */
static inline anonfd_t* anonfd_of_kind(const void* ino, const anonfd_ops_t* ops) {
    anonfd_t* f = anonfd_of(ino);
    return (f && f->ops == ops) ? f : 0;
}
/* References; both ignore an inode that is not an anonfd (callers pass any fd inode). */
void       anonfd_ref(void* ino);
void       anonfd_unref(void* ino);
/* Wake every task blocked on f (read/write) and every poll/epoll sleeper. */
void       anonfd_wake(anonfd_t* f);

/* Syscall-layer helpers for an fd whose inode is an anonfd. */
int  anonfd_read(struct process* c, int fd, void* ubuf, int len);
int  anonfd_write(struct process* c, int fd, const void* ubuf, int len);
int  anonfd_poll(void* ino);
long anonfd_ioctl(struct process* c, int fd, uint64_t cmd, uint64_t arg);

/* Install a new fd for f (consumes the caller's reference on success). flags:
 * O_NONBLOCK (0x800) kept in the fd status flags, cloexec = FD_CLOEXEC.
 * Returns the fd or -EMFILE (the reference is then still the caller's). */
int  anonfd_install(struct process* c, anonfd_t* f, uint32_t flags, int cloexec);

#endif /* ANONFD_H */
