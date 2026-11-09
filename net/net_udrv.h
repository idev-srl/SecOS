/*
 * net_udrv.h — [M58] the kernel proxy of a ring-3 NIC driver.
 * SPDX-License-Identifier: MIT
 *
 * One proxy per network-class PCI function given to ring 3 (udrv=). It is a
 * net_dev_t registered at boot by net_init() (so ethN exists, link down,
 * before any driver runs) whose transmit()/poll() talk to the region shared
 * with the driver (user/include/secos/udrv_net.h). kernel/udrv.c attaches it
 * on UDRV_OP_NET_ATTACH and detaches it in the claim teardown; between two
 * drivers the interface stays registered with its IP configuration.
 *
 * Locking: px->lk (LK_S22_UDRV_NET) guards the ring state and the region
 * pointers. transmit() takes it inside tx_lock, poll() inside rx_lock; frames
 * go to net_rx() only after it is dropped. attach/detach take it under
 * g_udrv_lock (K45). Inside it: the udrv event lock (S25) for the TX kick and
 * a kill.
 */
#ifndef NET_UDRV_H
#define NET_UDRV_H
#include <stdint.h>
#include "pci.h"

typedef struct net_udrv net_udrv_t;

/* net_init(): register one proxy per assigned network-class function. */
int  net_udrv_init(void);
/* The proxy of pd, or NULL. */
net_udrv_t* net_udrv_find(const pci_dev_t* pd);
/* Start using a region: ctrl/slots are kernel (physmap) addresses of zeroed
 * frames. cookie/gen identify the claim (kernel/udrv.c). Fills ifname.
 * 0, or -16 (EBUSY: attached). */
int  net_udrv_attach(net_udrv_t* px, void* ctrl, void* slots, const uint8_t mac[6],
                     void* cookie, uint32_t gen, uint32_t pid, char ifname[16]);
/* Stop using it (no access to the region after this returns). why: "exit",
 * "reap", "release", ... */
void net_udrv_detach(net_udrv_t* px, const char* why);
/* The driver kicked: drain rx[] / txc[] now (any context, no lock held). */
void net_udrv_kick(net_udrv_t* px);
/* Proxies without an attached driver (init's boot settle wait). */
int  net_udrv_waiting(void);
/* /proc/udrv: one "net ..." line per proxy. */
void net_udrv_generate(void (*emit)(void* ctx, const char* line), void* ctx);

/* The per-field checks (pure; see net_udrv.c). */
enum { OWN_KERNEL = 0, OWN_TX = 1, OWN_RX = 2 };          /* slot owner */
enum { UNV_OK = 0, UNV_BAD_INDEX, UNV_SLOT_RANGE, UNV_NOT_OWNED, UNV_BAD_LEN };
int  udrv_net_check_index(uint32_t prod, uint32_t k_cons, uint32_t ring);
int  udrv_net_check_rx(uint32_t slot, uint32_t len, const uint8_t* own);
int  udrv_net_check_txc(uint32_t slot, const uint8_t* own);
/* Count one rejected entry; 1 when this one must request the kill. */
int  udrv_net_reject_count(uint32_t* count, int* kill_sent);
const char* udrv_net_verdict_name(int v);

/* Provided by kernel/udrv.c (called with px->lk held). */
void udrv_net_notify(void* cookie, uint32_t gen);              /* UDRV_EV_TX_KICK */
void udrv_net_kill(void* cookie, uint32_t gen, const char* why);
int  udrv_watchdog_kill(void);                                  /* udrv.watchdog=kill */
const pci_dev_t* udrv_assigned(int i);                          /* NULL past the end */

#endif /* NET_UDRV_H */
