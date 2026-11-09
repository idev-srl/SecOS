/*
 * net_udrv.c — [M58] the kernel proxy of a ring-3 NIC driver
 * (docs/PHASE5_PLAN.md D6, docs/devlog/M58-udrv-e1000e.md).
 * SPDX-License-Identifier: MIT
 *
 * A net_dev_t whose transmit()/poll() move frames through the region shared
 * with the driver (ABI and ring protocol: user/include/secos/udrv_net.h).
 * The kernel keeps its own copy of every index and of the owner of every
 * slot; the shared memory is read once per field and never trusted:
 *   - an index may be ahead of the kernel's copy by at most the ring size;
 *   - a slot must be in range and owned by the driver for that direction;
 *   - a received length must be UDRV_NET_FRAME_MIN..MAX.
 * A bad entry is dropped, counted and logged; UDRV_NET_VIOL_KILL of them kill
 * the driver (kernel/udrv.c). Frames are copied out of the slot into a
 * kernel buffer before the stack sees them, so the driver (or its device)
 * cannot change a frame while the stack parses it.
 */
#include "net.h"
#include "net_udrv.h"
#include "../user/include/secos/udrv.h"
#include "../user/include/secos/udrv_net.h"
#include "spinlock.h"
#include "lock_order.h"
#include "iommu.h"
#include "debugcon.h"
#include "tsc.h"
#include <stddef.h>

_Static_assert(sizeof(struct udrv_net_ctrl) <= UDRV_NET_CTRL_PAGES * 4096, "ctrl region");
_Static_assert(offsetof(struct udrv_net_ctrl, tx_prod) == 64, "ctrl layout");
_Static_assert(offsetof(struct udrv_net_ctrl, txc_prod) == 128, "ctrl layout");
_Static_assert(offsetof(struct udrv_net_ctrl, tx) == 256, "ctrl layout");
_Static_assert(sizeof(struct udrv_net_desc) == 8, "desc");
_Static_assert(sizeof(struct udrv_net_attach) == 56, "attach");
_Static_assert(UDRV_NET_FRAME_MAX == NET_FRAME_MAX, "frame max");
_Static_assert((UDRV_NET_TX_RING & (UDRV_NET_TX_RING - 1)) == 0 && (UDRV_NET_RX_RING & (UDRV_NET_RX_RING - 1)) == 0, "rings");

#define NET_UDRV_MAX    2
#define RX_BATCH        16
#define STALL_MS        5000ull
#define VIOL_LOG_MAX    UDRV_NET_VIOL_KILL   /* every bad entry up to the kill is logged */

/* ---------------------------------------------------------- the checks
 * Pure functions (no lock, no side effect): one verdict per shared field.
 * Kept separate from the ring code so they can be tested on their own. */
const char* udrv_net_verdict_name(int v) {
    switch (v) {
    case UNV_OK:          return "accepted";
    case UNV_BAD_INDEX:   return "producer index outside the ring";
    case UNV_SLOT_RANGE:  return "slot index out of range";
    case UNV_NOT_OWNED:   return "slot not owned by the driver";
    case UNV_BAD_LEN:     return "length out of range";
    default:              return "?";
    }
}

int udrv_net_check_index(uint32_t prod, uint32_t k_cons, uint32_t ring) {
    return (prod - k_cons) <= ring ? UNV_OK : UNV_BAD_INDEX;
}

int udrv_net_check_rx(uint32_t slot, uint32_t len, const uint8_t* own) {
    if (slot < UDRV_NET_RX_BASE || slot >= UDRV_NET_NSLOTS) return UNV_SLOT_RANGE;
    if (own[slot] != OWN_RX) return UNV_NOT_OWNED;
    if (len < UDRV_NET_FRAME_MIN || len > UDRV_NET_FRAME_MAX) return UNV_BAD_LEN;
    return UNV_OK;
}

int udrv_net_check_txc(uint32_t slot, const uint8_t* own) {
    if (slot < UDRV_NET_TX_BASE || slot >= UDRV_NET_TX_BASE + UDRV_NET_TX_SLOTS) return UNV_SLOT_RANGE;
    if (own[slot] != OWN_TX) return UNV_NOT_OWNED;
    return UNV_OK;
}

