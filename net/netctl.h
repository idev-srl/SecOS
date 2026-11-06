/*
 * netctl.h — SYS_NETCTL ABI (shared by the kernel and user/netcfg.c).
 * SPDX-License-Identifier: MIT
 *
 * Addresses are in network byte order with octet 0 in the low byte (the
 * convention of net_dev_t and struct secos_sockaddr).
 */
#ifndef SECOS_NETCTL_H
#define SECOS_NETCTL_H
#include <stdint.h>

#define NETCTL_COUNT       0
#define NETCTL_INFO        1
#define NETCTL_SET         2
#define NETCTL_DHCP_START  3
#define NETCTL_DHCP_POLL   4
#define NETCTL_STATS       5   /* [M47] idx ignored, arg = struct secos_netstats* */

#define NETCTL_F_UP    0x1   /* has an address */
#define NETCTL_F_LINK  0x2   /* driver reports link up */
#define NETCTL_F_DHCP  0x4   /* the address is a DHCP lease */

struct secos_ifinfo {
    char     name[16];
    uint8_t  mac[6];
    uint8_t  _pad[2];
    uint32_t ip, netmask, gateway, dns;
    uint32_t flags;          /* NETCTL_F_* */
    uint32_t irq_mode;       /* 0 none, 1 MSI-X, 2 INTx, 3 polled */
};

/* [M47] Stack occupancy + counters (leak checks in the network stress gate). */
struct secos_netstats {
    uint32_t sockets;        /* socket table slots in use */
    uint32_t tcp_conns;      /* TCP control blocks in use (incl. orphans) */
    uint32_t tcp_orphans;    /* closed by the socket layer, finishing FIN */
    uint32_t udp_binds;      /* UDP port bindings */
    uint32_t arp_entries;    /* valid ARP cache entries */
    uint32_t _pad;
    uint64_t rx_frames;      /* frames handed to the stack */
    uint64_t tx_frames;      /* frames handed to a driver */
    uint64_t tx_errors;      /* driver transmit failures */
    uint64_t rx_rekicks;     /* RX drains requested while another CPU drained */
};

#endif
