/*
 * usb_hid.c — [M22/M40] USB HID: boot keyboard, mouse, tablet.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Up to HID_MAX HID interfaces, each keeping one report outstanding on its
 * interrupt-IN endpoint. usb_hid_poll() (driven from the keyboard input wait
 * and the evdev read/poll paths, NEVER an ISR) non-blockingly collects
 * completed reports and re-arms. Several armed endpoints share the xHCI event
 * ring; xhci.c parks each completion for its own poller.
 *
 *  - Keyboard (boot protocol, interface protocol 1): the 8-byte boot report is
 *    diffed against the previous one into Linux key codes (press + release),
 *    reported on /dev/input/eventN and fed to the console through
 *    keyboard_key_event() (keymap, modifiers, ISIG) unless a client grabbed it.
 *  - Mouse / tablet (anything else): the report descriptor is parsed for
 *    buttons, X/Y (relative -> EV_REL, absolute -> EV_ABS with the logical
 *    range, as QEMU's usb-tablet), wheel and AC Pan, then each report is
 *    decoded field by field into evdev events.
 *
 * [M50 B3] Locking: hid_lock (spinlock, LK_S12_HID, irqsave) guards the slots
 * (armed state, report buffers, the previous keyboard report) and g_nhid.
 * usb_hid_poll() can be called from syscall context on any CPU at once (the
 * console TTY wait, every evdev reader); it only TRIES hid_lock — a CPU that
 * finds another one polling just returns, the other delivers the reports — and
 * the xHCI calls it makes only try xhci_lock (xhci.h), so a poll never spins
 * behind a long USB Mass Storage transfer. Reports are delivered under
 * hid_lock (input_event -> input lock, keyboard_key_event -> kbd lock and,
 * for ISIG, proc lock), which is why hid_lock sits below all of those.
 * Attach runs under xhci_enum_lock (one enumeration at a time); its control
 * transfers use the shared descriptor buffers below without hid_lock and it
 * publishes the new slot under hid_lock.
 */
#include "usb.h"
#include "keyboard.h"
#include "input.h"
#include "debugcon.h"
#include "spinlock.h"
#include "dma.h"      // [M55] __dma: device-reachable static buffers
#include <stddef.h>

#define HID_SET_PROTOCOL  0x0B
#define HID_SET_IDLE      0x0A
#define HID_MAX           4
#define HID_REPORT_MAX    64
#define HID_FIELDS        16

enum { HID_KBD = 1, HID_PTR = 2 };

typedef struct {
    uint8_t  page;          /* usage page */
    uint16_t usage;
    uint16_t bitpos;        /* bit offset in the report (after the report ID byte) */
    uint8_t  bits;
    uint8_t  rel;           /* Input item "Relative" flag */
    uint8_t  report_id;
    int32_t  lmin, lmax;
} hid_field_t;

typedef struct {
    usb_device_t* d;
    uint8_t  ep;            /* interrupt IN bEndpointAddress */
    uint16_t mps;
    int      kind;
    int      armed;
    uint8_t  last[8];       /* keyboard: previous boot report */
    int      nf;
    int      has_ids;       /* reports start with a report ID byte */
    hid_field_t f[HID_FIELDS];
    input_dev_t in;
} hid_slot_t;

static hid_slot_t g_hid[HID_MAX];
static int g_nhid;
static spinlock_t hid_lock = SPINLOCK_INIT_CLASS(LK_S12_HID, "usb-hid");
static uint8_t g_rep[HID_MAX][HID_REPORT_MAX] __dma __attribute__((aligned(64)));
static uint8_t g_rdesc[512] __dma __attribute__((aligned(64)));
static uint8_t g_strbuf[128] __dma __attribute__((aligned(64)));

/* HID keyboard usage (page 7) -> Linux key code (drivers/hid/hid-input.c). */
static const uint8_t hid_kc[0x68] = {
      0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
     50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
      4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
     27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
     65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,
    105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
     72, 73, 82, 83, 86,127,116,117,
};
/* Modifier byte bits -> key codes: LCtrl LShift LAlt LMeta RCtrl RShift RAlt RMeta. */
static const uint8_t hid_mod_kc[8] = { 29, 42, 56, 125, 97, 54, 100, 126 };

static void scpy(char* d, const char* s, int max) {
    int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0;
}

/* ---- keyboard ------------------------------------------------------------------- */
static volatile uint32_t g_hid_keydowns;     /* [M50 B3] key presses seen (tests) */
uint32_t usb_hid_keydowns(void) { return g_hid_keydowns; }

static void kbd_key(hid_slot_t* h, int kc, int v) {
    if (v) g_hid_keydowns++;                     /* under hid_lock */
    input_event(&h->in, EV_KEY, (uint16_t)kc, v);
    if (!input_grabbed(&h->in)) keyboard_key_event(kc, v);
}

