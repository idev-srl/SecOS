/*
 * SecOS Kernel - Pseudo-terminals (/dev/ptmx, /dev/pts/N)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * A pty is a pair of character devices joined by the shared line discipline
 * (kernel/tty.c). Opening /dev/ptmx allocates one and returns its MASTER; the
 * SLAVE is /dev/pts/N (N from TIOCGPTN), locked until TIOCSPTLCK(0)
 * (unlockpt). Master write -> slave input (through the ldisc: ICRNL, echo,
 * ISIG, canonical editing...); slave output (OPOST/ONLCR applied) -> master
 * read. A pty is the controlling terminal of at most one session.
 *
 * File descriptors: a pty fd stores the master or slave vfs_inode_t embedded in
 * the pty object (fds[].inode, is_pipe=0), so stat/fstat/lseek treat it as the
 * character device it is. Every fd that holds one counts a reference
 * (pty_ref/pty_unref at open, dup, fork, spawn-inherit, close, exit), as does a
 * process that has it as its controlling terminal (pty_ctty_ref). The last
 * master close hangs the slave up (SIGHUP+SIGCONT to its foreground group and
 * session leader, slave read -> EIO after draining, write -> EIO); the last
 * slave close makes master reads return EIO once drained. The object is freed
 * when no fd and no controlling-terminal holder is left.
 */
#ifndef PTY_H
#define PTY_H
#include <stdint.h>
#include "vfs.h"

#define PTY_MAX 64
/* Blocking on "any pty changed" (for a future poll/select): set
 * current->wait_pty = PTY_WAIT_ANY and block; every pty state change (data in,
 * data out, room freed, hangup, close) and the periodic tty tick wake it. */
#define PTY_WAIT_ANY ((void*)1)

/* poll(2) event bits (Linux values) for pty_poll / tty_console_poll. */
#define PTY_POLLIN  0x001
#define PTY_POLLOUT 0x004
#define PTY_POLLERR 0x008
#define PTY_POLLHUP 0x010

#define PTY_KIND_NONE   0
#define PTY_KIND_MASTER 1
#define PTY_KIND_SLAVE  2

struct process;
struct tty;

int  pty_kind(const void* inode);          /* PTY_KIND_* */
int  pty_is_inode(const void* inode);      /* master or slave inode */
void pty_ref(void* inode);                 /* +1 fd reference (no-op for a non-pty inode) */
void pty_unref(void* inode);               /* -1 fd reference (hangup / EIO / free) */
void pty_ctty_ref(void* tty);              /* +1 controlling-terminal holder (NULL ok) */
void pty_proc_exit(struct process* p);     /* drop p's ctty; a session leader hangs it up */
void pty_inherit_ctty(struct process* child, const struct process* parent); /* spawn */

/* The controlling terminal of p: a pty slave's struct tty, or NULL. With NULL,
 * *is_console says whether that means the console (1) or none at all (0). */
struct tty* pty_proc_ctty(struct process* p, int* is_console);

/* ksys_open hook: if *ino is /dev/ptmx, /dev/tty or /dev/pts/N, substitute the
 * real terminal inode (a new master, the caller's controlling terminal, the
 * slave) and take its reference; a session leader without a controlling
 * terminal opening a slave without O_NOCTTY acquires it. 0 or -errno. */
int  pty_open_hook(struct process* c, vfs_inode_t** ino, int flags);

/* I/O on a pty fd. >0 bytes, -EIO, -EAGAIN (nonblock), -EINTR (job control),
 * or -2: would block, current->wait_pty armed (the syscall layer blocks). */
int  pty_read(void* inode, void* buf, int len, int nonblock);
int  pty_write(void* inode, const void* buf, int len, int nonblock);
/* ioctl on a pty fd: termios, window size, queues, job control, sessions,
 * TIOCGPTN/TIOCSPTLCK/TIOCGPTLCK. 0 / -errno; -515 = unknown request. */
int  pty_ioctl(struct process* c, void* inode, uint64_t cmd, uint64_t arg);
/* Readiness for poll/select: the subset of `events` (PTY_POLL*) that holds now,
 * plus POLLHUP/POLLERR. -1 if the inode is not a pty. */
int  pty_poll(void* inode, int events);
int  tty_console_poll(int events);

/* Console session ioctls (TIOCSCTTY/TIOCNOTTY on the console). */
int  pty_console_sctty(struct process* c);
int  pty_console_notty(struct process* c);

long ksys_setsid(void);                    /* SYS_SETSID (85) */
long ksys_getsid(int pid);                 /* SYS_GETSID (86) */

/* devfs glue: "/pts" and "/pts/N" lookups and the /pts listing. */
vfs_inode_t* pty_devfs_lookup(const char* rel);
int  pty_devfs_setattr(const char* rel, const vfs_attr_t* a, unsigned valid);   /* 1: not /pts/N */
int  pty_devfs_readdir(vfs_iter_cb cb, void* user);
extern vfs_inode_t g_pty_dir_inode;        /* /dev/pts */

#endif /* PTY_H */
