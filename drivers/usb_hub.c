/*
 * usb_hub.c — [M22] USB hub support.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * USB hub support — implemented but NOT YET TESTED on real hardware (there is no
 * hub device in the QEMU CI). This is ADDITIVE: it only runs when a device whose
 * class (device or interface) is 0x09 (Hub) is enumerated, which never happens on
 * the existing root-port-only QEMU test path, so the verified root-port behaviour
 * is unaffected.
 *
 * Flow when bound to a hub interface:
 *   1. GET_DESCRIPTOR(hub, type 0x29) -> bNbrPorts.
 *   2. SET_FEATURE(PORT_POWER) on every downstream port, then a settle delay.
 *   3. For each port: GET_PORT_STATUS, and on a connect, SET_FEATURE(PORT_RESET),
 *      poll for reset-complete, clear the change bits, derive the device speed,
 *      and address the downstream device through the xHCI driver (which calls
 *      usb_attach() on it, so nested hubs recurse naturally).
 *
 * Everything is polled and single-operation-at-a-time, matching the rest of the
 * USB stack. Hub class requests use the class request type bytes (0x23 host->dev
 * port-targeted SET/CLEAR_FEATURE, 0xA3 dev->host GET_STATUS).
 */
#include "usb.h"
#include "debugcon.h"
#include <stddef.h>

// Hub descriptor type (class-specific).
#define HUB_DT_HUB          0x29

// Hub/port feature selectors (USB 2.0 §11.24.2).
#define PORT_CONNECTION     0
#define PORT_ENABLE         1
#define PORT_RESET          4
#define PORT_POWER          8
#define C_PORT_CONNECTION   16
#define C_PORT_RESET        20
#define C_PORT_ENABLE       17

// wPortStatus bits (GET_PORT_STATUS first 16 bits).
#define PS_CONNECTION       (1u << 0)
#define PS_ENABLE           (1u << 1)
#define PS_RESET            (1u << 4)
#define PS_LOW_SPEED        (1u << 9)
#define PS_HIGH_SPEED       (1u << 10)

// wPortChange bits (GET_PORT_STATUS upper 16 bits).
#define PC_CONNECTION       (1u << 0)
#define PC_RESET            (1u << 4)
#define PC_ENABLE           (1u << 1)

// Class request type bytes.
#define REQ_SET_FEATURE     3
#define REQ_CLEAR_FEATURE   1
#define REQ_GET_STATUS      0
#define RT_PORT_OUT         0x23   // host->device, class, other (port)
#define RT_PORT_IN          0xA3   // device->host, class, other (port)

// xHCI speed codes (PORTSC PortSpeed field).
#define SPEED_FS            1
#define SPEED_LS            2
#define SPEED_HS            3
#define SPEED_SS            4

// Dedicated buffers so child enumeration (which reuses the USB core's shared
// control buffer) does not clobber the hub's own descriptor / port-status reads.
static uint8_t g_hubdesc[16] __attribute__((aligned(64)));
static uint8_t g_status[4]    __attribute__((aligned(64)));

static void hub_delay(uint64_t n) {
    for (volatile uint64_t i = 0; i < n; i++) { }
}

// SET_FEATURE / CLEAR_FEATURE on a downstream port (class request).
static int hub_port_set_feature(usb_device_t* d, uint16_t feat, uint16_t port) {
    return usb_control_out(d, RT_PORT_OUT, REQ_SET_FEATURE, feat, port);
}
static int hub_port_clear_feature(usb_device_t* d, uint16_t feat, uint16_t port) {
    return usb_control_out(d, RT_PORT_OUT, REQ_CLEAR_FEATURE, feat, port);
}

// GET_PORT_STATUS: 4 bytes (wPortStatus | wPortChange). Returns 0 and fills
// *status / *change on success, -1 otherwise.
static int hub_port_status(usb_device_t* d, uint16_t port,
                           uint16_t* status, uint16_t* change) {
    uint8_t s[8];
    usb_setup(s, RT_PORT_IN, REQ_GET_STATUS, 0, port, 4);
    if (xhci_control(d, s, g_status, 4) < 0) return -1;
    *status = (uint16_t)(g_status[0] | (g_status[1] << 8));
    *change = (uint16_t)(g_status[2] | (g_status[3] << 8));
    return 0;
}

