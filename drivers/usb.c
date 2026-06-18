/*
 * usb.c — [M22] USB core: enumeration + class dispatch.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Drives standard device enumeration over the xHCI control primitives: read the
 * device descriptor (fixing EP0 max-packet on full/low speed), read the
 * configuration descriptor, parse its interface + endpoints, SET_CONFIGURATION,
 * install the data endpoints (Configure Endpoint), and bind the matching class
 * driver (HID boot keyboard or Bulk-Only Mass Storage).
 */
#include "usb.h"
#include "debugcon.h"
#include <stddef.h>

// Shared control-transfer buffer (one operation at a time, like the rest of the
// polled stack). Static => physically addressable via kvirt_to_phys.
static uint8_t g_ctlbuf[512] __attribute__((aligned(64)));

void usb_setup(uint8_t* o, uint8_t t, uint8_t r, uint16_t v, uint16_t i, uint16_t l) {
    o[0] = t; o[1] = r;
    o[2] = (uint8_t)v; o[3] = (uint8_t)(v >> 8);
    o[4] = (uint8_t)i; o[5] = (uint8_t)(i >> 8);
    o[6] = (uint8_t)l; o[7] = (uint8_t)(l >> 8);
}

int usb_get_descriptor(usb_device_t* d, uint8_t type, uint8_t index, void* buf, int len) {
    uint8_t s[8];
    usb_setup(s, 0x80, 6, (uint16_t)((type << 8) | index), 0, (uint16_t)len);
    return xhci_control(d, s, buf, len);
}

int usb_set_configuration(usb_device_t* d, uint8_t cfg) {
    uint8_t s[8];
    usb_setup(s, 0x00, 9, cfg, 0, 0);
    return xhci_control(d, s, NULL, 0);
}

int usb_control_out(usb_device_t* d, uint8_t t, uint8_t r, uint16_t v, uint16_t i) {
    uint8_t s[8];
    usb_setup(s, t, r, v, i, 0);
    return xhci_control(d, s, NULL, 0);
}

void usb_attach(usb_device_t* d) {
    // Device descriptor: first 8 bytes (for bMaxPacketSize0), fix EP0 if needed.
    if (usb_get_descriptor(d, USB_DT_DEVICE, 0, g_ctlbuf, 8) < 0) {
        debugcon_writestring("[USB] get device desc(8) failed\n"); return;
    }
    if (d->speed < 3) {   // LS/FS: bMaxPacketSize0 is a byte count, may differ from 8
        uint8_t mps0 = g_ctlbuf[7];
        if (mps0 != 8 && (mps0 == 16 || mps0 == 32 || mps0 == 64))
            xhci_set_ep0_mps(d, mps0);
    }
    // Full device descriptor (18 bytes).
    if (usb_get_descriptor(d, USB_DT_DEVICE, 0, g_ctlbuf, 18) < 0) {
        debugcon_writestring("[USB] get device desc(18) failed\n"); return;
    }
    d->dev_class = g_ctlbuf[4]; d->dev_subclass = g_ctlbuf[5]; d->dev_proto = g_ctlbuf[6];
    d->vendor = (uint16_t)(g_ctlbuf[8] | (g_ctlbuf[9] << 8));
    d->product = (uint16_t)(g_ctlbuf[10] | (g_ctlbuf[11] << 8));
    debugcon_writestring("[USB] dev vid=0x"); debugcon_print_hex(d->vendor);
    debugcon_writestring(" pid=0x"); debugcon_print_hex(d->product);
    debugcon_writestring(" class=0x"); debugcon_print_hex(d->dev_class); debugcon_writestring("\n");

    // Configuration descriptor: 9-byte header for wTotalLength, then the whole.
    if (usb_get_descriptor(d, USB_DT_CONFIG, 0, g_ctlbuf, 9) < 0) {
        debugcon_writestring("[USB] get config(9) failed\n"); return;
    }
    int total = g_ctlbuf[2] | (g_ctlbuf[3] << 8);
    if (total > (int)sizeof(g_ctlbuf)) total = sizeof(g_ctlbuf);
    if (usb_get_descriptor(d, USB_DT_CONFIG, 0, g_ctlbuf, total) < 0) {
        debugcon_writestring("[USB] get config(full) failed\n"); return;
    }
    uint8_t cfg_val = g_ctlbuf[5];

    // Walk the descriptor list: find the first interface and its endpoints.
    uint8_t iface_num = 0, iface_class = 0, iface_sub = 0, iface_proto = 0;
    usb_endpoint_t eps[XHCI_EPR_PER_DEV]; int nep = 0; int have_iface = 0;
    int off = 0;
    while (off + 2 <= total) {
        uint8_t blen = g_ctlbuf[off];
        uint8_t btype = g_ctlbuf[off + 1];
        if (blen == 0) break;
        if (btype == USB_DT_INTERFACE && !have_iface) {
            iface_num = g_ctlbuf[off + 2];
            iface_class = g_ctlbuf[off + 5];
            iface_sub = g_ctlbuf[off + 6];
            iface_proto = g_ctlbuf[off + 7];
            have_iface = 1;
        } else if (btype == USB_DT_ENDPOINT && have_iface && nep < XHCI_EPR_PER_DEV) {
            eps[nep].addr = g_ctlbuf[off + 2];
            eps[nep].attr = g_ctlbuf[off + 3];
            eps[nep].max_packet = (uint16_t)(g_ctlbuf[off + 4] | (g_ctlbuf[off + 5] << 8));
            eps[nep].interval = g_ctlbuf[off + 6];
            nep++;
        }
        off += blen;
    }
    if (!have_iface) { debugcon_writestring("[USB] no interface\n"); return; }

    if (usb_set_configuration(d, cfg_val) < 0) {
        debugcon_writestring("[USB] set config failed\n"); return;
    }
    d->config_val = cfg_val;
    if (nep > 0 && xhci_configure_endpoints(d, eps, nep) != 0) {
        debugcon_writestring("[USB] configure endpoints failed\n"); return;
    }

    debugcon_writestring("[USB] iface class=0x"); debugcon_print_hex(iface_class);
    debugcon_writestring(" sub=0x"); debugcon_print_hex(iface_sub);
    debugcon_writestring(" proto=0x"); debugcon_print_hex(iface_proto);
    debugcon_writestring(" neps="); debugcon_print_hex((uint64_t)nep); debugcon_writestring("\n");

    if (d->dev_class == USB_CLASS_HUB || iface_class == USB_CLASS_HUB) {
        // USB hub (M22 hub support — NOT YET TESTED on real hardware, no hub in
        // QEMU CI). Power + reset downstream ports and enumerate behind it.
        d->class_id = USB_CLASS_HUB;
        usb_hub_attach(d, eps, nep, iface_num);
    } else if (iface_class == USB_CLASS_HID && iface_proto == 1) {   // boot keyboard
        d->class_id = USB_CLASS_HID;
        usb_hid_attach(d, eps, nep, iface_num);
    } else if (iface_class == USB_CLASS_MSC && iface_sub == 0x06 /* SCSI */) {
        d->class_id = USB_CLASS_MSC;
        usb_msc_attach(d, eps, nep, iface_num);
    } else {
        debugcon_writestring("[USB] no class driver for this device\n");
    }
}

int usb_init(void) {
    if (xhci_init() != 0) return -1;
    xhci_enumerate();
    return 0;
}
