/*
 * SecOS Kernel - 16550 UART (COM1) serial console
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "serial.h"
#include <stdint.h>

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r; __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
}

static int ready = 0;

void serial_init(void) {
    outb(COM1 + 1, 0x00); // disable UART interrupts (driver is polled)
    outb(COM1 + 3, 0x80); // DLAB on
    outb(COM1 + 0, 0x01); // divisor low  = 1  -> 115200 baud
    outb(COM1 + 1, 0x00); // divisor high = 0
    outb(COM1 + 3, 0x03); // DLAB off, 8 bits, no parity, 1 stop
    outb(COM1 + 2, 0xC7); // FIFO enable + clear, 14-byte threshold
    outb(COM1 + 4, 0x0B); // DTR + RTS + OUT2

    // Presence test: a non-existent UART (VMware / real PC with no COM port)
    // reads 0xFF on every register, so the Line Status Register's Data-Ready bit
    // looks permanently set and the polled input path floods the console with
    // 0xFF "characters". Put the UART in loopback, send a byte, and require it to
    // come back; if it doesn't, there is no UART — disable serial entirely.
    outb(COM1 + 4, 0x1E);            // loopback mode (LOOP | OUT2 | OUT1 | RTS)
    outb(COM1 + 0, 0xAE);            // transmit a test byte
    if (inb(COM1 + 0) != 0xAE) {     // not looped back -> no real UART
        outb(COM1 + 4, 0x0B);
        ready = 0;
        return;
    }
    outb(COM1 + 4, 0x0F);            // normal operation (DTR | RTS | OUT1 | OUT2)
    outb(COM1 + 2, 0xC7);            // clear FIFOs (drop the loopback test byte)
    ready = 1;
}

int serial_is_ready(void) { return ready; }

static inline int tx_ready(void) { return inb(COM1 + 5) & 0x20; } // THR empty

void serial_putchar(char c) {
    if (!ready) return;
    if (c == '\n') { while (!tx_ready()) { } outb(COM1, '\r'); }
    while (!tx_ready()) { }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char* s) {
    if (!s) return;
    while (*s) serial_putchar(*s++);
}

int serial_has_char(void) {
    if (!ready) return 0;
    return inb(COM1 + 5) & 0x01; // Data Ready
}

int serial_poll_char(void) {
    if (!serial_has_char()) return -1;
    uint8_t c = inb(COM1);
    if (c == '\r') c = '\n';   // Enter on most terminals
    if (c == 0x7F) c = '\b';   // DEL -> backspace
    return (int)c;
}
