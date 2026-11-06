/*
 * SecOS Kernel - PS/2 (i8042 AUX) mouse
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef PS2MOUSE_H
#define PS2MOUSE_H
#include <stdint.h>

/* Probe + enable the AUX port mouse (IntelliMouse wheel if supported) and
 * register /dev/input/eventN. Call with the keyboard already initialised and
 * before IRQ12 is unmasked. Returns 0 if a mouse answered. */
int  ps2mouse_init(void);
/* IRQ12 handler (vector 0x2C). */
void ps2mouse_handler(void);
/* One byte read from the data port with the AUX bit set (also used when such a
 * byte surfaces on the keyboard IRQ). */
void ps2mouse_byte(uint8_t b);

#endif
