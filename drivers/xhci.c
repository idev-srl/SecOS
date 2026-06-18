/*
 * xhci.c — [M22] xHCI (USB 3) host controller driver.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Polled, single-operation-at-a-time xHCI. Register windows (capability,
 * operational, runtime, doorbell) are reached through the physmap. One command
 * ring + one event ring (single ERST segment) drive the controller; each device
 * gets an EP0 ring plus up to XHCI_EPR_PER_DEV data-endpoint rings. All DMA
 * structures are static, suitably aligned kernel buffers; physical addresses
 * come from kvirt_to_phys. Completions are found by draining the event ring.
 */
#include "xhci.h"
#include "usb.h"
#include "pci.h"
#include "io.h"
#include "debugcon.h"
#include "vmm.h"
#include "idt.h"
#include "lapic.h"
#include <stddef.h>

// ---- Capability registers ----
#define CAP_CAPLENGTH   0x00
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

// ---- Operational registers (relative to op base) ----
#define OP_USBCMD   0x00
#define OP_USBSTS   0x04
#define OP_PAGESIZE 0x08
#define OP_CRCR     0x18   // 64-bit
#define OP_DCBAAP   0x30   // 64-bit
#define OP_CONFIG   0x38
#define OP_PORTS    0x400  // PORTSC[n] at 0x400 + (n-1)*0x10

#define USBCMD_RS    (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBSTS_HCH   (1u << 0)
#define USBSTS_CNR   (1u << 11)

#define PORTSC_CCS  (1u << 0)
#define PORTSC_PED  (1u << 1)
#define PORTSC_PR   (1u << 4)
#define PORTSC_PP   (1u << 9)
#define PORTSC_CSC  (1u << 17)
#define PORTSC_PRC  (1u << 21)
#define PORTSC_RW1C 0x00FE0000u   // change bits 17..23

// ---- Runtime: interrupter 0 (relative to runtime base + 0x20) ----
#define IR0_IMAN    0x20
#define IR0_ERSTSZ  0x28
#define IR0_ERSTBA  0x30   // 64-bit
#define IR0_ERDP    0x38   // 64-bit
#define IMAN_IP     (1u << 0)   // interrupt pending (RW1C)
#define IMAN_IE     (1u << 1)   // interrupt enable

// ---- TRB types ----
#define TRB_NORMAL        1
#define TRB_SETUP         2
#define TRB_DATA          3
#define TRB_STATUS        4
#define TRB_LINK          6
#define TRB_ENABLE_SLOT   9
#define TRB_ADDRESS_DEV   11
#define TRB_CONFIG_EP     12
#define TRB_EVAL_CTX      13
#define TRB_RESET_EP      14
#define TRB_SET_TR_DEQ    16
#define TRB_XFER_EVENT    32
#define TRB_CMD_COMPLETE  33
#define TRB_PORT_CHANGE   34

#define CC_SUCCESS        1
#define CC_SHORT_PACKET   13

#define TRB_TYPE(t)   ((uint32_t)(t) << 10)
#define TRB_CYCLE     (1u << 0)
#define TRB_IOC       (1u << 5)
#define TRB_IDT       (1u << 6)
#define TRB_DIR_IN    (1u << 16)
#define TRB_TC        (1u << 1)   // link: toggle cycle

#define MAX_DEV 4

// ---- Static DMA structures ----
static xhci_trb_t g_cmd[XHCI_RING_SIZE] __attribute__((aligned(1024)));
static xhci_trb_t g_evt[XHCI_RING_SIZE] __attribute__((aligned(1024)));
static uint8_t    g_erst[64]            __attribute__((aligned(64)));
static uint64_t   g_dcbaa[256]          __attribute__((aligned(4096)));
static uint64_t   g_scratch_arr[64]     __attribute__((aligned(4096)));
static uint8_t    g_scratch_buf[16][4096] __attribute__((aligned(4096)));

