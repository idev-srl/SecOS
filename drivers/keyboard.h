/*
 * SecOS Kernel - Keyboard Driver (Header)
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

void keyboard_init(void);              // Initialize driver
void keyboard_handler(void);           // Interrupt handler (ISR)
char keyboard_getchar(void);           // Blocking read char
bool keyboard_has_char(void);          // Non-blocking availability check
void keyboard_readline(char* buffer, int max_len); // Read line until Enter

#endif