static int in_report(const uint8_t* rep, uint8_t code) {
    for (int i = 2; i < 8; i++) if (rep[i] == code) return 1;
    return 0;
}

static void kbd_report(hid_slot_t* h, const uint8_t* rep, int len) {
    if (len < 8) return;
    for (int i = 2; i < 8; i++) if (rep[i] == 1) return;   /* ErrorRollOver: ignore */
    uint8_t chg = rep[0] ^ h->last[0];
    for (int b = 0; b < 8; b++)
        if (chg & (1u << b)) kbd_key(h, hid_mod_kc[b], (rep[0] >> b) & 1);
    for (int i = 2; i < 8; i++) {                          /* releases */
        uint8_t c = h->last[i];
        if (c >= 4 && c < 0x68 && hid_kc[c] && !in_report(rep, c)) kbd_key(h, hid_kc[c], 0);
    }
    for (int i = 2; i < 8; i++) {                          /* presses */
        uint8_t c = rep[i];
        if (c >= 4 && c < 0x68 && hid_kc[c] && !in_report(h->last, c)) {
#ifdef HID_DEBUG_INJECT
            debugcon_writestring("[HID] key 0x"); debugcon_print_hex(hid_kc[c]); debugcon_writestring("\n");
#endif
            kbd_key(h, hid_kc[c], 1);
        }
    }
    input_sync(&h->in);
    for (int i = 0; i < 8; i++) h->last[i] = rep[i];
}

/* ---- report descriptor parser ------------------------------------------------------ */
static int32_t sext(uint32_t v, int bytes) {
    if (bytes == 1) return (int8_t)v;
    if (bytes == 2) return (int16_t)v;
    return (int32_t)v;
}

static int field_wanted(uint8_t page, uint16_t usage) {
    if (page == 0x09) return usage >= 1 && usage <= 5;                 /* buttons 1..5 */
    if (page == 0x01) return usage == 0x30 || usage == 0x31 || usage == 0x38; /* X Y Wheel */
    if (page == 0x0C) return usage == 0x238;                           /* AC Pan */
    return 0;
}

static void parse_rdesc(hid_slot_t* h, const uint8_t* p, int len) {
    uint8_t page = 0, rsize = 0, rid = 0;
    uint32_t rcount = 0;
    int32_t lmin = 0, lmax = 0;
    uint32_t usages[16]; int nu = 0;
    uint32_t umin = 0, umax = 0; int have_range = 0;
    uint16_t bitpos[8] = { 0 }; uint8_t ids[8] = { 0 }; int nids = 1;   /* per report ID */
    int cur = 0;
    h->nf = 0; h->has_ids = 0;
    int i = 0;
    while (i < len) {
        uint8_t b = p[i++];
        if (b == 0xFE) {                                   /* long item: skip */
            if (i + 1 >= len) break;
            i += 2 + p[i];
            continue;
        }
        int sz = b & 3; if (sz == 3) sz = 4;
        int type = (b >> 2) & 3, tag = b >> 4;
        if (i + sz > len) break;
        uint32_t v = 0;
        for (int k = 0; k < sz; k++) v |= (uint32_t)p[i + k] << (8 * k);
        i += sz;
        if (type == 1) {                                   /* global */
            switch (tag) {
            case 0x0: page = (uint8_t)v; break;
            case 0x1: lmin = sext(v, sz); break;
            case 0x2: lmax = (lmin < 0) ? sext(v, sz) : (int32_t)v; break;
            case 0x7: rsize = (uint8_t)v; break;
            case 0x8: {                                    /* report ID */
                rid = (uint8_t)v; h->has_ids = 1;
                int k = 0; for (; k < nids; k++) if (ids[k] == rid) break;
                if (k == nids && nids < 8) { ids[nids] = rid; bitpos[nids] = 0; nids++; }
                cur = (k < 8) ? k : 0;
                break;
            }
            case 0x9: rcount = v; break;
            default: break;
            }
        } else if (type == 2) {                            /* local */
            if (tag == 0x0 && nu < 16) usages[nu++] = v;
            else if (tag == 0x1) { umin = v; have_range = 1; }
            else if (tag == 0x2) { umax = v; have_range = 1; }
        } else if (type == 0) {                            /* main */
            if (tag == 0x8) {                              /* Input */
                for (uint32_t n = 0; n < rcount; n++) {
                    uint32_t u = 0;
                    if (nu) u = usages[n < (uint32_t)nu ? n : (uint32_t)nu - 1];
                    else if (have_range && umin + n <= umax) u = umin + n;
                    uint8_t pg = page;
                    if (u > 0xFFFF) { pg = (uint8_t)(u >> 16); u &= 0xFFFF; }   /* extended usage */
                    if (!(v & 1) && field_wanted(pg, (uint16_t)u) && h->nf < HID_FIELDS) {
                        hid_field_t* f = &h->f[h->nf++];
                        f->page = pg; f->usage = (uint16_t)u; f->bitpos = bitpos[cur];
                        f->bits = rsize; f->rel = (v & 4) != 0; f->report_id = rid;
                        f->lmin = lmin; f->lmax = lmax;
                    }
                    bitpos[cur] = (uint16_t)(bitpos[cur] + rsize);
                }
            }
            nu = 0; have_range = 0; umin = umax = 0;       /* locals end at a main item */
        }
    }
}