static uint8_t    d_input[MAX_DEV][4096] __attribute__((aligned(64)));
static uint8_t    d_output[MAX_DEV][4096] __attribute__((aligned(64)));
static xhci_trb_t d_ep0[MAX_DEV][XHCI_RING_SIZE] __attribute__((aligned(1024)));
static xhci_trb_t d_epr[MAX_DEV][XHCI_EPR_PER_DEV][XHCI_RING_SIZE] __attribute__((aligned(1024)));

static usb_device_t g_dev[MAX_DEV];
static int g_ndev;
static int g_last_cc;       // completion code of the most recent transfer

int xhci_last_cc(void) { return g_last_cc; }

// MSI-X plumbing (defined at end of file; NOT enabled by default).
int  xhci_enable_irq(void);
void xhci_irq_handler(void);

static pci_device_t g_pci;          // located xHCI PCI function (for MSI-X)
static volatile uint8_t* g_cap;     // capability base
static volatile uint8_t* g_op;      // operational base
static volatile uint8_t* g_rt;      // runtime base
static volatile uint32_t* g_db;     // doorbell array
static uint32_t g_max_ports;
static uint32_t g_ctx_stride;       // 32 or 64 bytes per context
static xhci_ring_t g_cmd_ring;
static struct { xhci_trb_t* trb; uint64_t phys; uint32_t deq; uint32_t cycle; } g_evt_ring;

static inline uint64_t phys_of(const void* p) { return kvirt_to_phys((uint64_t)(uintptr_t)p); }
static inline uint32_t cap_rd(uint32_t o) { return *(volatile uint32_t*)(g_cap + o); }
static inline uint32_t op_rd(uint32_t o) { return *(volatile uint32_t*)(g_op + o); }
static inline void     op_wr(uint32_t o, uint32_t v) { *(volatile uint32_t*)(g_op + o) = v; }
static inline void op_wr64(uint32_t o, uint64_t v) {
    *(volatile uint32_t*)(g_op + o) = (uint32_t)v;
    *(volatile uint32_t*)(g_op + o + 4) = (uint32_t)(v >> 32);
}
static inline uint32_t rt_rd(uint32_t o) { return *(volatile uint32_t*)(g_rt + o); }
static inline void     rt_wr(uint32_t o, uint32_t v) { *(volatile uint32_t*)(g_rt + o) = v; }
static inline void rt_wr64(uint32_t o, uint64_t v) {
    *(volatile uint32_t*)(g_rt + o) = (uint32_t)v;
    *(volatile uint32_t*)(g_rt + o + 4) = (uint32_t)(v >> 32);
}
static inline uint32_t port_rd(int p) { return *(volatile uint32_t*)(g_op + OP_PORTS + (p - 1) * 0x10); }
static inline void     port_wr(int p, uint32_t v) { *(volatile uint32_t*)(g_op + OP_PORTS + (p - 1) * 0x10) = v; }
static void zero(uint8_t* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) p[i] = 0; }
static uint32_t* ctx_dw(uint8_t* block, int index) { return (uint32_t*)(block + index * g_ctx_stride); }

static void ring_init(xhci_ring_t* r, xhci_trb_t* mem, uint8_t dci) {
    r->trb = mem; r->phys = phys_of(mem); r->enq = 0; r->cycle = 1; r->dci = dci;
    for (int i = 0; i < XHCI_RING_SIZE; i++) r->trb[i].d[0] = r->trb[i].d[1] = r->trb[i].d[2] = r->trb[i].d[3] = 0;
    // Last TRB is a Link back to the start, toggling cycle on wrap.
    r->trb[XHCI_RING_SIZE - 1].d[0] = (uint32_t)r->phys;
    r->trb[XHCI_RING_SIZE - 1].d[1] = (uint32_t)(r->phys >> 32);
    r->trb[XHCI_RING_SIZE - 1].d[2] = 0;
    r->trb[XHCI_RING_SIZE - 1].d[3] = TRB_TYPE(TRB_LINK) | TRB_TC | TRB_CYCLE;
}

