/*
 * udrv/e1000e.c — [M58] the Intel 82574L (e1000e) NIC driver in ring 3, on
 * Driver Space v2 (user/include/secos/udrv.h) and the kernel network proxy
 * (user/include/secos/udrv_net.h). Installed as /sbin/udrv-e1000e; init
 * starts it (`net0:driver:/sbin/udrv-e1000e`) when the boot command line
 * gives the NIC to ring 3 (udrv=8086:10d3).
 *
 * A port of drivers/e1000e.c (same registers, legacy descriptors):
 *   1. CLAIM 8086:10d3 and INFO. Without DMA rights (no IOMMU, or no
 *      interrupt remapping) the NIC cannot work from ring 3: say so and exit
 *      0, so init does not restart it (remove udrv= to use the in-kernel
 *      driver);
 *   2. memory decode + bus mastering, MAP_BAR 0, device reset, link up, MAC
 *      from RAL0/RAH0;
 *   3. DMA_ALLOC one page each for the RX (256) and TX (256) descriptor
 *      rings; NET_ATTACH: the kernel's packet slots are the NIC buffers
 *      (their IOVAs go into the descriptors, nothing is copied here);
 *   4. IRQ_SETUP 1 MSI-X vector with RxQ0, TxQ0 and Other routed to it; if
 *      that fails, poll mode (1 ms);
 *   5. loop: wait on the claim fd (poll(), 10 ms backstop), then service the
 *      rings and write one ack to the claim fd (unmasks the vector; with
 *      UDRV_ACK_NET_KICK when something was published for the kernel).
 *
 * usage: udrv-e1000e [--poll] [-v]
 * Output: "[udrv-e1000e] ..." lines (init's /var/log/<id>.log).
 * Exit: 0 nothing to drive, 1 setup failed, 2 the kernel asked it to stop.
 * SPDX-License-Identifier: MIT
 */
#include "libsecos.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <poll.h>
#include <unistd.h>
#include <secos/udrv.h>
#include <secos/udrv_net.h>

/* ---- registers (BAR0) ---- */
#define E_CTRL     0x00000
#define E_STATUS   0x00008
#define E_ICR      0x000C0
#define E_ITR      0x000C4
#define E_IMS      0x000D0
#define E_IMC      0x000D8
#define E_IVAR     0x000E4
#define E_RCTL     0x00100
#define E_TCTL     0x00400
#define E_TIPG     0x00410
#define E_RDBAL    0x02800
#define E_RDBAH    0x02804
#define E_RDLEN    0x02808
#define E_RDH      0x02810
#define E_RDT      0x02818
#define E_RDTR     0x02820
#define E_TDBAL    0x03800
#define E_TDBAH    0x03804
#define E_TDLEN    0x03808
#define E_TDH      0x03810
#define E_TDT      0x03818
#define E_RAL0     0x05400
#define E_RAH0     0x05404

#define CTRL_ASDE  (1u << 5)
#define CTRL_SLU   (1u << 6)
#define CTRL_RST   (1u << 26)
#define STATUS_LU  (1u << 1)
#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)
#define TXD_EOP    0x01
#define TXD_IFCS   0x02
#define TXD_RS     0x08
#define TXD_DD     0x01
#define RXD_DD     0x01
#define RXD_EOP    0x02
/* Causes: TXDW, LSC, RXO, RXT0 and the 82574 MSI-X causes RxQ0/TxQ0/Other. */
#define IMS_ALL    ((1u << 0) | (1u << 2) | (1u << 6) | (1u << 7) | (1u << 20) | (1u << 22) | (1u << 24))
/* IVAR: RxQ0, TxQ0, Other -> MSI-X entry 0 (valid bits); TX writeback IRQs. */
#define IVAR_ALL0  (0x8u | (0x8u << 8) | (0x8u << 16) | (1u << 31))

#define NRX 256u
#define NTX 256u

