#ifndef TIMER_H
#define TIMER_H
/*
 * SecOS Kernel - Timer Driver
 * Provides basic timer functionality and tick management.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

// Inizializza il timer PIT
void timer_init(uint32_t frequency);

// Handler interrupt (chiamato da assembly)
void timer_handler(void);

// Ottieni il numero di tick dall'avvio
uint64_t timer_get_ticks(void);

// Ottieni uptime in secondi
uint64_t timer_get_uptime_seconds(void);

// Sleep per un numero di tick
void timer_sleep(uint32_t ticks);

// Sleep per millisecondi
void timer_sleep_ms(uint32_t ms);

// Ottieni la frequenza del timer
uint32_t timer_get_frequency(void);

// Registrazione callback tick (chiamato ogni interrupt timer)
typedef void (*timer_tick_cb_t)(void);
int timer_register_tick_callback(timer_tick_cb_t cb);

#endif