// Enqueue a TRB; returns the physical address of the slot used (for matching).
static uint64_t ring_push(xhci_ring_t* r, uint32_t p0, uint32_t p1, uint32_t st, uint32_t ctrl) {
    uint64_t slot_phys = r->phys + (uint64_t)r->enq * sizeof(xhci_trb_t);
    xhci_trb_t* t = &r->trb[r->enq];
    t->d[0] = p0; t->d[1] = p1; t->d[2] = st;
    t->d[3] = ctrl | (r->cycle ? TRB_CYCLE : 0);
    r->enq++;
    if (r->enq == XHCI_RING_SIZE - 1) {
        // Reached the Link TRB: set its cycle to current, toggle, wrap.
        xhci_trb_t* lk = &r->trb[XHCI_RING_SIZE - 1];
        lk->d[3] = (lk->d[3] & ~TRB_CYCLE) | (r->cycle ? TRB_CYCLE : 0);
        r->cycle ^= 1u;
        r->enq = 0;
    }
    return slot_phys;
}

static void ring_db(int slot, uint32_t target) { g_db[slot] = target; io_mfence(); }

// Drain one event from the event ring; returns 1 and fills *out, else 0.
static int evt_poll(xhci_trb_t* out) {
    xhci_trb_t* t = &g_evt_ring.trb[g_evt_ring.deq];
    if ((t->d[3] & TRB_CYCLE ? 1u : 0u) != g_evt_ring.cycle) return 0;
    out->d[0] = t->d[0]; out->d[1] = t->d[1]; out->d[2] = t->d[2]; out->d[3] = t->d[3];
    g_evt_ring.deq++;
    if (g_evt_ring.deq == XHCI_RING_SIZE) { g_evt_ring.deq = 0; g_evt_ring.cycle ^= 1u; }
    uint64_t erdp = g_evt_ring.phys + (uint64_t)g_evt_ring.deq * sizeof(xhci_trb_t);
    rt_wr64(IR0_ERDP, erdp | (1u << 3));   // EHB: clear event-handler-busy
    return 1;
}

// Wait for an event of a given TRB type (port-change events are consumed).
// Returns 1 on match (event in *out), 0 on timeout.
static int evt_wait(uint32_t want_type, xhci_trb_t* out) {
    for (volatile uint64_t i = 0; i < 20000000ull; i++) {
        xhci_trb_t ev;
        if (evt_poll(&ev)) {
            uint32_t type = (ev.d[3] >> 10) & 0x3F;
            if (type == want_type) { *out = ev; return 1; }
            // else: port status change or stray event — keep draining.
            i = 0;
        }
    }
    return 0;
}

// Issue a command TRB and wait for its Command Completion Event. Returns the
// completion code, with the slot id in *slot_out (if non-NULL).
static int cmd_run(uint32_t p0, uint32_t p1, uint32_t st, uint32_t ctrl, int* slot_out) {
    ring_push(&g_cmd_ring, p0, p1, st, ctrl);
    ring_db(0, 0);
    xhci_trb_t ev;
    if (!evt_wait(TRB_CMD_COMPLETE, &ev)) return -1;
    if (slot_out) *slot_out = (int)((ev.d[3] >> 24) & 0xFF);
    return (int)((ev.d[2] >> 24) & 0xFF);   // completion code
}

