/*
 * SecOS Kernel - Minimal IPC channels (M13)
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "ipc.h"
#include "terminal.h"
#include "spinlock.h"

typedef struct {
    uint8_t  buf[IPC_CHAN_BYTES];
    uint32_t head;   /* next write index */
    uint32_t tail;   /* next read index  */
    uint32_t count;  /* bytes currently queued */
} ipc_channel_t;

#include <stdint.h>
static ipc_channel_t g_chan[IPC_NUM_CHANNELS];
static int g_inited;
static spinlock_t g_ipc_lock = SPINLOCK_INIT;   // guards g_chan[] rings

void ipc_init(void) {
    for (int i = 0; i < IPC_NUM_CHANNELS; i++) {
        g_chan[i].head = g_chan[i].tail = g_chan[i].count = 0;
    }
    g_inited = 1;
    terminal_writestring("[IPC] channels initialized\n");
}

int ipc_send(int chan, const void* buf, int len) {
    if (!g_inited) ipc_init();
    if (chan < 0 || chan >= IPC_NUM_CHANNELS || !buf || len < 0) return -1;
    ipc_channel_t* c = &g_chan[chan];
    const uint8_t* p = (const uint8_t*)buf;
    int n = 0;
    uint64_t fl = spin_lock_irqsave(&g_ipc_lock);
    while (n < len && c->count < IPC_CHAN_BYTES) {
        c->buf[c->head] = p[n++];
        c->head = (c->head + 1) % IPC_CHAN_BYTES;
        c->count++;
    }
    spin_unlock_irqrestore(&g_ipc_lock, fl);
    return n; /* bytes accepted (may be < len if the ring filled) */
}

int ipc_recv(int chan, void* buf, int len) {
    if (!g_inited) ipc_init();
    if (chan < 0 || chan >= IPC_NUM_CHANNELS || !buf || len < 0) return -1;
    ipc_channel_t* c = &g_chan[chan];
    uint8_t* p = (uint8_t*)buf;
    int n = 0;
    uint64_t fl = spin_lock_irqsave(&g_ipc_lock);
    while (n < len && c->count > 0) {
        p[n++] = c->buf[c->tail];
        c->tail = (c->tail + 1) % IPC_CHAN_BYTES;
        c->count--;
    }
    spin_unlock_irqrestore(&g_ipc_lock, fl);
    return n; /* 0 when empty (non-blocking) */
}
