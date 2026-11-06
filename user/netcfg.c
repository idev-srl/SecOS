/* netcfg - SecOS interface configuration over SYS_NETCTL (95).
 * One signed multi-call program (CAP_NET manifest), dispatching on argv[0]:
 *
 *   ifconfig [IFACE]                         show the interfaces (also: netcfg)
 *   ifconfig IFACE ADDR [netmask M] [gw G] [dns D]   static address
 *   ifup   [-t SECONDS] [IFACE]              DHCP lease (default 10 s, first NIC)
 *   ifdown [IFACE]                           drop the address
 *
 * The DHCP client is the kernel's (net/dhcp.c); ifup starts it and polls it
 * between short sleeps, so it never blocks the kernel. ifup gives up after the
 * timeout and exits 1 (the previous address, if any, is kept), so a boot with
 * no NIC or no DHCP server goes on.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "libsecos.h"
#include "../net/netctl.h"

#define SYS_NETCTL 95

static long netctl(long op, long idx, void* arg) {
    return secos_syscall(SYS_NETCTL, op, idx, (long)arg, 0, 0);
}

static const char* base(const char* p) {
    const char* b = p;
    for (; *p; p++) if (*p == '/') b = p + 1;
    return b;
}

static void ipstr(uint32_t ip, char* out) {
    sprintf(out, "%u.%u.%u.%u", ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, ip >> 24);
}

static int parse_ip(const char* s, uint32_t* out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return -1;
        unsigned o = 0;
        while (*s >= '0' && *s <= '9') { o = o * 10 + (unsigned)(*s - '0'); s++; if (o > 255) return -1; }
        v |= (uint32_t)o << (8 * i);
        if (i < 3) { if (*s != '.') return -1; s++; }
    }
    if (*s) return -1;
    *out = v;
    return 0;
}

static int prefix_len(uint32_t mask) {
    int n = 0;
    for (int i = 0; i < 32; i++) if (mask & (1u << i)) n++;
    return n;
}

static int count_ifaces(const char* me) {
    long n = netctl(NETCTL_COUNT, 0, 0);
    if (n < 0) {
        dprintf(2, "%s: netctl failed (%ld)%s\n", me, n, n == -1 ? " - not permitted (CAP_NET)" : "");
        return -1;
    }
    return (int)n;
}

/* Interface index by name ("eth0") or number; -1 if absent. */
static int find_iface(const char* name, int n) {
    for (int i = 0; i < n; i++) {
        struct secos_ifinfo k;
        if (netctl(NETCTL_INFO, i, &k) == 0 && strcmp(k.name, name) == 0) return i;
    }
    if (name[0] >= '0' && name[0] <= '9' && atoi(name) < n) return atoi(name);
    return -1;
}

static void show(int idx) {
    struct secos_ifinfo k;
    if (netctl(NETCTL_INFO, idx, &k) != 0) return;
    char a[16], m[16], g[16], d[16];
    ipstr(k.ip, a); ipstr(k.netmask, m); ipstr(k.gateway, g); ipstr(k.dns, d);
    static const char* const irq[] = { "none", "MSI-X", "INTx", "polled" };
    printf("%-9s Link encap:Ethernet  HWaddr %02x:%02x:%02x:%02x:%02x:%02x\n", k.name,
           k.mac[0], k.mac[1], k.mac[2], k.mac[3], k.mac[4], k.mac[5]);
    if (k.ip)
        printf("          inet %s/%d  Mask:%s  Gateway:%s  DNS:%s\n", a, prefix_len(k.netmask), m, g, d);
    else
        printf("          no address\n");
    printf("          %s%s %s  rx:%s\n\n", k.ip ? "UP" : "DOWN",
           (k.flags & NETCTL_F_LINK) ? " RUNNING" : "",
           (k.flags & NETCTL_F_DHCP) ? "dhcp" : (k.ip ? "static" : "unconfigured"),
           k.irq_mode < 4 ? irq[k.irq_mode] : "?");
}