int xhci_control(usb_device_t* d, const void* setup, void* data, int len) {
    const uint8_t* s = (const uint8_t*)setup;
    int in = (s[0] & 0x80) != 0;
    // Setup stage (immediate data: the 8 setup bytes live in the TRB).
    uint32_t sp0 = (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
    uint32_t sp1 = (uint32_t)s[4] | ((uint32_t)s[5] << 8) | ((uint32_t)s[6] << 16) | ((uint32_t)s[7] << 24);
    uint32_t trt = (len == 0) ? 0u : (in ? 3u : 2u);
    ring_push(&d->ep0, sp0, sp1, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
    if (len > 0) {
        uint64_t dp = phys_of(data);
        ring_push(&d->ep0, (uint32_t)dp, (uint32_t)(dp >> 32), (uint32_t)len,
                  TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN : 0));
    }
    // Status stage: direction opposite of data (IN if there was no/OUT data).
    ring_push(&d->ep0, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | ((len > 0 && in) ? 0 : TRB_DIR_IN));
    ring_db(d->slot_id, 1);   // EP0 doorbell, DCI 1

    xhci_trb_t ev;
    if (!evt_wait(TRB_XFER_EVENT, &ev)) { g_last_cc = -1; return -1; }
    int cc = (ev.d[2] >> 24) & 0xFF;
    g_last_cc = cc;
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) return -1;
    return len;
}

int xhci_set_ep0_mps(usb_device_t* d, uint16_t mps) {
    uint8_t* in = d->input_ctx;
    zero(in, g_ctx_stride * 3);
    ctx_dw(in, 0)[1] = (1u << 1);                 // add A1 (EP0)
    uint32_t* ep0 = ctx_dw(in, 2);
    ep0[1] = (4u << 3) | (3u << 1) | ((uint32_t)mps << 16);  // control, CErr=3, MPS
    uint64_t rp = d->ep0.phys;
    ep0[2] = (uint32_t)rp | 1u;                              // DCS=1 (ignored by Evaluate Context)
    ep0[3] = (uint32_t)(rp >> 32);
    ep0[4] = 8;
    int cc = cmd_run((uint32_t)phys_of(in), (uint32_t)(phys_of(in) >> 32), 0,
                     TRB_TYPE(TRB_EVAL_CTX) | ((uint32_t)d->slot_id << 24), NULL);
    return (cc == CC_SUCCESS) ? 0 : -1;
}

static uint8_t ep_dci(uint8_t addr) {
    uint8_t num = addr & 0x0F;
    return (uint8_t)(num * 2 + ((addr & 0x80) ? 1 : 0));
}

int xhci_configure_endpoints(usb_device_t* d, const usb_endpoint_t* eps, int n) {
    uint8_t* in = d->input_ctx;
    zero(in, sizeof(d_input[0]));
    uint32_t add = (1u << 0);    // A0 (slot context)
    uint8_t max_dci = 1;
    d->n_epr = 0;
    for (int i = 0; i < n && d->n_epr < XHCI_EPR_PER_DEV; i++) {
        uint8_t dci = ep_dci(eps[i].addr);
        if (dci > max_dci) max_dci = dci;
        add |= (1u << dci);
        xhci_ring_t* r = &d->epr[d->n_epr];
        ring_init(r, d_epr[d->devidx][d->n_epr], dci);
        d->n_epr++;
        // Endpoint type: bulk=2 attr -> EP type 2(out)/6(in); interrupt=3 -> 3(out)/7(in).
        uint8_t tt = eps[i].attr & 0x3;
        uint8_t in_dir = (eps[i].addr & 0x80) ? 1 : 0;
        uint8_t eptype = (tt == 2) ? (in_dir ? 6 : 2) : (in_dir ? 7 : 3);
        uint32_t* ec = ctx_dw(in, dci + 1);
        ec[0] = (eps[i].interval ? ((uint32_t)(eps[i].interval - 1) << 16) : 0);
        ec[1] = ((uint32_t)eptype << 3) | (3u << 1) | ((uint32_t)eps[i].max_packet << 16);
        ec[2] = (uint32_t)r->phys | 1u;   // DCS=1
        ec[3] = (uint32_t)(r->phys >> 32);
        ec[4] = eps[i].max_packet;        // average TRB length ~ max packet
    }
    ctx_dw(in, 0)[1] = add;               // input control: add flags
    // Slot context: keep speed/port, bump Context Entries to the highest DCI.
    uint32_t* slot = ctx_dw(in, 1);
    slot[0] = ((uint32_t)d->speed << 20) | ((uint32_t)max_dci << 27);
    slot[1] = ((uint32_t)d->port << 16);
    int cc = cmd_run((uint32_t)phys_of(in), (uint32_t)(phys_of(in) >> 32), 0,
                     TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)d->slot_id << 24), NULL);
    return (cc == CC_SUCCESS) ? 0 : -1;
}