struct rx_desc { uint64_t addr; uint16_t length, csum; uint8_t status, errors; uint16_t special; } __attribute__((packed));
struct tx_desc { uint64_t addr; uint16_t length; uint8_t cso, cmd, status, css; uint16_t special; } __attribute__((packed));

static int g_fd = -1, g_verbose, g_irq;
static volatile uint8_t* g_bar;
static volatile struct rx_desc* g_rx;
static volatile struct tx_desc* g_tx;
static struct udrv_net_ctrl* g_c;
static uint64_t g_slots_iova, g_rx_iova, g_tx_iova;
static int32_t  g_rx_slot[NRX], g_tx_slot[NTX];
static uint32_t g_rx_clean, g_rx_tail, g_tx_clean, g_tx_tail;
static uint32_t g_tx_cons, g_rxf_cons, g_txc_prod, g_rx_prod;
static uint16_t g_spare[NRX];
static uint32_t g_nspare;
static uint32_t g_link = 0xFFFFFFFFu;
static uint64_t g_nrx, g_ntx, g_nirq, g_rxerr;

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(b) - 1) n = (int)sizeof(b) - 1;
    write(1, b, (size_t)n);
}

static long udrv(long op, void* arg, unsigned long size) { return secos_udrv(op, g_fd, arg, size); }
static long fd_read(void* b, long n) { return secos_syscall(3 /*SYS_READ*/, g_fd, (long)b, n, 0, 0); }
static long fd_write(const void* b, long n) { return secos_syscall(2 /*SYS_WRITE*/, g_fd, (long)b, n, 0, 0); }

static uint32_t rd(uint32_t off) { return *(volatile uint32_t*)(g_bar + off); }
static void wr(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_bar + off) = v; }
static void wmb(void) { __asm__ volatile("sfence" ::: "memory"); }
static void mb(void) { __asm__ volatile("mfence" ::: "memory"); }

static uint64_t slot_iova(uint32_t slot) { return g_slots_iova + (uint64_t)slot * UDRV_NET_SLOT_SIZE; }

static long dma_page(uint64_t* iova, void** uva) {
    struct udrv_dma d;
    memset(&d, 0, sizeof(d));
    d.size = 4096; d.dir = UDRV_DMA_BIDIR;
    long r = udrv(UDRV_OP_DMA_ALLOC, &d, sizeof(d));
    if (r == 0) { *iova = d.iova; *uva = (void*)(uintptr_t)d.uva; }
    return r;
}

static int hw_reset(uint8_t mac[6]) {
    wr(E_IMC, 0xFFFFFFFFu);
    (void)rd(E_ICR);
    wr(E_CTRL, rd(E_CTRL) | CTRL_RST);
    mb();
    for (int i = 0; rd(E_CTRL) & CTRL_RST; i++) {
        if (i > 1000) { say("[udrv-e1000e] reset timeout\n"); return -1; }
        usleep(1000);
    }
    wr(E_IMC, 0xFFFFFFFFu);
    wr(E_ICR, 0xFFFFFFFFu);
    wr(E_CTRL, rd(E_CTRL) | CTRL_ASDE | CTRL_SLU);
    uint32_t ral = rd(E_RAL0), rah = rd(E_RAH0);
    mac[0] = (uint8_t)ral; mac[1] = (uint8_t)(ral >> 8); mac[2] = (uint8_t)(ral >> 16);
    mac[3] = (uint8_t)(ral >> 24); mac[4] = (uint8_t)rah; mac[5] = (uint8_t)(rah >> 8);
    return 0;
}