int udrv_net_reject_count(uint32_t* count, int* kill_sent) {
    (*count)++;
    if (*count >= UDRV_NET_VIOL_KILL && !*kill_sent) { *kill_sent = 1; return 1; }
    return 0;
}

struct net_udrv {
    net_dev_t        dev;               /* first: poll/transmit get &dev */
    const pci_dev_t* pd;
    spinlock_t       lk;                /* LK_S22_UDRV_NET */
    int              attached;
    struct udrv_net_ctrl* ctrl;         /* kernel mapping (physmap) */
    uint8_t*         slots;             /* kernel mapping (physmap) */
    void*            cookie;            /* the claim (kernel/udrv.c) */
    uint32_t         gen, pid;
    /* the kernel's own ring state */
    uint32_t k_tx_prod, k_txc_cons, k_rx_cons, k_rxf_prod;
    uint8_t  own[UDRV_NET_NSLOTS];
    uint16_t txfree[UDRV_NET_TX_SLOTS];
    uint32_t ntxfree, tx_inflight;
    uint64_t tx_progress_ms;
    int      stall_logged;
    uint32_t viol;                      /* this claim */
    int      kill_sent;
    /* statistics (lk) */
    uint64_t viol_total, attaches, deaths, tx_full, tx_frames, rx_frames, stalls;
    /* received frames, copied out of the slots (rx_lock: poll only) */
    uint8_t  rxb[RX_BATCH][UDRV_NET_FRAME_MAX];
    uint16_t rxl[RX_BATCH];
};

static net_udrv_t g_px[NET_UDRV_MAX];
static int        g_npx;

extern int net_poll_add(net_dev_t* dev);

static uint64_t now_ms(void) { return ktime_ns() / 1000000ull; }

typedef uint64_t __attribute__((may_alias, aligned(1))) u64_alias_t;
static void copy_bytes(void* dst, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dst; const uint8_t* s = (const uint8_t*)src;
    while (n >= 8) { *(u64_alias_t*)d = *(const u64_alias_t*)s; d += 8; s += 8; n -= 8; }
    while (n--) *d++ = *s++;
}

static void ln_px(iommu_line_t* l, const net_udrv_t* px) {
    iommu_ln_s(l, "[UDRV] "); iommu_ln_s(l, px->dev.name);
}

/* A bad entry from the driver (lk held). */
static void violation(net_udrv_t* px, const char* what, uint32_t idx, uint32_t slot, uint32_t len) {
    int kill = udrv_net_reject_count(&px->viol, &px->kill_sent);
    px->viol_total++;
    if (px->viol <= VIOL_LOG_MAX) {
        iommu_line_t l; l.n = 0;
        ln_px(&l, px);
        iommu_ln_s(&l, " bad ring entry: "); iommu_ln_s(&l, what);
        iommu_ln_s(&l, " (index "); iommu_ln_dec(&l, idx);
        iommu_ln_s(&l, " slot "); iommu_ln_dec(&l, slot);
        iommu_ln_s(&l, " len "); iommu_ln_dec(&l, len);
        iommu_ln_s(&l, ") dropped, "); iommu_ln_dec(&l, px->viol);
        iommu_ln_s(&l, "/"); iommu_ln_dec(&l, UDRV_NET_VIOL_KILL);
        iommu_ln_s(&l, " pid="); iommu_ln_dec(&l, px->pid);
        debugcon_writestring(iommu_ln_end(&l));
    }
    if (kill) udrv_net_kill(px->cookie, px->gen, "too many rejected ring entries");
}

static void post_rx_locked(net_udrv_t* px, uint16_t slot) {
    struct udrv_net_desc* e = &px->ctrl->rxf[px->k_rxf_prod & (UDRV_NET_RX_RING - 1)];
    e->slot = slot; e->len = 0; e->rsvd = 0;
    px->own[slot] = OWN_RX;
    px->k_rxf_prod++;
}

