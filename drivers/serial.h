/*
 * SecOS Kernel - 16550 UART (COM1) serial console (Header)
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Minimal polled serial driver for a headless console: terminal output is
 * mirrored to COM1, and serial RX is fed into the shell input path.  Lets the
 * interactive shell run entirely over `qemu -serial stdio -display none`,
 * without a graphical framebuffer window.
 */
#ifndef SERIAL_H
#define SERIAL_H

void serial_init(void);            // Configure COM1 (115200 8N1)
int  serial_is_ready(void);        // 1 after serial_init()
void serial_putchar(char c);       // TX one byte ('\n' -> "\r\n")
void serial_write(const char* s);  // TX NUL-terminated string
int  serial_has_char(void);        // 1 if an RX byte is waiting
int  serial_poll_char(void);       // Non-blocking RX: byte, or -1 if none

#endif // SERIAL_H