static void rings_init(void) {
    for (uint32_t i = 0; i < NRX; i++) { g_rx_slot[i] = -1; g_rx[i].status = 0; g_rx[i].addr = 0; }
    for (uint32_t i = 0; i < NTX; i++) { g_tx_slot[i] = -1; g_tx[i].status = TXD_DD; g_tx[i].cmd = 0; }
    g_rx_clean = g_rx_tail = g_tx_clean = g_tx_tail = 0;
    wr(E_RDBAL, (uint32_t)g_rx_iova); wr(E_RDBAH, (uint32_t)(g_rx_iova >> 32));
    wr(E_RDLEN, NRX * (uint32_t)sizeof(struct rx_desc));
    wr(E_RDH, 0); wr(E_RDT, 0); wr(E_RDTR, 0);
    wr(E_TDBAL, (uint32_t)g_tx_iova); wr(E_TDBAH, (uint32_t)(g_tx_iova >> 32));
    wr(E_TDLEN, NTX * (uint32_t)sizeof(struct tx_desc));
    wr(E_TDH, 0); wr(E_TDT, 0);
    wr(E_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);          /* 2048-byte buffers */
    wr(E_TCTL, TCTL_EN | TCTL_PSP | (0x0Fu << 4) | (0x40u << 12));
    wr(E_TIPG, 10u | (8u << 10) | (6u << 20));
}

/* Give free slots to RX descriptors: slots of dropped frames first, then the
 * kernel's rxf[]. Descriptors [clean, tail) are the NIC's; one stays empty. */
static void rx_refill(void) {
    uint32_t prod = __atomic_load_n(&g_c->rxf_prod, __ATOMIC_ACQUIRE);
    int n = 0;
    while (((g_rx_tail + 1) % NRX) != g_rx_clean) {
        uint32_t slot;
        if (g_nspare) slot = g_spare[--g_nspare];
        else if (g_rxf_cons != prod) slot = g_c->rxf[g_rxf_cons++ % UDRV_NET_RX_RING].slot;
        else break;
        g_rx_slot[g_rx_tail] = (int32_t)slot;
        g_rx[g_rx_tail].addr = slot_iova(slot);
        g_rx[g_rx_tail].length = 0;
        g_rx[g_rx_tail].errors = 0;
        g_rx[g_rx_tail].status = 0;
        g_rx_tail = (g_rx_tail + 1) % NRX;
        n++;
    }
    if (n) { wmb(); wr(E_RDT, g_rx_tail); }
}

/* Received frames -> rx[]. Returns how many were published. */
static int rx_collect(void) {
    int n = 0;
    while (g_rx_clean != g_rx_tail && (g_rx[g_rx_clean].status & RXD_DD)) {
        uint32_t i = g_rx_clean;
        uint8_t st = g_rx[i].status, er = g_rx[i].errors;
        uint32_t len = g_rx[i].length;
        uint32_t slot = (uint32_t)g_rx_slot[i];
        g_rx_slot[i] = -1;
        g_rx_clean = (i + 1) % NRX;
        if (!(st & RXD_EOP) || er || len < UDRV_NET_FRAME_MIN || len > UDRV_NET_FRAME_MAX) {
            g_rxerr++;
            g_spare[g_nspare++] = (uint16_t)slot;       /* still ours: reuse it */
            continue;
        }
        struct udrv_net_desc* e = &g_c->rx[g_rx_prod % UDRV_NET_RX_RING];
        e->slot = (uint16_t)slot; e->len = (uint16_t)len; e->rsvd = 0;
        g_rx_prod++;
        g_nrx++;
        n++;
    }
    return n;
}

/* Finished TX descriptors -> txc[]. Returns how many. */
static int tx_reap(void) {
    int n = 0;
    while (g_tx_clean != g_tx_tail && (g_tx[g_tx_clean].status & TXD_DD)) {
        uint32_t i = g_tx_clean;
        struct udrv_net_desc* e = &g_c->txc[g_txc_prod % UDRV_NET_TX_RING];
        e->slot = (uint16_t)g_tx_slot[i]; e->len = 0; e->rsvd = 0;
        g_txc_prod++;
        g_tx_slot[i] = -1;
        g_tx_clean = (i + 1) % NTX;
        n++;
    }
    return n;
}

