/*
 * SecOS Kernel - Port I/O helpers
 * Shared inline wrappers for x86 port-mapped I/O (byte/word/dword).
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>

static inline void io_outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t io_inb(uint16_t port) {
    uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
static inline void io_outw(uint16_t port, uint16_t v) {
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint16_t io_inw(uint16_t port) {
    uint16_t v; __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
static inline void io_outl(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint32_t io_inl(uint16_t port) {
    uint32_t v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port)); return v;
}

/* Full memory barrier (DMA ordering vs. device). x86 is strongly ordered, but
 * mfence guarantees prior stores are globally visible before the doorbell. */
static inline void io_mfence(void) {
    __asm__ volatile ("mfence" ::: "memory");
}
