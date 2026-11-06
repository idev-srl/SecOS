/*
 * udp.h — [M24] UDP transport + DHCP/DNS clients.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * UDP is a thin demux over IPv4: a small table of port bindings, each with a
 * delivery callback. DHCP (config) and DNS (name resolution) are clients that
 * bind an ephemeral/well-known port, send, and capture the reply from the
 * callback. The socket layer (M24 wave 3) binds a port whose callback enqueues
 * datagrams into the socket's receive ring.
 */
#ifndef NET_UDP_H
#define NET_UDP_H
#include <stdint.h>
#include "net.h"

/* Delivery callback: invoked from the RX path (stack lock held) when a datagram
 * arrives for a bound local port. 'data'/'len' is the UDP payload (header
 * stripped). It must not block or take a net lock.
 * [M47] udp_bind/unbind/ephemeral_port/udp_output expect the stack lock held. */
typedef void (*udp_recv_cb)(void* ctx, net_dev_t* dev, uint32_t src_ip,
                            uint16_t src_port, const uint8_t* data, uint32_t len);

int      udp_bind(uint16_t port, udp_recv_cb cb, void* ctx);  /* 0 ok, -1 full, -2 in use */
void     udp_unbind(uint16_t port);
uint16_t udp_ephemeral_port(void);                            /* an unused local port */

/* Send a datagram. src_port/dst_port are host order; dst_ip network order.
 * dst_ip 0xFFFFFFFF is a link broadcast (no ARP). 0 ok, <0 error.
 * udp_output: stack lock held. udp_send: takes it (callers outside net/). */
int      udp_output(net_dev_t* dev, uint32_t dst_ip, uint16_t src_port,
                    uint16_t dst_port, const void* data, uint32_t len);
int      udp_send(net_dev_t* dev, uint32_t dst_ip, uint16_t src_port,
                  uint16_t dst_port, const void* data, uint32_t len);
int      udp_bind_count(void);

/* ---- DHCP client (net/dhcp.c) ---- */
/* [M47] The DHCP/DNS entry points below take the stack lock themselves. */
/* Run a full DISCOVER/OFFER/REQUEST/ACK exchange on dev. On success sets
 * dev->ip/netmask/gateway/dns from the lease. Returns 0 ok, -1 on timeout.
 * Spins on hlt: kernel shell/idle context only (RX needs the CPU to hlt — M24). */
int      dhcp_configure(net_dev_t* dev);
/* Asynchronous form (used by SYS_NETCTL): dhcp_start() sends DISCOVER and
 * returns (0, -EBUSY while another exchange runs, -ENODEV); dhcp_poll() advances
 * the exchange and returns its phase. A failed exchange restores the previous
 * address; a lease sets dev->ip/netmask/gateway/dns. */
enum { DHCP_PH_IDLE = 0, DHCP_PH_DISCOVER, DHCP_PH_REQUEST, DHCP_PH_BOUND, DHCP_PH_FAILED };
int      dhcp_start(net_dev_t* dev);
int      dhcp_poll(net_dev_t* dev);
int      dhcp_poll_locked(net_dev_t* dev);       /* same, stack lock already held */
int      dhcp_has_lease(const net_dev_t* dev);   /* 1: dev's address is a DHCP lease */

/* ---- DNS client (net/dns.c) ---- */
/* Resolve a hostname (A record) via dev->dns into *out_ip (network order).
 * Returns 0 ok, -1 on failure/timeout. Shell/idle context (see above). */
int      dns_resolve(net_dev_t* dev, const char* name, uint32_t* out_ip);

#endif /* NET_UDP_H */
