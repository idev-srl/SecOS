/*
 * usb_hid.c — [M22] USB HID boot keyboard.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Binds a HID interface with boot keyboard protocol: forces boot protocol
 * (SET_PROTOCOL 0), then keeps one 8-byte boot report outstanding on the
 * interrupt-IN endpoint. usb_hid_poll() (driven from the keyboard input wait,
 * NOT an ISR) non-blockingly checks for a completed report, translates new
 * key-down usages to ASCII and injects them into the shared keyboard buffer,
 * then re-arms. Single keyboard supported.
 */
#include "usb.h"
#include "keyboard.h"
#include "debugcon.h"
#include <stddef.h>

#define HID_SET_PROTOCOL  0x0B
#define HID_SET_IDLE      0x0A

// HID Usage (keyboard page) -> ASCII, unshifted. Index = usage id (0..0x65).
static const char hid_unshift[0x68] = {
    [0x04]='a',[0x05]='b',[0x06]='c',[0x07]='d',[0x08]='e',[0x09]='f',[0x0A]='g',
    [0x0B]='h',[0x0C]='i',[0x0D]='j',[0x0E]='k',[0x0F]='l',[0x10]='m',[0x11]='n',
    [0x12]='o',[0x13]='p',[0x14]='q',[0x15]='r',[0x16]='s',[0x17]='t',[0x18]='u',
    [0x19]='v',[0x1A]='w',[0x1B]='x',[0x1C]='y',[0x1D]='z',
    [0x1E]='1',[0x1F]='2',[0x20]='3',[0x21]='4',[0x22]='5',[0x23]='6',[0x24]='7',
    [0x25]='8',[0x26]='9',[0x27]='0',
    [0x28]='\n',[0x29]=27,[0x2A]='\b',[0x2B]='\t',[0x2C]=' ',
    [0x2D]='-',[0x2E]='=',[0x2F]='[',[0x30]=']',[0x31]='\\',
    [0x33]=';',[0x34]='\'',[0x35]='`',[0x36]=',',[0x37]='.',[0x38]='/',
};
static const char hid_shift[0x68] = {
    [0x04]='A',[0x05]='B',[0x06]='C',[0x07]='D',[0x08]='E',[0x09]='F',[0x0A]='G',
    [0x0B]='H',[0x0C]='I',[0x0D]='J',[0x0E]='K',[0x0F]='L',[0x10]='M',[0x11]='N',
    [0x12]='O',[0x13]='P',[0x14]='Q',[0x15]='R',[0x16]='S',[0x17]='T',[0x18]='U',
    [0x19]='V',[0x1A]='W',[0x1B]='X',[0x1C]='Y',[0x1D]='Z',
    [0x1E]='!',[0x1F]='@',[0x20]='#',[0x21]='$',[0x22]='%',[0x23]='^',[0x24]='&',
    [0x25]='*',[0x26]='(',[0x27]=')',
    [0x28]='\n',[0x29]=27,[0x2A]='\b',[0x2B]='\t',[0x2C]=' ',
    [0x2D]='_',[0x2E]='+',[0x2F]='{',[0x30]='}',[0x31]='|',
    [0x33]=':',[0x34]='"',[0x35]='~',[0x36]='<',[0x37]='>',[0x38]='?',
};

static usb_device_t* g_kbd;
static uint8_t g_kbd_ep;             // interrupt IN bEndpointAddress
static uint8_t g_report[8] __attribute__((aligned(64)));
static uint8_t g_last[8];            // previous report (for key-down edge detection)
static int g_armed;                  // a transfer is outstanding
static bool g_caps;

static int key_in_report(const uint8_t* rep, uint8_t code) {
    for (int i = 2; i < 8; i++) if (rep[i] == code) return 1;
    return 0;
}

static void process_report(void) {
    uint8_t mods = g_report[0];
    int shift = (mods & 0x22) != 0;   // L/R shift
    int ctrl  = (mods & 0x11) != 0;   // [M25] L/R ctrl -> control codes
    for (int i = 2; i < 8; i++) {
        uint8_t code = g_report[i];
        if (code == 0 || code == 1) continue;             // none / rollover
        if (key_in_report(g_last, code)) continue;        // still held -> not a new press
        if (code == 0x39) { g_caps = !g_caps; continue; } // Caps Lock toggle
        if (code >= 0x68) continue;
        char c = shift ? hid_shift[code] : hid_unshift[code];
        if (!shift && g_caps && c >= 'a' && c <= 'z') c -= 32;
        if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c = (char)(c & 0x1F);  // [M25]
        if (c) {
#ifdef HID_DEBUG_INJECT
            debugcon_writestring("[HID] key 0x"); debugcon_print_hex((uint64_t)(uint8_t)c); debugcon_writestring("\n");
#endif
            keyboard_inject_char(c);
        }
    }
    for (int i = 0; i < 8; i++) g_last[i] = g_report[i];
}

void usb_hid_poll(void) {
    if (!g_kbd) return;
    if (g_armed) {
        int bytes;
        if (xhci_poll_transfer(g_kbd, g_kbd_ep, 8, &bytes)) {
            g_armed = 0;
            if (bytes >= 0) process_report();
        }
    }
    if (!g_armed) {
        if (xhci_submit(g_kbd, g_kbd_ep, g_report, 8) == 0) g_armed = 1;
    }
}

void usb_hid_attach(usb_device_t* d, const usb_endpoint_t* eps, int n, uint8_t iface) {
    // Find the interrupt IN endpoint.
    uint8_t ep_in = 0;
    for (int i = 0; i < n; i++)
        if ((eps[i].addr & 0x80) && (eps[i].attr & 0x3) == 3) { ep_in = eps[i].addr; break; }
    if (!ep_in) { debugcon_writestring("[HID] no interrupt IN endpoint\n"); return; }

    // Force boot protocol; SET_IDLE 0 (errors are non-fatal on some devices).
    usb_control_out(d, 0x21, HID_SET_PROTOCOL, 0, iface);
    usb_control_out(d, 0x21, HID_SET_IDLE, 0, iface);

    g_kbd = d; g_kbd_ep = ep_in; g_armed = 0; g_caps = false;
    for (int i = 0; i < 8; i++) g_last[i] = 0;
    // Arm the first report.
    if (xhci_submit(d, ep_in, g_report, 8) == 0) g_armed = 1;
    debugcon_writestring("[HID] boot keyboard ready ep=0x");
    debugcon_print_hex(ep_in); debugcon_writestring("\n");
}