static void publish_rxf_locked(net_udrv_t* px) {
    __atomic_thread_fence(__ATOMIC_RELEASE);
    px->ctrl->rxf_prod = px->k_rxf_prod;
}

/* Take back the TX slots the driver reports done (lk held). */
static void reap_tx_locked(net_udrv_t* px) {
    struct udrv_net_ctrl* c = px->ctrl;
    uint32_t prod = __atomic_load_n(&c->txc_prod, __ATOMIC_ACQUIRE);
    uint32_t avail = prod - px->k_txc_cons;
    if (udrv_net_check_index(prod, px->k_txc_cons, UDRV_NET_TX_RING) != UNV_OK) {
        violation(px, "txc producer index outside the ring", prod, 0, 0);
        return;
    }
    while (avail--) {
        uint32_t idx = px->k_txc_cons & (UDRV_NET_TX_RING - 1);
        uint32_t slot = *(volatile uint16_t*)&c->txc[idx].slot;
        px->k_txc_cons++;
        int v = udrv_net_check_txc(slot, px->own);
        if (v != UNV_OK) {
            violation(px, v == UNV_SLOT_RANGE ? "txc slot index out of range" : "txc slot not owned by the driver",
                      px->k_txc_cons - 1, slot, 0);
            continue;
        }
        px->own[slot] = OWN_KERNEL;
        px->txfree[px->ntxfree++] = (uint16_t)slot;
        px->tx_inflight--;
        px->tx_progress_ms = now_ms();
        px->stall_logged = 0;
    }
    c->txc_done = px->k_txc_cons;
}

static void stall_check_locked(net_udrv_t* px) {
    if (!px->tx_inflight || px->stall_logged) return;
    if (now_ms() - px->tx_progress_ms < STALL_MS) return;
    px->stall_logged = 1;
    px->stalls++;
    iommu_line_t l; l.n = 0;
    ln_px(&l, px);
    iommu_ln_s(&l, " stalled: "); iommu_ln_dec(&l, px->tx_inflight);
    iommu_ln_s(&l, " frame(s) queued, no TX completion for 5 s, driver pid="); iommu_ln_dec(&l, px->pid);
    iommu_ln_s(&l, udrv_watchdog_kill() ? " (udrv.watchdog=kill: killing it)" : "");
    debugcon_writestring(iommu_ln_end(&l));
    if (udrv_watchdog_kill() && !px->kill_sent) {
        px->kill_sent = 1;
        udrv_net_kill(px->cookie, px->gen, "TX stalled");
    }
}

/* ---------------------------------------------------------- net_dev ops */
static int px_transmit(net_dev_t* dev, const void* frame, uint32_t len) {
    net_udrv_t* px = (net_udrv_t*)dev;
    if (!frame || len < UDRV_NET_FRAME_MIN || len > UDRV_NET_FRAME_MAX) return -1;
    uint64_t fl = spin_lock_irqsave(&px->lk);
    if (!px->attached) { spin_unlock_irqrestore(&px->lk, fl); return -1; }
    reap_tx_locked(px);
    if (!px->attached || px->ntxfree == 0) {
        px->tx_full++;
        if (px->attached) udrv_net_notify(px->cookie, px->gen);   /* let it reclaim */
        spin_unlock_irqrestore(&px->lk, fl);
        return -1;
    }
    uint16_t slot = px->txfree[--px->ntxfree];
    copy_bytes(px->slots + (uint64_t)slot * UDRV_NET_SLOT_SIZE, frame, len);
    struct udrv_net_desc* e = &px->ctrl->tx[px->k_tx_prod & (UDRV_NET_TX_RING - 1)];
    e->slot = slot; e->len = (uint16_t)len; e->rsvd = 0;
    px->own[slot] = OWN_TX;
    if (px->tx_inflight++ == 0) px->tx_progress_ms = now_ms();
    px->k_tx_prod++;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    px->ctrl->tx_prod = px->k_tx_prod;
    px->tx_frames++;
    udrv_net_notify(px->cookie, px->gen);
    spin_unlock_irqrestore(&px->lk, fl);
    return 0;
}