int xhci_submit(usb_device_t* d, uint8_t ep_addr, void* data, int len) {
    uint8_t dci = ep_dci(ep_addr);
    xhci_ring_t* r = NULL;
    for (int i = 0; i < d->n_epr; i++) if (d->epr[i].dci == dci) { r = &d->epr[i]; break; }
    if (!r) return -1;
    uint64_t dp = phys_of(data);
    ring_push(r, (uint32_t)dp, (uint32_t)(dp >> 32), (uint32_t)len,
              TRB_TYPE(TRB_NORMAL) | TRB_IOC | (1u << 2 /* ISP */));
    ring_db(d->slot_id, dci);
    return 0;
}

int xhci_poll_transfer(usb_device_t* d, uint8_t ep_addr, int len, int* bytes) {
    uint8_t dci = ep_dci(ep_addr);
    xhci_trb_t ev;
    if (!evt_poll(&ev)) return 0;
    if (((ev.d[3] >> 10) & 0x3F) != TRB_XFER_EVENT) return 0;
    int slot = (ev.d[3] >> 24) & 0xFF;
    int eid  = (ev.d[3] >> 16) & 0x1F;
    if (slot != d->slot_id || eid != (int)dci) return 0;   // not ours — drop
    int cc = (ev.d[2] >> 24) & 0xFF;
    g_last_cc = cc;
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) { *bytes = -1; return 1; }
    *bytes = len - (int)(ev.d[2] & 0xFFFFFF);
    return 1;
}

int xhci_transfer(usb_device_t* d, uint8_t ep_addr, void* data, int len) {
    if (xhci_submit(d, ep_addr, data, len) < 0) return -1;
    xhci_trb_t ev;
    if (!evt_wait(TRB_XFER_EVENT, &ev)) { g_last_cc = -1; return -1; }
    int cc = (ev.d[2] >> 24) & 0xFF;
    g_last_cc = cc;
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) return -1;
    return len - (int)(ev.d[2] & 0xFFFFFF);
}

// Find the transfer ring serving a given DCI (1 = EP0).
static xhci_ring_t* ring_for_dci(usb_device_t* d, uint8_t dci) {
    if (dci == 1) return &d->ep0;
    for (int i = 0; i < d->n_epr; i++) if (d->epr[i].dci == dci) return &d->epr[i];
    return NULL;
}

int xhci_reset_endpoint(usb_device_t* d, uint8_t ep_addr) {
    uint8_t dci = (ep_addr == 0) ? 1 : ep_dci(ep_addr);
    xhci_ring_t* r = ring_for_dci(d, dci);
    if (!r) return -1;
    // Reset Endpoint: moves the halted endpoint back to Stopped.
    int cc = cmd_run(0, 0, 0, TRB_TYPE(TRB_RESET_EP) |
                     ((uint32_t)dci << 16) | ((uint32_t)d->slot_id << 24), NULL);
    if (cc != CC_SUCCESS) return -1;
    // Reset the software ring (enq=0, cycle=1, link rebuilt) and point the HW
    // dequeue at its base with DCS=1 to match.
    ring_init(r, r->trb, dci);
    uint64_t dq = r->phys | 1u;
    cc = cmd_run((uint32_t)dq, (uint32_t)(dq >> 32), 0, TRB_TYPE(TRB_SET_TR_DEQ) |
                 ((uint32_t)dci << 16) | ((uint32_t)d->slot_id << 24), NULL);
    return (cc == CC_SUCCESS) ? 0 : -1;
}