static int do_ifconfig(const char* me, int argc, char** argv) {
    int n = count_ifaces(me);
    if (n < 0) return 1;
    if (argc < 2) {
        if (n == 0) printf("no network interface\n");
        for (int i = 0; i < n; i++) show(i);
        return 0;
    }
    int idx = find_iface(argv[1], n);
    if (idx < 0) { dprintf(2, "%s: %s: no such interface\n", me, argv[1]); return 1; }
    if (argc == 2) { show(idx); return 0; }
    struct secos_ifinfo k;
    if (netctl(NETCTL_INFO, idx, &k) != 0) return 1;
    if (strcmp(argv[2], "down") == 0) {
        k.ip = k.netmask = k.gateway = k.dns = 0;
    } else {
        if (parse_ip(argv[2], &k.ip) != 0) { dprintf(2, "%s: bad address '%s'\n", me, argv[2]); return 1; }
        k.netmask = 0x00ffffffu;                         /* /24 unless given */
        for (int i = 3; i + 1 < argc; i += 2) {
            uint32_t* f = strcmp(argv[i], "netmask") == 0 ? &k.netmask :
                          strcmp(argv[i], "gw") == 0 ? &k.gateway :
                          strcmp(argv[i], "dns") == 0 ? &k.dns : 0;
            if (!f || parse_ip(argv[i + 1], f) != 0) {
                dprintf(2, "%s: bad option '%s %s'\n", me, argv[i], argv[i + 1]);
                return 1;
            }
        }
    }
    long r = netctl(NETCTL_SET, idx, &k);
    if (r != 0) { dprintf(2, "%s: cannot configure %s (%ld)\n", me, k.name, r); return 1; }
    return 0;
}

static int do_ifup(const char* me, int argc, char** argv) {
    int timeout = 10;
    const char* name = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) timeout = atoi(argv[++i]);
        else if (argv[i][0] == '-') { dprintf(2, "usage: ifup [-t SECONDS] [IFACE]\n"); return 2; }
        else name = argv[i];
    }
    if (timeout <= 0) timeout = 1;
    int n = count_ifaces(me);
    if (n < 0) return 1;
    if (n == 0) { printf("ifup: no network interface; skipping\n"); return 1; }
    int idx = name ? find_iface(name, n) : 0;
    if (idx < 0) { dprintf(2, "%s: %s: no such interface\n", me, name); return 1; }
    struct secos_ifinfo k;
    netctl(NETCTL_INFO, idx, &k);
    long r = netctl(NETCTL_DHCP_START, idx, 0);
    if (r == -16) {                                     /* another exchange runs: wait for it */
        printf("ifup: %s: DHCP already in progress, waiting\n", k.name);
        r = 0;
    }
    if (r < 0) { dprintf(2, "%s: %s: cannot start DHCP (%ld)\n", me, k.name, r); return 1; }
    printf("ifup: %s: requesting a DHCP lease (timeout %d s)\n", k.name, timeout);
    unsigned long deadline = getticks() + (unsigned long)timeout * 1000;
    for (;;) {
        r = netctl(NETCTL_DHCP_POLL, idx, 0);
        if (r != 0 || getticks() >= deadline) break;
        usleep(50000);
    }
    netctl(NETCTL_INFO, idx, &k);
    char a[16], g[16], d[16];
    ipstr(k.ip, a); ipstr(k.gateway, g); ipstr(k.dns, d);
    if (r == 1) {
        printf("ifup: %s: lease %s/%d gw %s dns %s\n", k.name, a, prefix_len(k.netmask), g, d);
        return 0;
    }
    if (k.ip) printf("ifup: %s: no DHCP lease; keeping %s\n", k.name, a);
    else printf("ifup: %s: no DHCP lease; interface left unconfigured\n", k.name);
    return 1;
}

static int do_ifdown(const char* me, int argc, char** argv) {
    int n = count_ifaces(me);
    if (n <= 0) return 1;
    int idx = argc > 1 ? find_iface(argv[1], n) : 0;
    if (idx < 0) { dprintf(2, "%s: %s: no such interface\n", me, argv[1]); return 1; }
    struct secos_ifinfo k;
    if (netctl(NETCTL_INFO, idx, &k) != 0) return 1;
    k.ip = k.netmask = k.gateway = k.dns = 0;
    return netctl(NETCTL_SET, idx, &k) == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    const char* me = (argc > 0 && argv[0]) ? base(argv[0]) : "netcfg";
    if (strcmp(me, "ifup") == 0) return do_ifup(me, argc, argv);
    if (strcmp(me, "ifdown") == 0) return do_ifdown(me, argc, argv);
    return do_ifconfig(me, argc, argv);
}