/* rx_lock held (net_dev_poll). Copy up to RX_BATCH frames out under lk, hand
 * them to the stack after dropping it; repeat until rx[] is empty. */
static void px_poll(net_dev_t* dev) {
    net_udrv_t* px = (net_udrv_t*)dev;
    for (int round = 0; round < 64; round++) {
        int n = 0, more = 0;
        uint64_t fl = spin_lock_irqsave(&px->lk);
        if (!px->attached) { spin_unlock_irqrestore(&px->lk, fl); return; }
        struct udrv_net_ctrl* c = px->ctrl;
        reap_tx_locked(px);
        if (px->attached) {
            dev->link_up = (__atomic_load_n(&c->link, __ATOMIC_RELAXED) == 1);
            uint32_t prod = __atomic_load_n(&c->rx_prod, __ATOMIC_ACQUIRE);
            uint32_t avail = prod - px->k_rx_cons;
            if (udrv_net_check_index(prod, px->k_rx_cons, UDRV_NET_RX_RING) != UNV_OK) {
                violation(px, "rx producer index outside the ring", prod, 0, 0);
                avail = 0;
            }
            uint32_t posted = 0;
            while (avail && n < RX_BATCH) {
                avail--;
                uint32_t idx = px->k_rx_cons & (UDRV_NET_RX_RING - 1);
                struct udrv_net_desc e;
                e.slot = *(volatile uint16_t*)&c->rx[idx].slot;
                e.len = *(volatile uint16_t*)&c->rx[idx].len;
                px->k_rx_cons++;
                int v = udrv_net_check_rx(e.slot, e.len, px->own);
                if (v == UNV_SLOT_RANGE || v == UNV_NOT_OWNED) {
                    violation(px, v == UNV_SLOT_RANGE ? "rx slot index out of range" : "rx slot not owned by the driver",
                              px->k_rx_cons - 1, e.slot, e.len);
                    continue;
                }
                px->own[e.slot] = OWN_KERNEL;
                if (v == UNV_BAD_LEN) {
                    violation(px, "rx length out of range", px->k_rx_cons - 1, e.slot, e.len);
                    post_rx_locked(px, e.slot);
                    posted++;
                    continue;
                }
                copy_bytes(px->rxb[n], px->slots + (uint64_t)e.slot * UDRV_NET_SLOT_SIZE, e.len);
                px->rxl[n] = e.len;
                n++;
                post_rx_locked(px, e.slot);
                posted++;
            }
            more = avail != 0;
            c->rx_done = px->k_rx_cons;
            if (posted) publish_rxf_locked(px);
            px->rx_frames += (uint64_t)n;
            stall_check_locked(px);
        }
        spin_unlock_irqrestore(&px->lk, fl);
        for (int i = 0; i < n; i++) net_rx(dev, px->rxb[i], px->rxl[i]);
        if (!more) return;
    }
}

/* ------------------------------------------------------------ attach */
int net_udrv_init(void) {
    for (int i = 0; ; i++) {
        const pci_dev_t* pd = udrv_assigned(i);
        if (!pd) break;
        if (pd->class_code != 0x02) continue;              /* network controllers */
        if (g_npx >= NET_UDRV_MAX) { debugcon_writestring("[NET] too many ring-3 NICs; proxy skipped\n"); break; }
        net_udrv_t* px = &g_px[g_npx];
        px->pd = pd;
        spin_init_class_dynamic(&px->lk, LK_S22_UDRV_NET, "net-udrv");
        px->dev.transmit = px_transmit;
        px->dev.poll = px_poll;
        px->dev.pci.bus = pd->bus; px->dev.pci.slot = pd->slot; px->dev.pci.func = pd->func;
        px->dev.pci.vendor_id = pd->vendor_id; px->dev.pci.device_id = pd->device_id;
        px->dev.link_up = 0;
        px->dev.irq_mode = NET_IRQ_POLL;
        if (net_register_dev(&px->dev) != 0) { debugcon_writestring("[NET] ring-3 NIC proxy: registry full\n"); break; }
        g_npx++;
        net_poll_add(&px->dev);                            /* the 1 kHz backstop + watchdog */
        iommu_line_t l; l.n = 0;
        iommu_ln_s(&l, "[NET] "); iommu_ln_s(&l, px->dev.name);
        iommu_ln_s(&l, ": ring-3 driver proxy for ");
        iommu_ln_bdf(&l, (uint16_t)((pd->bus << 8) | (pd->slot << 3) | pd->func));
        iommu_ln_s(&l, " "); iommu_ln_hexw(&l, pd->vendor_id, 4); iommu_ln_s(&l, ":"); iommu_ln_hexw(&l, pd->device_id, 4);
        iommu_ln_s(&l, " (link down until its driver attaches)");
        debugcon_writestring(iommu_ln_end(&l));
    }
    return g_npx;
}

