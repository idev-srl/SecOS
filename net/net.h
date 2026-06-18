/*
 * net.h — [M24] SecOS network core contract.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * The stable interface every NIC driver and protocol layer builds against —
 * analogous to block_dev_t for storage. A NIC driver fills a net_dev_t (MAC +
 * transmit + poll), registers it, delivers received frames via net_rx(), and
 * wires its interrupt with net_request_irq() (MSI-X preferred for 2.5GbE+, INTx
 * fallback, timer-tick polling as a last resort). The stack (eth/arp/ip/...)
 * consumes frames from net_rx and sends through dev->transmit.
 */
#ifndef NET_H
#define NET_H
#include <stdint.h>
#include "pci.h"

#define NET_MTU       1500
#define NET_FRAME_MAX 1536        /* MTU + Ethernet header + FCS slack */
#define ETH_ALEN      6

/* IRQ delivery mode chosen by net_request_irq(). */
typedef enum { NET_IRQ_NONE = 0, NET_IRQ_MSIX, NET_IRQ_INTX, NET_IRQ_POLL } net_irq_mode_t;

typedef struct net_dev {
    char     name[16];            /* "eth0", ... */
    uint8_t  mac[ETH_ALEN];       /* filled by the driver from the NIC */
    /* L3 config (static or DHCP-assigned), network byte order. 0 = unset. */
    uint32_t ip, netmask, gateway, dns;
    /* Send one fully-formed Ethernet frame (dst+src+type+payload). 0 ok, <0 err. */
    int      (*transmit)(struct net_dev* dev, const void* frame, uint32_t len);
    /* Drain the RX ring, calling net_rx() for each frame. Invoked from the NIC
     * IRQ (NAPI-style: ack, then drain to empty) and/or the timer-tick poll. */
    void     (*poll)(struct net_dev* dev);
    void*    priv;                /* driver state */
    pci_device_t pci;            /* the NIC's PCI function (for IRQ setup) */
    net_irq_mode_t irq_mode;
    int      link_up;
} net_dev_t;

/* ---- Driver-facing core API ---- */
int        net_register_dev(net_dev_t* dev);       /* 0 ok, -1 full/dup */
int        net_dev_count(void);
net_dev_t* net_get_dev(int i);
net_dev_t* net_primary(void);                      /* first registered NIC */

/* Driver -> stack: hand a received Ethernet frame to the stack (copies out). */
void net_rx(net_dev_t* dev, const void* frame, uint32_t len);

/* Wire the device's interrupt to dev->poll: try MSI-X, then legacy INTx, then
 * register dev->poll for timer-tick polling. Returns the chosen mode (also
 * stored in dev->irq_mode). The driver must have filled dev->pci + dev->poll. */
net_irq_mode_t net_request_irq(net_dev_t* dev);

/* Bring up the whole network subsystem: probe every supported NIC, register the
 * stack, configure (static IP for now), and start. Returns the number of NICs. */
int net_init(void);

/* Called from the timer tick (drives polled NICs + protocol timers like ARP/TCP
 * retransmit). Cheap no-op when networking is absent. */
void net_tick(void);

/* ---- Protocol layer entry points (implemented across net/*.c) ---- */
void eth_input(net_dev_t* dev, const uint8_t* frame, uint32_t len);   /* L2 demux */
/* Send a payload as an Ethernet frame to dst_mac with the given ethertype. */
int  eth_send(net_dev_t* dev, const uint8_t dst_mac[6], uint16_t ethertype,
              const void* payload, uint32_t len);

/* ARP */
void arp_input(net_dev_t* dev, const uint8_t* frame, uint32_t len);
int  arp_resolve(net_dev_t* dev, uint32_t ip, uint8_t out_mac[6]);    /* 0 ok, -1 pending */
void arp_request(net_dev_t* dev, uint32_t target_ip);

/* IPv4 / ICMP */
void ipv4_input(net_dev_t* dev, const uint8_t* frame, uint32_t len, const uint8_t src_mac[6]);
int  ipv4_send(net_dev_t* dev, uint32_t dst_ip, uint8_t proto, const void* payload, uint32_t len);
void icmp_input(net_dev_t* dev, uint32_t src_ip, const uint8_t* pkt, uint32_t len);

/* ---- byte order + checksum helpers ---- */
static inline uint16_t htons(uint16_t x){ return (uint16_t)((x<<8)|(x>>8)); }
static inline uint16_t ntohs(uint16_t x){ return htons(x); }
static inline uint32_t htonl(uint32_t x){ return ((x&0xFF)<<24)|((x&0xFF00)<<8)|((x>>8)&0xFF00)|((x>>24)&0xFF); }
static inline uint32_t ntohl(uint32_t x){ return htonl(x); }
uint16_t net_checksum(const void* data, uint32_t len);          /* one's-complement */

#endif /* NET_H */
