/*
 * SecOS Kernel - Anonymous pipes
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * [M25] A unidirectional in-kernel byte stream backed by a fixed ring buffer.
 * pipe_alloc() returns an object with one reader and one writer reference; the
 * two ends are exposed to user space as a read fd and a write fd (SYS_PIPE).
 * read/write are non-blocking here (return -2 == "would block"); the syscall
 * trap layer turns -2 into a real block on the pipe (sched_wake_pipe wakes it).
 */
#ifndef PIPE_H
#define PIPE_H
#include <stdint.h>

typedef struct pipe pipe_t;

/* POSIX PIPE_BUF: a write of <= PIPE_BUF bytes is atomic (all or would-block,
 * never split). Equal to the ring size, so such a write always fits eventually. */
#define PIPE_BUF 4096
#ifndef EPIPE
#define EPIPE 32        /* write() with no reader left (returned as -EPIPE) */
#endif

pipe_t* pipe_alloc(void);                          /* readers=writers=1, or NULL */
int  pipe_read(pipe_t* p, void* buf, int len);     /* >0 bytes; 0 EOF; -2 would-block */
/* len <= PIPE_BUF: all len bytes or -2 (would block); larger: what fits (>0).
 * -1 EPIPE (+SIGPIPE) when no reader is left. */
int  pipe_write(pipe_t* p, const void* buf, int len);
/* Non-atomic variant for the chunks of a write larger than PIPE_BUF: stores
 * whatever fits (>0), -2 only when the ring is full, -1 EPIPE. */
int  pipe_write_some(pipe_t* p, const void* buf, int len);
void pipe_ref(pipe_t* p, int writer);              /* bump reader (0) / writer (1) count */
void pipe_unref(pipe_t* p, int writer);            /* drop ref; wake peers; free when both 0 */

#endif /* PIPE_H */