net_udrv_t* net_udrv_find(const pci_dev_t* pd) {
    for (int i = 0; i < g_npx; i++) if (g_px[i].pd == pd) return &g_px[i];
    return NULL;
}

int net_udrv_attach(net_udrv_t* px, void* ctrl, void* slots, const uint8_t mac[6],
                    void* cookie, uint32_t gen, uint32_t pid, char ifname[16]) {
    if (!px || !ctrl || !slots) return -22;
    /* The MAC is read by the stack under its lock. */
    uint64_t nf = net_lock();
    for (int i = 0; i < 6; i++) px->dev.mac[i] = mac[i];
    net_unlock(nf);
    uint64_t fl = spin_lock_irqsave(&px->lk);
    if (px->attached) { spin_unlock_irqrestore(&px->lk, fl); return -16; }
    struct udrv_net_ctrl* c = (struct udrv_net_ctrl*)ctrl;
    c->magic = UDRV_NET_MAGIC; c->version = UDRV_NET_VERSION;
    c->nslots = UDRV_NET_NSLOTS; c->slot_size = UDRV_NET_SLOT_SIZE;
    c->tx_base = UDRV_NET_TX_BASE; c->tx_slots = UDRV_NET_TX_SLOTS;
    c->rx_base = UDRV_NET_RX_BASE; c->rx_slots = UDRV_NET_RX_SLOTS;
    c->tx_ring = UDRV_NET_TX_RING; c->rx_ring = UDRV_NET_RX_RING;
    c->ctrl_len = UDRV_NET_CTRL_PAGES * 4096; c->frame_max = UDRV_NET_FRAME_MAX;
    px->ctrl = c; px->slots = (uint8_t*)slots;
    px->cookie = cookie; px->gen = gen; px->pid = pid;
    px->k_tx_prod = px->k_txc_cons = px->k_rx_cons = px->k_rxf_prod = 0;
    for (uint32_t i = 0; i < UDRV_NET_NSLOTS; i++) px->own[i] = OWN_KERNEL;
    px->ntxfree = 0;
    for (uint32_t i = UDRV_NET_TX_SLOTS; i-- > 0; ) px->txfree[px->ntxfree++] = (uint16_t)(UDRV_NET_TX_BASE + i);
    px->tx_inflight = 0; px->tx_progress_ms = now_ms(); px->stall_logged = 0;
    px->viol = 0; px->kill_sent = 0;
    for (uint32_t i = 0; i < UDRV_NET_RX_SLOTS; i++) post_rx_locked(px, (uint16_t)(UDRV_NET_RX_BASE + i));
    publish_rxf_locked(px);
    px->attached = 1;
    px->attaches++;
    spin_unlock_irqrestore(&px->lk, fl);
    for (int i = 0; i < 16; i++) ifname[i] = (i < 15) ? px->dev.name[i] : 0;
    iommu_line_t l; l.n = 0;
    ln_px(&l, px);
    iommu_ln_s(&l, " driver attached pid="); iommu_ln_dec(&l, pid);
    iommu_ln_s(&l, " mac=");
    for (int i = 0; i < 6; i++) { iommu_ln_hexw(&l, mac[i], 2); if (i < 5) iommu_ln_s(&l, ":"); }
    iommu_ln_s(&l, " slots="); iommu_ln_dec(&l, UDRV_NET_NSLOTS);
    iommu_ln_s(&l, " attach#"); iommu_ln_dec(&l, px->attaches);
    debugcon_writestring(iommu_ln_end(&l));
    return 0;
}