// Reset one root port and return its speed (0 if it failed to enable).
static int port_reset(int p) {
    uint32_t sc = port_rd(p);
    if (!(sc & PORTSC_CCS)) return 0;
    // USB3 ports auto-enable on connect; otherwise drive a port reset.
    if (!(sc & PORTSC_PED)) {
        uint32_t v = (sc & ~PORTSC_RW1C & ~PORTSC_PED) | PORTSC_PR;
        port_wr(p, v);
        for (volatile uint64_t i = 0; i < 5000000ull; i++) {
            sc = port_rd(p);
            if (sc & PORTSC_PRC) break;
        }
    }
    sc = port_rd(p);
    // Acknowledge connect + reset change bits.
    port_wr(p, (sc & ~PORTSC_PED) | PORTSC_CSC | PORTSC_PRC);
    sc = port_rd(p);
    if (!(sc & PORTSC_PED)) return 0;
    return (int)((sc >> 10) & 0xF);
}

void xhci_enumerate(void) {
    extern void usb_attach(usb_device_t* d);
    for (uint32_t p = 1; p <= g_max_ports && g_ndev < MAX_DEV; p++) {
        int speed = port_reset((int)p);
        if (speed == 0) continue;
        // Enable a device slot.
        int slot = 0;
        int cc = cmd_run(0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot);
        if (cc != CC_SUCCESS || slot <= 0 || slot >= 256) {
            debugcon_writestring("[XHCI] enable slot failed\n"); continue;
        }
        usb_device_t* d = &g_dev[g_ndev];
        d->slot_id = slot; d->port = (int)p; d->speed = speed; d->devidx = g_ndev;
        d->input_ctx = d_input[g_ndev]; d->output_ctx = d_output[g_ndev];
        d->n_epr = 0; d->class_id = 0; d->class_priv = NULL;
        ring_init(&d->ep0, d_ep0[g_ndev], 1);
        zero(d->output_ctx, sizeof(d_output[0]));
        g_dcbaa[slot] = phys_of(d->output_ctx);

        // Address Device: input ctx with A0 (slot) + A1 (EP0).
        uint8_t* in = d->input_ctx;
        zero(in, sizeof(d_input[0]));
        ctx_dw(in, 0)[1] = (1u << 0) | (1u << 1);
        uint32_t* sc = ctx_dw(in, 1);
        sc[0] = ((uint32_t)speed << 20) | (1u << 27);   // context entries = 1
        sc[1] = ((uint32_t)p << 16);                    // root hub port number
        uint16_t mps0 = (speed == 4) ? 512 : (speed == 3) ? 64 : 8;
        uint32_t* ep0 = ctx_dw(in, 2);
        ep0[1] = (4u << 3) | (3u << 1) | ((uint32_t)mps0 << 16);
        ep0[2] = (uint32_t)d->ep0.phys | 1u;
        ep0[3] = (uint32_t)(d->ep0.phys >> 32);
        ep0[4] = 8;
        cc = cmd_run((uint32_t)phys_of(in), (uint32_t)(phys_of(in) >> 32), 0,
                     TRB_TYPE(TRB_ADDRESS_DEV) | ((uint32_t)slot << 24), NULL);
        if (cc != CC_SUCCESS) { debugcon_writestring("[XHCI] address device failed\n"); continue; }

        debugcon_writestring("[XHCI] port "); debugcon_print_hex(p);
        debugcon_writestring(" slot "); debugcon_print_hex((uint64_t)slot);
        debugcon_writestring(" speed "); debugcon_print_hex((uint64_t)speed);
        debugcon_writestring(" addressed\n");
        g_ndev++;
        usb_attach(d);   // USB core: descriptors, set config, class bind
    }
}