/* New frames from the kernel's tx[] -> TX descriptors. */
static void tx_post(void) {
    uint32_t prod = __atomic_load_n(&g_c->tx_prod, __ATOMIC_ACQUIRE);
    int n = 0;
    while (g_tx_cons != prod && ((g_tx_tail + 1) % NTX) != g_tx_clean) {
        struct udrv_net_desc e = g_c->tx[g_tx_cons % UDRV_NET_TX_RING];
        g_tx_cons++;
        uint32_t i = g_tx_tail;
        g_tx_slot[i] = e.slot;
        g_tx[i].addr = slot_iova(e.slot);
        g_tx[i].length = e.len;
        g_tx[i].cso = 0;
        g_tx[i].cmd = TXD_EOP | TXD_IFCS | TXD_RS;
        g_tx[i].status = 0;
        g_tx_tail = (i + 1) % NTX;
        g_ntx++;
        n++;
    }
    if (n) { wmb(); wr(E_TDT, g_tx_tail); }
}

/* One pass over every ring. Returns 1 if the kernel has something to read. */
static int service(void) {
    int pub = tx_reap();
    tx_post();
    pub += rx_collect();
    uint32_t link = (rd(E_STATUS) & STATUS_LU) ? 1u : 0u;
    if (link != g_link) {
        g_link = link;
        g_c->link = link;
        say("[udrv-e1000e] link %s\n", link ? "up" : "down");
        pub++;
    }
    wmb();
    g_c->txc_prod = g_txc_prod;
    g_c->rx_prod = g_rx_prod;
    rx_refill();
    return pub != 0;
}

static int claim_dev(void) {
    struct udrv_claim c;
    memset(&c, 0, sizeof(c));
    c.vendor = 0x8086; c.device = 0x10D3;
    c.flags = UDRV_CLAIM_BY_ID;
    c.dma_mask = ~0ull;                         /* 64-bit descriptors */
    return (int)secos_udrv(UDRV_OP_CLAIM, 0, &c, sizeof(c));
}

