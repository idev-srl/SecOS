/*
 * SecOS Kernel - PS/2 (i8042 AUX) mouse
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Enables the i8042 auxiliary port, resets the mouse to defaults, tries the
 * IntelliMouse "magic knock" (sample rates 200/100/80 -> device ID 3 = 4-byte
 * packets with a wheel) and turns on streaming. Packets arrive on IRQ12
 * (routed through the IOAPIC like the keyboard's IRQ1, or the slave 8259 in PIC
 * mode) and are reported as EV_REL REL_X/REL_Y/REL_WHEEL + BTN_LEFT/RIGHT/
 * MIDDLE on /dev/input/eventN, with the Linux sign conventions (Y grows
 * downward, wheel > 0 = away from the user).
 */
#include "ps2mouse.h"
#include "input.h"
#include "debugcon.h"
#include <stdbool.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(port));
}

static input_dev_t g_mouse;
static int g_ready;
static int g_wheel;               /* IntelliMouse: 4-byte packets */
static uint8_t g_pkt[4];
static int g_idx;
static uint8_t g_buttons;

static int wait_write(void) {
    for (int i = 0; i < 100000; i++) if (!(inb(PS2_STATUS) & 0x02)) return 0;
    return -1;
}
static int wait_read(void) {
    for (int i = 0; i < 1000000; i++) if (inb(PS2_STATUS) & 0x01) return 0;
    return -1;
}
/* Read one byte, polled (init only: IRQ12 is still masked). */
static int read_byte(void) {
    if (wait_read()) return -1;
    return inb(PS2_DATA);
}
/* Send a byte to the mouse (0xD4 prefix) and wait for its ACK (0xFA). Keyboard
 * bytes that sneak in are ignored (the AUX status bit tells them apart). */
static int mouse_cmd(uint8_t b) {
    if (wait_write()) return -1;
    outb(PS2_CMD, 0xD4);
    if (wait_write()) return -1;
    outb(PS2_DATA, b);
    for (int tries = 0; tries < 8; tries++) {
        if (wait_read()) return -1;
        uint8_t st = inb(PS2_STATUS);
        uint8_t r = inb(PS2_DATA);
        if (!(st & 0x20)) continue;          /* a keyboard byte: not ours */
        if (r == 0xFA) return 0;
        if (r == 0xFE || r == 0xFC) return -1;
    }
    return -1;
}
static int mouse_read_aux(void) {
    for (int tries = 0; tries < 8; tries++) {
        if (wait_read()) return -1;
        uint8_t st = inb(PS2_STATUS);
        uint8_t r = inb(PS2_DATA);
        if (st & 0x20) return r;
    }
    return -1;
}

int ps2mouse_init(void) {
    uint64_t rfl;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(rfl) :: "memory");
    /* Enable the AUX port; set "IRQ12 enable" and clear "AUX clock disable" in
     * the controller configuration byte. */
    if (wait_write()) goto fail;
    outb(PS2_CMD, 0xA8);
    if (wait_write()) goto fail;
    outb(PS2_CMD, 0x20);
    int cfg = read_byte();
    if (cfg < 0) goto fail;
    cfg = (cfg | 0x02) & ~0x20;
    if (wait_write()) goto fail;
    outb(PS2_CMD, 0x60);
    if (wait_write()) goto fail;
    outb(PS2_DATA, (uint8_t)cfg);

    if (mouse_cmd(0xF6)) goto fail;          /* set defaults */
    /* IntelliMouse knock: sample rate 200, 100, 80, then read the ID. */
    g_wheel = 0;
    if (!mouse_cmd(0xF3) && !mouse_cmd(200) && !mouse_cmd(0xF3) && !mouse_cmd(100) &&
        !mouse_cmd(0xF3) && !mouse_cmd(80) && !mouse_cmd(0xF2)) {
        int id = mouse_read_aux();
        g_wheel = (id == 3 || id == 4);
    }
    mouse_cmd(0xF3); mouse_cmd(100);         /* back to 100 samples/s */
    if (mouse_cmd(0xF4)) goto fail;          /* enable data reporting */
    g_idx = 0; g_buttons = 0;

    input_dev_t* d = &g_mouse;
    const char* nm = g_wheel ? "ImExPS/2 Generic Explorer Mouse" : "PS/2 Generic Mouse";
    int i = 0;
    for (; nm[i]; i++) d->name[i] = nm[i];
    d->name[i] = 0;
    const char* ph = "isa0060/serio1/input0";
    for (i = 0; ph[i]; i++) d->phys[i] = ph[i];
    d->phys[i] = 0;
    d->id.bustype = BUS_I8042; d->id.vendor = 0x0002; d->id.product = g_wheel ? 0x0006 : 0x0001;
    d->propbit = 1u << INPUT_PROP_POINTER;
    input_set_capability(d, EV_KEY, BTN_LEFT);
    input_set_capability(d, EV_KEY, BTN_RIGHT);
    input_set_capability(d, EV_KEY, BTN_MIDDLE);
    input_set_capability(d, EV_REL, REL_X);
    input_set_capability(d, EV_REL, REL_Y);
    if (g_wheel) input_set_capability(d, EV_REL, REL_WHEEL);
    if (input_register(d) >= 0) g_ready = 1;
    __asm__ volatile ("push %0; popfq" :: "r"(rfl) : "memory", "cc");
    debugcon_writestring(g_wheel ? "[PS2M] mouse ready (wheel)\n" : "[PS2M] mouse ready\n");
    return 0;
fail:
    __asm__ volatile ("push %0; popfq" :: "r"(rfl) : "memory", "cc");
    debugcon_writestring("[PS2M] no PS/2 mouse\n");
    return -1;
}

static void report_packet(void) {
    uint8_t b0 = g_pkt[0];
    int dx = g_pkt[1], dy = g_pkt[2];
    if (b0 & 0x10) dx -= 256;                /* 9-bit two's complement */
    if (b0 & 0x20) dy -= 256;
    if (b0 & 0xC0) { dx = 0; dy = 0; }       /* overflow: drop the motion */
    uint8_t btn = b0 & 0x07;
    uint8_t chg = btn ^ g_buttons;
    g_buttons = btn;
    if (chg & 1) input_event(&g_mouse, EV_KEY, BTN_LEFT, btn & 1);
    if (chg & 2) input_event(&g_mouse, EV_KEY, BTN_RIGHT, (btn >> 1) & 1);
    if (chg & 4) input_event(&g_mouse, EV_KEY, BTN_MIDDLE, (btn >> 2) & 1);
    input_event(&g_mouse, EV_REL, REL_X, dx);
    input_event(&g_mouse, EV_REL, REL_Y, -dy);   /* PS/2: up is positive */
    if (g_wheel) {
        int dz = g_pkt[3] & 0x0F;
        if (dz & 0x08) dz -= 16;
        input_event(&g_mouse, EV_REL, REL_WHEEL, -dz);
    }
    input_sync(&g_mouse);
}

void ps2mouse_byte(uint8_t b) {
    if (!g_ready) return;
    if (g_idx == 0 && !(b & 0x08)) return;   /* resync: byte 0 always has bit 3 set */
    g_pkt[g_idx++] = b;
    if (g_idx >= (g_wheel ? 4 : 3)) { g_idx = 0; report_packet(); }
}

void ps2mouse_handler(void) {
    uint8_t st = inb(PS2_STATUS);
    if (!(st & 0x01)) return;
    uint8_t b = inb(PS2_DATA);
    if (!(st & 0x20)) return;                /* a keyboard byte on IRQ12: should not happen */
    ps2mouse_byte(b);
}