int xhci_init(void) {
    pci_device_t dev;
    if (pci_find_class(0x0C, 0x03, 0x30, &dev) != 0) {
        debugcon_writestring("[XHCI] no controller on PCI\n");
        return -1;
    }
    g_pci = dev;   // remembered for optional MSI-X (gated, NOT enabled by default)
    pci_enable_mem_and_busmaster(&dev);
    uint64_t bar = pci_bar_mem64(&dev, 0);
    if (bar == 0) { debugcon_writestring("[XHCI] BAR0 not memory\n"); return -1; }
    vmm_extend_physmap(bar + 0x10000);
    g_cap = (volatile uint8_t*)phys_to_virt(bar);

    uint8_t caplen = (uint8_t)(cap_rd(CAP_CAPLENGTH) & 0xFF);
    g_op = g_cap + caplen;
    g_rt = g_cap + (cap_rd(CAP_RTSOFF) & ~0x1Fu);
    g_db = (volatile uint32_t*)(g_cap + (cap_rd(CAP_DBOFF) & ~0x3u));
    uint32_t hcs1 = cap_rd(CAP_HCSPARAMS1);
    uint32_t max_slots = hcs1 & 0xFF;
    g_max_ports = (hcs1 >> 24) & 0xFF;
    uint32_t hcc1 = cap_rd(CAP_HCCPARAMS1);
    g_ctx_stride = (hcc1 & (1u << 2)) ? 64 : 32;
    debugcon_writestring("[XHCI] BAR=0x"); debugcon_print_hex(bar);
    debugcon_writestring(" slots="); debugcon_print_hex(max_slots);
    debugcon_writestring(" ports="); debugcon_print_hex(g_max_ports);
    debugcon_writestring(" ctx="); debugcon_print_hex(g_ctx_stride); debugcon_writestring("\n");

    // Reset the controller: stop, HCRST, wait for clear + not-ready clear.
    op_wr(OP_USBCMD, op_rd(OP_USBCMD) & ~USBCMD_RS);
    for (volatile uint64_t i = 0; !(op_rd(OP_USBSTS) & USBSTS_HCH); i++)
        if (i > 10000000ull) break;
    op_wr(OP_USBCMD, op_rd(OP_USBCMD) | USBCMD_HCRST);
    for (volatile uint64_t i = 0; (op_rd(OP_USBCMD) & USBCMD_HCRST); i++)
        if (i > 20000000ull) { debugcon_writestring("[XHCI] reset timeout\n"); return -1; }
    for (volatile uint64_t i = 0; (op_rd(OP_USBSTS) & USBSTS_CNR); i++)
        if (i > 20000000ull) { debugcon_writestring("[XHCI] CNR timeout\n"); return -1; }

    // DCBAA + scratchpad buffers.
    for (int i = 0; i < 256; i++) g_dcbaa[i] = 0;
    uint32_t hcs2 = cap_rd(CAP_HCSPARAMS2);
    uint32_t spb = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (spb > 16) spb = 16;
    if (spb > 0) {
        for (uint32_t i = 0; i < spb; i++) {
            zero(g_scratch_buf[i], 4096);
            g_scratch_arr[i] = phys_of(g_scratch_buf[i]);
        }
        g_dcbaa[0] = phys_of(g_scratch_arr);
    }
    op_wr64(OP_DCBAAP, phys_of(g_dcbaa));
    op_wr(OP_CONFIG, max_slots);

    // Command ring.
    ring_init(&g_cmd_ring, g_cmd, 0);
    op_wr64(OP_CRCR, g_cmd_ring.phys | 1u /* RCS */);

    // Event ring (single ERST segment).
    g_evt_ring.trb = g_evt; g_evt_ring.phys = phys_of(g_evt);
    g_evt_ring.deq = 0; g_evt_ring.cycle = 1;
    for (int i = 0; i < XHCI_RING_SIZE; i++) g_evt[i].d[0] = g_evt[i].d[1] = g_evt[i].d[2] = g_evt[i].d[3] = 0;
    uint64_t* erst = (uint64_t*)g_erst;
    erst[0] = g_evt_ring.phys;
    erst[1] = XHCI_RING_SIZE;             // segment size (low 16 bits)
    rt_wr(IR0_ERSTSZ, 1);
    rt_wr64(IR0_ERDP, g_evt_ring.phys | (1u << 3));
    rt_wr64(IR0_ERSTBA, phys_of(g_erst));
    rt_wr(IR0_IMAN, rt_rd(IR0_IMAN) | 0x3);   // clear IP, set IE (we still poll)

    // Run.
    op_wr(OP_USBCMD, op_rd(OP_USBCMD) | USBCMD_RS);
    for (volatile uint64_t i = 0; (op_rd(OP_USBSTS) & USBSTS_HCH); i++)
        if (i > 10000000ull) { debugcon_writestring("[XHCI] run timeout\n"); return -1; }

    // Some controllers need a moment for ports to settle after RS.
    for (volatile uint64_t i = 0; i < 2000000ull; i++) { }
    debugcon_writestring("[XHCI] running\n");
    g_ndev = 0;

#ifdef XHCI_USE_IRQ
    // MSI-X interrupt mode — IMPLEMENTED BUT NOT YET TESTED. OFF by default; the
    // completion path above (evt_poll / evt_wait) remains the working default.
    // When this gate is built, we additionally arm MSI-X so the handler can
    // drain the event ring; the two coexist on a single CPU because the handler
    // only advances ERDP, which the poller tolerates.
    xhci_enable_irq();
#endif
    return 0;
}