void net_udrv_detach(net_udrv_t* px, const char* why) {
    if (!px) return;
    uint64_t fl = spin_lock_irqsave(&px->lk);
    int was = px->attached;
    uint32_t pid = px->pid, inflight = px->tx_inflight;
    int died = !(why[0] == 'r' && why[1] == 'e' && why[2] == 'l');   /* not "release" */
    px->attached = 0;
    px->ctrl = NULL; px->slots = NULL; px->cookie = NULL;
    px->ntxfree = 0; px->tx_inflight = 0;
    px->dev.link_up = 0;
    if (was && died) px->deaths++;
    spin_unlock_irqrestore(&px->lk, fl);
    if (!was) return;
    iommu_line_t l; l.n = 0;
    ln_px(&l, px);
    iommu_ln_s(&l, died ? " driver died pid=" : " driver detached pid="); iommu_ln_dec(&l, pid);
    iommu_ln_s(&l, " ("); iommu_ln_s(&l, why); iommu_ln_s(&l, "), link down, ");
    iommu_ln_dec(&l, inflight); iommu_ln_s(&l, " queued frame(s) dropped; the interface stays");
    debugcon_writestring(iommu_ln_end(&l));
}

void net_udrv_kick(net_udrv_t* px) {
    if (px) net_dev_poll(&px->dev);
}

int net_udrv_waiting(void) {
    int n = 0;
    for (int i = 0; i < g_npx; i++) if (!__atomic_load_n(&g_px[i].attached, __ATOMIC_RELAXED)) n++;
    return n;
}

void net_udrv_generate(void (*emit)(void* ctx, const char* line), void* ctx) {
    for (int i = 0; i < g_npx; i++) {
        net_udrv_t* px = &g_px[i];
        uint64_t fl = spin_lock_irqsave(&px->lk);
        int att = px->attached, link = px->dev.link_up;
        uint32_t pid = px->pid, inflight = px->tx_inflight, viol = px->viol;
        uint64_t vt = px->viol_total, at = px->attaches, de = px->deaths, full = px->tx_full;
        uint64_t txf = px->tx_frames, rxf = px->rx_frames, st = px->stalls;
        spin_unlock_irqrestore(&px->lk, fl);
        iommu_line_t l; l.n = 0;
        iommu_ln_s(&l, "net "); iommu_ln_s(&l, px->dev.name);
        iommu_ln_s(&l, " dev="); iommu_ln_bdf(&l, (uint16_t)((px->pd->bus << 8) | (px->pd->slot << 3) | px->pd->func));
        iommu_ln_s(&l, att ? " attached" : " waiting");
        if (att) { iommu_ln_s(&l, " pid="); iommu_ln_dec(&l, pid); }
        iommu_ln_s(&l, " link="); iommu_ln_dec(&l, (uint64_t)link);
        iommu_ln_s(&l, " attaches="); iommu_ln_dec(&l, at);
        iommu_ln_s(&l, " deaths="); iommu_ln_dec(&l, de);
        iommu_ln_s(&l, " tx="); iommu_ln_dec(&l, txf);
        iommu_ln_s(&l, " rx="); iommu_ln_dec(&l, rxf);
        iommu_ln_s(&l, " tx_inflight="); iommu_ln_dec(&l, inflight);
        iommu_ln_s(&l, " tx_full="); iommu_ln_dec(&l, full);
        iommu_ln_s(&l, " bad_entries="); iommu_ln_dec(&l, viol);
        iommu_ln_s(&l, " bad_total="); iommu_ln_dec(&l, vt);
        iommu_ln_s(&l, " stalls="); iommu_ln_dec(&l, st);
        emit(ctx, iommu_ln_end(&l));
    }
}
