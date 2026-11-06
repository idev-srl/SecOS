/*
 * net_lock.h — network stack locking (M47).
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 *
 * Lock hierarchy (always acquire in increasing level, never the reverse):
 *
 *   level 0  tick gate      net_tick(): one CPU runs the protocol timers
 *   level 1  dev->rx_lock   per NIC: the driver's RX ring + dev->poll()
 *   level 2  net stack lock one lock: sockets, TCP control blocks + timers,
 *                           UDP bindings, ARP cache, DHCP state, ICMP state,
 *                           interface L3 config, all protocol TX scratch
 *   level 3  dev->tx_lock   per NIC: the driver's TX ring + dev->transmit()
 *
 * Every net lock is IRQ-safe: it is always taken with local interrupts off
 * (spin_lock_irqsave), because the RX path runs from the MSI-X ISR and from
 * the timer tick. Nothing that can sleep, fault on a user address or take a
 * scheduler lock runs while one is held: user copies happen in the syscall
 * layer on kernel bounce buffers, and blocking is the syscall layer's job
 * (-EAGAIN + sleep + re-run, see kernel/syscall_trap.c).
 *
 * Calling convention: the protocol internals (eth/arp/ipv4/icmp/udp_output/tcp
 * core) run with the stack lock held. The entry points take it themselves:
 * net_rx (driver RX), net_tick, the socket layer, netctl, DHCP/DNS clients,
 * the shell helpers (net_ping_*, tcp_connect/..., udp_send).
 *
 * -DSECOS_LOCKDEP_LITE records the levels held per CPU and panics on an
 * out-of-order or recursive acquisition, on an out-of-order release, and on a
 * net lock taken with interrupts enabled; NET_ASSERT_STACK_LOCKED() checks
 * that internals are entered with the stack lock held.
 */
#ifndef NET_LOCK_H
#define NET_LOCK_H
#include <stdint.h>
#include "../arch/x86/spinlock.h"

enum { NETLK_TICK = 0, NETLK_RX = 1, NETLK_STACK = 2, NETLK_TX = 3 };

typedef struct net_spinlock {
    spinlock_t  l;
    uint8_t     level;      /* NETLK_* */
    const char* name;
} net_spinlock_t;

void     netlk_init(net_spinlock_t* lk, uint8_t level, const char* name);
uint64_t netlk_acquire(net_spinlock_t* lk);                 /* irqsave */
int      netlk_try(net_spinlock_t* lk, uint64_t* flags);    /* 1 = taken */
void     netlk_release(net_spinlock_t* lk, uint64_t flags);

/* The stack lock (level 2). */
uint64_t net_lock(void);
void     net_unlock(uint64_t flags);

#ifdef SECOS_LOCKDEP_LITE
int  net_stack_lock_held(void);     /* by this CPU */
#define NET_ASSERT_STACK_LOCKED() \
    do { if (!net_stack_lock_held()) netlk_bug("stack lock not held", __func__); } while (0)
void netlk_bug(const char* what, const char* where) __attribute__((noreturn));
#else
#define NET_ASSERT_STACK_LOCKED() do { } while (0)
#endif

#endif /* NET_LOCK_H */