/* ============================================================================
 * MSI-X interrupt support — IMPLEMENTED BUT NOT YET TESTED / not enabled by
 * default (the kernel is polled). Needs the LAPIC + hardware validation. The
 * polled completion path (evt_poll/evt_wait) stays the working default; this
 * handler only drains the event ring and acknowledges, so an unexpected
 * interrupt can never corrupt an in-flight polled transfer.
 * ========================================================================== */

// Volatile flag a future interrupt-driven waiter could spin on instead of the
// event ring. Set by the ISR; unused by the default polled path.
static volatile uint32_t g_xhci_irq_count;

// C half of isr_xhci (vector 0x40). Acknowledge the interrupter, advance the
// event ring dequeue (so the controller stops asserting), and EOI the LAPIC.
void xhci_irq_handler(void) {
    if (g_rt) {
        // RW1C the interrupter's IP bit and re-arm IE.
        uint32_t iman = rt_rd(IR0_IMAN);
        rt_wr(IR0_IMAN, iman | IMAN_IP | IMAN_IE);
        // Drain any pending events to clear EHB and advance ERDP.
        xhci_trb_t ev;
        while (evt_poll(&ev)) { /* consumed; ERDP advanced inside evt_poll */ }
    }
    g_xhci_irq_count++;
    lapic_eoi();
}

// Bring up MSI-X delivery for the xHCI event interrupter. NOT YET TESTED.
// Returns 0 on success, -1 if MSI-X/LAPIC could not be programmed. Safe to
// leave unused: the default build never calls it.
int xhci_enable_irq(void) {
    if (lapic_enable() != 0) {
        debugcon_writestring("[MSIX] xhci: LAPIC unavailable, staying polled\n");
        return -1;
    }
    idt_install_msi_vector(IDT_VECTOR_XHCI, isr_xhci);
    if (pci_enable_msix(&g_pci, IDT_VECTOR_XHCI) != 0) {
        // Fall back to plain MSI if the controller exposes only that.
        if (pci_enable_msi(&g_pci, IDT_VECTOR_XHCI) != 0) {
            debugcon_writestring("[MSIX] xhci: no MSI-X/MSI capability\n");
            return -1;
        }
    }
    // Make sure interrupter 0 has IE set; IP is RW1C so writing it clears it.
    if (g_rt) rt_wr(IR0_IMAN, rt_rd(IR0_IMAN) | IMAN_IE | IMAN_IP);
    debugcon_writestring("[MSIX] xhci vector=0x40 armed (NOT TESTED)\n");
    return 0;
}