static int32_t get_bits(const uint8_t* r, int rlen, int pos, int bits, int sign) {
    uint32_t v = 0;
    for (int k = 0; k < bits && k < 32; k++) {
        int bp = pos + k;
        if (bp / 8 >= rlen) break;
        if ((r[bp / 8] >> (bp % 8)) & 1) v |= (1u << k);
    }
    if (sign && bits > 0 && bits < 32 && (v & (1u << (bits - 1)))) v |= ~((1u << bits) - 1);
    return (int32_t)v;
}

static void ptr_report(hid_slot_t* h, const uint8_t* rep, int len) {
    uint8_t rid = 0;
    if (h->has_ids) { if (len < 1) return; rid = rep[0]; rep++; len--; }
    for (int i = 0; i < h->nf; i++) {
        hid_field_t* f = &h->f[i];
        if (h->has_ids && f->report_id != rid) continue;
        int32_t v = get_bits(rep, len, f->bitpos, f->bits, f->lmin < 0);
        if (f->page == 0x09) {
            input_event(&h->in, EV_KEY, (uint16_t)(BTN_MOUSE + f->usage - 1), v ? 1 : 0);
        } else if (f->page == 0x01 && (f->usage == 0x30 || f->usage == 0x31)) {
            uint16_t axis = f->usage == 0x30 ? 0 : 1;
            if (f->rel) input_event(&h->in, EV_REL, axis == 0 ? REL_X : REL_Y, v);
            else        input_event(&h->in, EV_ABS, axis == 0 ? ABS_X : ABS_Y, v);
        } else if (f->page == 0x01 && f->usage == 0x38) {
            input_event(&h->in, EV_REL, REL_WHEEL, v);
        } else if (f->page == 0x0C && f->usage == 0x238) {
            input_event(&h->in, EV_REL, REL_HWHEEL, v);
        }
    }
    input_sync(&h->in);
}

/* ---- polling ------------------------------------------------------------------------- */
// Take hid_lock only if it is free; interrupts are off iff it was taken.
static int hid_trylock_irqsave(uint64_t* fl) {
    uint64_t f;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    if (spin_trylock(&hid_lock)) { *fl = f; return 1; }
    __asm__ volatile ("push %0; popfq" :: "r"(f) : "memory", "cc");
    return 0;
}

void usb_hid_poll(void) {
    if (!__atomic_load_n(&g_nhid, __ATOMIC_ACQUIRE)) return;   /* no HID: no lock */
    uint64_t fl;
    if (!hid_trylock_irqsave(&fl)) return;       /* another CPU is polling */
    for (int s = 0; s < g_nhid; s++) {
        hid_slot_t* h = &g_hid[s];
        if (!h->d) continue;
        if (h->armed) {
            int bytes;
            if (xhci_poll_transfer(h->d, h->ep, h->mps, &bytes)) {
                h->armed = 0;
                if (bytes > 0) {
                    if (h->kind == HID_KBD) kbd_report(h, g_rep[s], bytes);
                    else ptr_report(h, g_rep[s], bytes);
                }
            }
        }
        if (!h->armed && xhci_submit(h->d, h->ep, g_rep[s], h->mps) == 0) h->armed = 1;
    }
    spin_unlock_irqrestore(&hid_lock, fl);
}

