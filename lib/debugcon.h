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

/* Kernel log ring (kernel/klog.c): every kernel debugcon byte is also kept
 * there for dmesg / SYS_SYSLOG / /proc/kmsg. */
void klog_putc(char c);

/* Write one character to ISA debugcon port 0xE9 only (not the kernel log):
 * for user programs' console output, which is not a kernel message. */
static inline void debugcon_putchar_raw(char c) {
    __asm__ volatile ("outb %0, $0xE9" : : "a"(c) : "memory");
}

/* [M48] Kernel debugcon output is line-atomic across CPUs (kernel/klog.c):
 * with more than one CPU each CPU assembles its line privately and emits it
 * whole (port 0xE9 + kernel log) at '\n'; single-core output is immediate. */
void debugcon_kputc(char c);
void debugcon_kwrite(const char* s);
void debugcon_kflush(void);          /* emit this CPU's partial line now (panic) */

/* Write one character to ISA debugcon port 0xE9 (and the kernel log). */
static inline void debugcon_putchar(char c) {
    debugcon_kputc(c);
}

/* Write a NUL-terminated string to debugcon. */
static inline void debugcon_writestring(const char *s) {
    debugcon_kwrite(s);
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
