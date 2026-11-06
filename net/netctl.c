/*
 * netctl.c — SYS_NETCTL (95): interface configuration from ring 3.
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 *
 * Lets a signed CAP_NET program (init's /bin/ifup) list the NICs, read their
 * address configuration, set a static one, and run the kernel DHCP client
 * (net/dhcp.c) without the kernel shell. Every call returns at once: DHCP is
 * started by NETCTL_DHCP_START and advanced by NETCTL_DHCP_POLL, which the
 * caller issues between sleeps, so no ring-0 spinning on the network.
 *
 *   netctl(NETCTL_COUNT, 0, 0)               -> number of NICs
 *   netctl(NETCTL_INFO,  idx, ifinfo*)       -> 0, fills the struct
 *   netctl(NETCTL_SET,   idx, const ifinfo*) -> 0, sets ip/netmask/gateway/dns
 *   netctl(NETCTL_DHCP_START, idx, 0)        -> 0 / -EBUSY / -ENODEV
 *   netctl(NETCTL_DHCP_POLL,  idx, 0)        -> 0 running, 1 bound, -ETIMEDOUT
 *
 * Errors are -errno. The caller needs CAP_NET in its signed manifest (EPERM
 * otherwise, audited like the socket calls).
 */
#include <stdint.h>
#include <stddef.h>
#include "net.h"
#include "udp.h"
#include "netctl.h"
#include "debugcon.h"
#include "../kernel/process.h"
#include "../kernel/sched.h"
#include "../mm/user_copy.h"

static net_dev_t* nc_dev(uint64_t idx) {
    if (idx >= (uint64_t)net_dev_count()) return NULL;
    return net_get_dev((int)idx);
}

long ksys_netctl(uint64_t op, uint64_t idx, uint64_t arg) {
    process_t* c = sched_get_current();
    if (!c || !c->cap_net) {
        debugcon_writestring("[NET] netctl denied: no CAP_NET\n");
        return -1;                                              /* -EPERM */
    }
    if (op == NETCTL_COUNT) return net_dev_count();
    net_dev_t* d = nc_dev(idx);
    if (!d) return -19;                                         /* -ENODEV */
    switch (op) {
    case NETCTL_INFO: {
        struct secos_ifinfo k;
        for (size_t i = 0; i < sizeof(k); i++) ((uint8_t*)&k)[i] = 0;
        for (int i = 0; i < 15 && d->name[i]; i++) k.name[i] = d->name[i];
        for (int i = 0; i < 6; i++) k.mac[i] = d->mac[i];
        k.ip = d->ip; k.netmask = d->netmask; k.gateway = d->gateway; k.dns = d->dns;
        k.flags = (d->link_up ? NETCTL_F_LINK : 0) | (dhcp_has_lease(d) ? NETCTL_F_DHCP : 0)
                | (d->ip ? NETCTL_F_UP : 0);
        k.irq_mode = (uint32_t)d->irq_mode;
        if (copy_to_user((void*)arg, &k, sizeof(k)) != 0) return -14;   /* -EFAULT */
        return 0;
    }
    case NETCTL_SET: {
        struct secos_ifinfo k;
        if (copy_from_user(&k, (const void*)arg, sizeof(k)) != 0) return -14;
        int ph = dhcp_poll(d);
        if (ph == DHCP_PH_DISCOVER || ph == DHCP_PH_REQUEST) return -16;  /* -EBUSY */
        d->ip = k.ip; d->netmask = k.netmask; d->gateway = k.gateway; d->dns = k.dns;
        debugcon_writestring("[NET] netctl: static config ip="); debugcon_print_hex(d->ip);
        debugcon_writestring("\n");
        return 0;
    }
    case NETCTL_DHCP_START:
        return dhcp_start(d);
    case NETCTL_DHCP_POLL: {
        int ph = dhcp_poll(d);
        if (ph == DHCP_PH_BOUND) return 1;
        if (ph == DHCP_PH_DISCOVER || ph == DHCP_PH_REQUEST) return 0;
        return -110;                                            /* -ETIMEDOUT */
    }
    default:
        return -22;                                             /* -EINVAL */
    }
}
