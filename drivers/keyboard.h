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

/* Non-ASCII keys, translated to the xterm/VT100 byte sequences a terminal
 * sends (see keyboard_key_bytes). Shared by the PS/2 and USB HID drivers. */
enum {
    KEY_UP = 0x100, KEY_DOWN, KEY_RIGHT, KEY_LEFT,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_INS, KEY_DEL,
    KEY_BACKTAB,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
};
/* Modifier bits for keyboard_key_bytes. */
#define KMOD_SHIFT 0x1
#define KMOD_ALT   0x2
#define KMOD_CTRL  0x4

void keyboard_init(void);              // Initialize driver
void keyboard_handler(void);           // Interrupt handler (ISR)
char keyboard_getchar(void);           // Blocking read char (kernel shell: '\n', '\b', no ESC sequences)
bool keyboard_has_char(void);          // Non-blocking availability check
bool keyboard_input_pending_noirq(void); // PS/2 buffer or serial RX waiting (no USB poll; ISR-safe)
int  keyboard_getc_raw(void);          // [TTY] Non-blocking raw byte (PS/2/USB ring, then serial), -1 if none
void keyboard_readline(char* buffer, int max_len); // Read line until Enter
void keyboard_inject_char(char c);     // [M22] Inject a char (e.g. from a USB HID keyboard)
void keyboard_inject_seq(const char* s, int n); // inject a whole key sequence atomically
/* Bytes for a key press: `key` is an ASCII code (Enter = '\r', Backspace =
 * 0x7f) or a KEY_* code; `mods` are KMOD_* bits. Ctrl turns letters and
 * @[\]^_ into C0 controls, Alt prefixes ESC (meta). Returns the byte count. */
int  keyboard_key_bytes(int key, int mods, char out[8]);
// Ctrl-C pressed with no foreground process group (i.e. at the shell / inside a
// shell builtin). Long-running builtins poll this to abort; the shell clears it
// before each command. Lets `cat /dev/random` etc. be interrupted.
int  keyboard_break_pending(void);
void keyboard_clear_break(void);
/* [M40] One key event (Linux key code, 1 = press / 2 = repeat / 0 = release)
 * from any keyboard into the console: modifiers, keymap, ISIG, byte ring. */
void keyboard_key_event(int keycode, int value);
void keyboard_register_input(void);    /* PS/2 keyboard -> /dev/input/eventN */
const char* keyboard_keymap_name(void);
int  keyboard_set_keymap(const char* name);  /* 0, or -1 for an unknown layout */

#endif