/* Product string (UTF-16LE string descriptor -> ASCII), or a generic name. */
static void dev_name(usb_device_t* d, char* out, int max, const char* fallback) {
    if (d->iproduct) {
        uint8_t s[8];
        usb_setup(s, 0x80, 6, (uint16_t)((3 << 8) | d->iproduct), 0x0409, sizeof(g_strbuf));
        if (xhci_control(d, s, g_strbuf, sizeof(g_strbuf)) >= 0 && g_strbuf[1] == 3 && g_strbuf[0] > 2) {
            int n = (g_strbuf[0] - 2) / 2, k = 0;
            for (int i = 0; i < n && k < max - 1; i++) {
                uint8_t c = g_strbuf[2 + 2 * i];
                out[k++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
            }
            out[k] = 0;
            if (k) return;
        }
    }
    scpy(out, fallback, max);
}

void usb_hid_attach(usb_device_t* d, const usb_endpoint_t* eps, int n, uint8_t iface) {
    /* g_nhid only grows, here, under xhci_enum_lock: slot g_nhid is ours until
     * it is published below. */
    if (g_nhid >= HID_MAX) { debugcon_writestring("[HID] too many HID devices\n"); return; }
    uint8_t ep_in = 0; uint16_t mps = 8;
    for (int i = 0; i < n; i++)
        if ((eps[i].addr & 0x80) && (eps[i].attr & 0x3) == 3) { ep_in = eps[i].addr; mps = eps[i].max_packet; break; }
    if (!ep_in) { debugcon_writestring("[HID] no interrupt IN endpoint\n"); return; }
    if (mps == 0 || mps > HID_REPORT_MAX) mps = HID_REPORT_MAX;

    int s = g_nhid;
    hid_slot_t* h = &g_hid[s];
    h->kind = (d->iface_proto == 1) ? HID_KBD : HID_PTR;
    input_dev_t* in = &h->in;
    in->id.bustype = BUS_USB; in->id.vendor = d->vendor; in->id.product = d->product; in->id.version = 1;
    scpy(in->phys, "usb-xhci/input0", sizeof(in->phys));

    if (h->kind == HID_KBD) {
        // Force boot protocol; SET_IDLE 0 (errors are non-fatal on some devices).
        usb_control_out(d, 0x21, HID_SET_PROTOCOL, 0, iface);
        usb_control_out(d, 0x21, HID_SET_IDLE, 0, iface);
        mps = 8;
        dev_name(d, in->name, sizeof(in->name), "USB HID Keyboard");
        input_set_capability(in, EV_REP, 0);
        for (int k = 1; k < 128; k++) input_set_capability(in, EV_KEY, (uint16_t)k);
    } else {
        usb_control_out(d, 0x21, HID_SET_IDLE, 0, iface);
        int rl = d->hid_rdesc_len;
        if (rl <= 0 || rl > (int)sizeof(g_rdesc)) rl = sizeof(g_rdesc);
        /* GET_DESCRIPTOR(Report) is an interface request (bmRequestType 0x81). */
        if (usb_control_in(d, 0x81, 6, 0x2200, iface, g_rdesc, rl) < 0) {
            debugcon_writestring("[HID] report descriptor read failed\n"); return;
        }
        parse_rdesc(h, g_rdesc, rl);
        int abs = 0;
        for (int i = 0; i < h->nf; i++) {
            hid_field_t* f = &h->f[i];
            if (f->page == 0x09) input_set_capability(in, EV_KEY, (uint16_t)(BTN_MOUSE + f->usage - 1));
            else if (f->page == 0x01 && (f->usage == 0x30 || f->usage == 0x31)) {
                uint16_t axis = f->usage == 0x30 ? 0 : 1;
                if (f->rel) input_set_capability(in, EV_REL, axis == 0 ? REL_X : REL_Y);
                else { input_set_abs_params(in, axis == 0 ? ABS_X : ABS_Y, f->lmin, f->lmax); abs = 1; }
            } else if (f->page == 0x01 && f->usage == 0x38) input_set_capability(in, EV_REL, REL_WHEEL);
            else if (f->page == 0x0C) input_set_capability(in, EV_REL, REL_HWHEEL);
        }
        if (h->nf == 0) { debugcon_writestring("[HID] no usable fields in the report\n"); return; }
        in->propbit = abs ? (1u << INPUT_PROP_DIRECT) : (1u << INPUT_PROP_POINTER);
        dev_name(d, in->name, sizeof(in->name), abs ? "USB HID Tablet" : "USB HID Mouse");
    }
    in->poll = usb_hid_poll;
    uint64_t fl = spin_lock_irqsave(&hid_lock);
    h->d = d; h->ep = ep_in; h->mps = mps; h->armed = 0;
    for (int i = 0; i < 8; i++) h->last[i] = 0;
    input_register(in);
    if (xhci_submit(d, ep_in, g_rep[s], mps) == 0) h->armed = 1;   /* else: next poll */
    __atomic_store_n(&g_nhid, g_nhid + 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&hid_lock, fl);
    debugcon_writestring(h->kind == HID_KBD ? "[HID] boot keyboard ready ep=0x" : "[HID] pointer ready ep=0x");
    debugcon_print_hex(ep_in);
    debugcon_writestring(" fields="); debugcon_print_hex((uint64_t)h->nf);
    debugcon_writestring(" name="); debugcon_writestring(in->name);
    debugcon_writestring("\n");
}