int main(int argc, char** argv) {
    int force_poll = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--poll")) force_poll = 1;
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else { say("usage: udrv-e1000e [--poll] [-v]\n"); return 1; }
    }
    g_fd = claim_dev();
    if (g_fd == -19) { say("[udrv-e1000e] no 82574L given to ring 3 (udrv=): nothing to do\n"); return 0; }
    if (g_fd < 0) { say("[udrv-e1000e] claim failed: %d\n", g_fd); return 1; }
    struct udrv_info in;
    memset(&in, 0, sizeof(in));
    if (udrv(UDRV_OP_INFO, &in, sizeof(in)) != 0) { say("[udrv-e1000e] INFO failed\n"); return 1; }
    say("[udrv-e1000e] claimed %02x:%02x.%x %04x:%04x msix=%u iommu=%u ir=%u dma_ok=%u\n",
        in.bus, in.dev, in.fn, in.vendor, in.device, in.msix_count, in.iommu, in.ir, in.dma_ok);
    if (!in.dma_ok) {
        say("[udrv-e1000e] no DMA rights (needs iommu=on with interrupt remapping): cannot drive the NIC; "
            "boot without udrv= to use the in-kernel driver\n");
        udrv(UDRV_OP_RELEASE, 0, 0);
        return 0;
    }
    struct udrv_cfg cf = { 0x04, 2, 0x0006, 0 };   /* memory decode + bus mastering */
    if (udrv(UDRV_OP_CFG_WRITE, &cf, sizeof(cf)) != 0) { say("[udrv-e1000e] bus mastering refused\n"); return 1; }
    struct udrv_map_bar mb0;
    memset(&mb0, 0, sizeof(mb0));
    if (udrv(UDRV_OP_MAP_BAR, &mb0, sizeof(mb0)) != 0 || mb0.len < 0x20000) {
        say("[udrv-e1000e] MAP_BAR 0 failed\n"); return 1;
    }
    g_bar = (volatile uint8_t*)(uintptr_t)mb0.uva;
    uint8_t mac[6];
    if (hw_reset(mac) != 0) return 1;
    void* rxu; void* txu;
    if (dma_page(&g_rx_iova, &rxu) != 0 || dma_page(&g_tx_iova, &txu) != 0) {
        say("[udrv-e1000e] DMA_ALLOC failed\n"); return 1;
    }
    g_rx = (volatile struct rx_desc*)rxu;
    g_tx = (volatile struct tx_desc*)txu;
    struct udrv_net_attach na;
    memset(&na, 0, sizeof(na));
    memcpy(na.mac, mac, 6);
    long r = udrv(UDRV_OP_NET_ATTACH, &na, sizeof(na));
    if (r != 0) { say("[udrv-e1000e] NET_ATTACH failed: %ld\n", r); return 1; }
    g_c = (struct udrv_net_ctrl*)(uintptr_t)na.ctrl_uva;
    g_slots_iova = na.slots_iova;
    if (g_c->magic != UDRV_NET_MAGIC || g_c->version != UDRV_NET_VERSION) {
        say("[udrv-e1000e] unexpected proxy ABI\n"); return 1;
    }
    rings_init();
    rx_refill();
    if (!force_poll) {
        struct udrv_irq iq = { 1, 0 };
        if (udrv(UDRV_OP_IRQ_SETUP, &iq, sizeof(iq)) == 0 && iq.mode == UDRV_IRQ_MSIX) {
            wr(E_IVAR, IVAR_ALL0);
            wr(E_ITR, 0);
            g_irq = 1;
        } else {
            say("[udrv-e1000e] no MSI-X vector: poll mode\n");
        }
    }
    if (g_irq) wr(E_IMS, IMS_ALL);
    say("[udrv-e1000e] %s up mac=%02x:%02x:%02x:%02x:%02x:%02x mode=%s rx=%u tx=%u\n", na.ifname,
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], g_irq ? "msix" : "poll", NRX, NTX);

    uint32_t pending = g_irq ? 1u : 0u;            /* the first ack arms the vector */
    for (;;) {
        int kick = service();
        struct udrv_ack a = { pending, kick ? UDRV_ACK_NET_KICK : 0u };
        if (a.vec_mask || a.flags) {
            long w = fd_write(&a, sizeof(a));
            if (w < 0) { say("[udrv-e1000e] claim fd write: %ld; exiting\n", w); return 2; }
        }
        pending = 0;
        if (kick) continue;                        /* the kernel posted free slots */
        struct pollfd pf = { g_fd, POLLIN, 0 };
        /* Frames on the wire: look again within 1 ms even if the TX-done
         * interrupt is late, so the kernel's TX slots come back quickly. */
        int busy = g_tx_clean != g_tx_tail;
        int pr = poll(&pf, 1, (g_irq && !busy) ? 10 : 1);
        if (pr > 0 && (pf.revents & (POLLHUP | POLLERR))) { say("[udrv-e1000e] claim ended; exiting\n"); return 2; }
        if (pr > 0 && (pf.revents & POLLIN)) {
            struct udrv_events ev;
            long n = fd_read(&ev, sizeof(ev));
            if (n == (long)sizeof(ev)) {
                if (ev.flags & UDRV_EV_STOP) { say("[udrv-e1000e] stop requested by the kernel\n"); return 2; }
                if (ev.vec_pending_mask & 1u) {
                    pending = 1;
                    g_nirq++;
                    wr(E_ICR, 0xFFFFFFFFu);        /* write-1-to-clear the causes */
                }
            } else if (n < 0 && n != -11) {
                say("[udrv-e1000e] claim fd read: %ld; exiting\n", n);
                return 2;
            }
        }
        if (g_verbose && (g_nrx + g_ntx) % 1000 == 0 && g_nrx + g_ntx)
            say("[udrv-e1000e] rx=%lu tx=%lu irq=%lu rxerr=%lu\n", (unsigned long)g_nrx,
                (unsigned long)g_ntx, (unsigned long)g_nirq, (unsigned long)g_rxerr);
    }
}
