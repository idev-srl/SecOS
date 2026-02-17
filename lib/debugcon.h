/*
 * debugcon.h — Minimal ISA debugcon output (QEMU port 0xE9).
 *
 * Writes appear in QEMU's -debugcon target (file, stdio, etc.).
 * Used exclusively for early-boot markers and crash diagnosis;
 * NOT a general-purpose print layer.
 *
 * All functions are static inline — no compilation unit dependency.
 */
#pragma once

#include <stdint.h>

/* Write one character to ISA debugcon port 0xE9. */
static inline void debugcon_putchar(char c) {
    __asm__ volatile ("outb %0, $0xE9" : : "a"(c) : "memory");
}

/* Write a NUL-terminated string to debugcon. */
static inline void debugcon_writestring(const char *s) {
    while (*s) debugcon_putchar(*s++);
}

/* Write a uint64_t as 18-char "0x<16 hex digits>" to debugcon. */
static inline void debugcon_print_hex(uint64_t v) {
    static const char h[] = "0123456789ABCDEF";
    char buf[19];
    buf[0]  = '0'; buf[1] = 'x';
    for (int i = 17; i >= 2; i--) { buf[i] = h[v & 0xF]; v >>= 4; }
    buf[18] = '\0';
    debugcon_writestring(buf);
}
