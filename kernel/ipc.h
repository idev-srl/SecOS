/*
 * SecOS Kernel - Minimal IPC channels (M13)
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * A small fixed set of kernel-owned byte channels (ring buffers) addressed by a
 * small integer id. Any process may send/recv on a channel via SYS_MSG_SEND /
 * SYS_MSG_RECV; the kernel buffer persists independently of process lifetime,
 * so two separately-spawned ring-3 programs can communicate without fork/fd
 * inheritance. Non-blocking: recv returns 0 when the channel is empty.
 */
#ifndef SECOS_IPC_H
#define SECOS_IPC_H
#include <stddef.h>

#define IPC_NUM_CHANNELS 4
#define IPC_CHAN_BYTES   512   /* ring capacity per channel */

void ipc_init(void);
/* Enqueue up to len bytes; returns bytes accepted (0 if full), or -1 on bad args. */
int  ipc_send(int chan, const void* buf, int len);
/* Dequeue up to len bytes; returns bytes read (0 if empty), or -1 on bad args. */
int  ipc_recv(int chan, void* buf, int len);

#endif /* SECOS_IPC_H */