// Map a downstream port's USB2 speed bits to an xHCI speed code.
static int hub_port_speed(uint16_t status) {
    if (status & PS_LOW_SPEED)  return SPEED_LS;
    if (status & PS_HIGH_SPEED) return SPEED_HS;
    return SPEED_FS;
}

void usb_hub_attach(usb_device_t* d, const usb_endpoint_t* eps, int n, uint8_t iface) {
    (void)eps; (void)n; (void)iface;

    // Read the hub descriptor (class request: GET_DESCRIPTOR type 0x29). The
    // bmRequestType for a hub-class descriptor is 0xA0 (device->host, class,
    // device); wValue = (0x29 << 8).
    uint8_t s[8];
    usb_setup(s, 0xA0, 6 /* GET_DESCRIPTOR */, (uint16_t)(HUB_DT_HUB << 8), 0,
              (uint16_t)sizeof(g_hubdesc));
    if (xhci_control(d, s, g_hubdesc, (int)sizeof(g_hubdesc)) < 0) {
        debugcon_writestring("[HUB] get hub descriptor failed\n");
        return;
    }
    uint8_t nports = g_hubdesc[2];   // bNbrPorts
    if (nports == 0 || nports > 15) {
        debugcon_writestring("[HUB] bad port count\n");
        return;
    }
    debugcon_writestring("[HUB] hub at slot 0x"); debugcon_print_hex((uint64_t)d->slot_id);
    debugcon_writestring(" ports="); debugcon_print_hex((uint64_t)nports);
    debugcon_writestring(" tier="); debugcon_print_hex((uint64_t)d->tier);
    debugcon_writestring("\n");

    // Power every downstream port, then let power-on settle (PwrOn2PwrGood is in
    // g_hubdesc[5] in 2 ms units; use a generous fixed spin instead of a clock).
    for (uint16_t p = 1; p <= nports; p++) {
        hub_port_set_feature(d, PORT_POWER, p);
    }
    hub_delay(4000000ull);

    // Walk each downstream port: reset connected devices and enumerate them.
    for (uint16_t p = 1; p <= nports; p++) {
        uint16_t status = 0, change = 0;
        if (hub_port_status(d, p, &status, &change) < 0) continue;
        // Clear a pending connection-change before deciding.
        if (change & PC_CONNECTION) hub_port_clear_feature(d, C_PORT_CONNECTION, p);
        if (!(status & PS_CONNECTION)) continue;   // nothing attached

        debugcon_writestring("[HUB] port "); debugcon_print_hex((uint64_t)p);
        debugcon_writestring(" connected, resetting\n");

        // Drive a port reset and poll for completion (reset-change or enabled).
        if (hub_port_set_feature(d, PORT_RESET, p) < 0) continue;
        int reset_ok = 0;
        for (int tries = 0; tries < 50; tries++) {
            hub_delay(400000ull);
            if (hub_port_status(d, p, &status, &change) < 0) break;
            if ((change & PC_RESET) || ((status & PS_ENABLE) && !(status & PS_RESET))) {
                reset_ok = 1;
                break;
            }
        }
        // Clear reset/enable/connection change bits regardless.
        hub_port_clear_feature(d, C_PORT_RESET, p);
        hub_port_clear_feature(d, C_PORT_ENABLE, p);
        hub_port_clear_feature(d, C_PORT_CONNECTION, p);

        if (!reset_ok || !(status & PS_ENABLE)) {
            debugcon_writestring("[HUB] port reset failed\n");
            continue;
        }

        int speed = hub_port_speed(status);
        uint32_t route = xhci_route_append(d->route_string, d->tier, (int)p);
        debugcon_writestring("[HUB] enumerating downstream port "); debugcon_print_hex((uint64_t)p);
        debugcon_writestring(" speed "); debugcon_print_hex((uint64_t)speed);
        debugcon_writestring(" route 0x"); debugcon_print_hex((uint64_t)route);
        debugcon_writestring("\n");

        // Address the downstream device through xHCI. For a LS/FS device behind a
        // HS hub the parent (this hub) provides the Transaction Translator info;
        // for a device behind a non-TT hub the controller derives it from the
        // chain. xhci_attach_hub_device() calls usb_attach() so nested hubs
        // recurse, bounded by MAX_DEV slots and the route-string tier cap.
        xhci_attach_hub_device(d->slot_id, (int)p, d->root_port, route,
                               d->tier + 1, speed);
    }
}
