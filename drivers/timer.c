/*
 * SecOS Kernel - PIT Timer Driver
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "timer.h"
#include "sched.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_BASE_FREQUENCY 1193182

// Tick counter
static volatile uint64_t timer_ticks = 0;
static uint32_t timer_frequency = 0;
// Registered tick callbacks (simple static array)
#define MAX_TICK_CBS 8
static timer_tick_cb_t tick_cbs[MAX_TICK_CBS];
static int tick_cb_count = 0;

// Inline port I/O functions
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Timer interrupt handler (IRQ0)
void timer_handler(void) {
    timer_ticks++;
    sched_on_timer_tick();
    // Execute registered callbacks
    for(int i=0;i<tick_cb_count;i++) {
        if (tick_cbs[i]) tick_cbs[i]();
    }
}

// Initialize PIT timer
void timer_init(uint32_t frequency) {
    timer_frequency = frequency;
    timer_ticks = 0;
    
    // Calculate divisor to program desired frequency
    uint32_t divisor = PIT_BASE_FREQUENCY / frequency;
    
    // Clamp divisor into valid range
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1) divisor = 1;
    
    // Send command to PIT
    // 0x36 = 00110110
    // 00 = Channel 0
    // 11 = Access mode: lobyte/hibyte
    // 011 = Mode 3 (square wave)
    // 0 = Binary mode
    outb(PIT_COMMAND, 0x36);
    
    // Send divisor (low byte then high byte)
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
    // Clear callbacks
    for(int i=0;i<MAX_TICK_CBS;i++) tick_cbs[i]=0; tick_cb_count=0;
}

// Ottieni il numero di tick
uint64_t timer_get_ticks(void) {
    return timer_ticks;
}

// Ottieni uptime in secondi
uint64_t timer_get_uptime_seconds(void) {
    return timer_ticks / timer_frequency;
}

// Ottieni la frequenza
uint32_t timer_get_frequency(void) {
    return timer_frequency;
}

int timer_register_tick_callback(timer_tick_cb_t cb) {
    if (tick_cb_count >= MAX_TICK_CBS) return -1;
    tick_cbs[tick_cb_count++] = cb;
    return 0;
}

// Sleep per un numero di tick (bloccante)
void timer_sleep(uint32_t ticks) {
    uint64_t target = timer_ticks + ticks;
    while (timer_ticks < target) {
        __asm__ volatile ("hlt");  // Attendi interrupt
    }
}

// Sleep per millisecondi
void timer_sleep_ms(uint32_t ms) {
    uint32_t ticks = (ms * timer_frequency) / 1000;
    timer_sleep(ticks);
}