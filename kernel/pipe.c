/*
 * SecOS Kernel - Anonymous pipes
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * [M25] See pipe.h. A small static pool of fixed-size ring buffers; blocking is
 * implemented one level up (the syscall trap layer blocks on -2 and re-runs the
 * read/write when sched_wake_pipe() flips the task back to READY).
 */
#include "pipe.h"
#include <stddef.h>

#define PIPE_SIZE 4096
#define PIPE_MAX  16

struct pipe {
    int      used;
    uint8_t  buf[PIPE_SIZE];
    uint32_t head, len;     /* ring: read index, valid byte count */
    int      readers, writers;
};

static struct pipe g_pipes[PIPE_MAX];

extern void sched_wake_pipe(void* p);

pipe_t* pipe_alloc(void) {
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!g_pipes[i].used) {
            struct pipe* p = &g_pipes[i];
            p->used = 1; p->head = 0; p->len = 0;
            p->readers = 1; p->writers = 1;
            return p;
        }
    }
    return NULL;
}

int pipe_read(pipe_t* p, void* buf, int len) {
    if (!p || len <= 0) return -1;
    if (p->len == 0) {
        if (p->writers == 0) return 0;     /* EOF: drained and all write ends closed */
        return -2;                          /* empty but writers live -> would block */
    }
    uint8_t* d = (uint8_t*)buf;
    uint32_t n = (uint32_t)len; if (n > p->len) n = p->len;
    for (uint32_t i = 0; i < n; i++) d[i] = p->buf[(p->head + i) % PIPE_SIZE];
    p->head = (p->head + n) % PIPE_SIZE; p->len -= n;
    sched_wake_pipe(p);                     /* space freed -> wake blocked writers */
    return (int)n;
}

int pipe_write(pipe_t* p, const void* buf, int len) {
    if (!p || len <= 0) return -1;
    if (p->readers == 0) return -1;         /* EPIPE: no reader can ever consume */
    uint32_t space = PIPE_SIZE - p->len;
    if (space == 0) return -2;              /* full but readers live -> would block */
    const uint8_t* s = (const uint8_t*)buf;
    uint32_t n = (uint32_t)len; if (n > space) n = space;  /* short write is allowed */
    uint32_t tail = (p->head + p->len) % PIPE_SIZE;
    for (uint32_t i = 0; i < n; i++) p->buf[(tail + i) % PIPE_SIZE] = s[i];
    p->len += n;
    sched_wake_pipe(p);                     /* data available -> wake blocked readers */
    return (int)n;
}

void pipe_ref(pipe_t* p, int writer) {
    if (!p) return;
    if (writer) p->writers++; else p->readers++;
}

void pipe_unref(pipe_t* p, int writer) {
    if (!p) return;
    if (writer) { if (p->writers > 0) p->writers--; }
    else        { if (p->readers > 0) p->readers--; }
    sched_wake_pipe(p);                     /* peer may now observe EOF / EPIPE */
    if (p->readers == 0 && p->writers == 0) p->used = 0;  /* both ends gone -> free */
}
