/*
 * SecOS Kernel - Keyboard layouts (keymaps)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Both keyboard paths (PS/2 scancode set 1 and USB HID usages) are first turned
 * into Linux key codes (drivers/input.h KC_*); a keymap then turns a key code
 * plus the Shift / AltGr state into a Unicode code point. Layout-independent
 * keys (Enter, Backspace, Tab, Esc, arrows, F-keys, keypad) never reach the
 * keymap. The console is byte oriented, so code points >= 0x80 are delivered
 * as UTF-8.
 *
 * Layouts: "us" (code default) and "it". The boot default is chosen at build
 * time (make KEYMAP=it -> -DSECOS_KEYMAP_DEFAULT=\"it\"); at runtime the
 * console tty ioctl KDSKBLAYOUT / the `loadkeys` command switch it.
 */
#ifndef KEYMAP_H
#define KEYMAP_H
#include <stdint.h>

#define KEYMAP_KEYS 128       /* Linux key codes 0..127 */

/* Columns of a keymap entry. */
#define KM_PLAIN       0
#define KM_SHIFT       1
#define KM_ALTGR       2
#define KM_SHIFT_ALTGR 3

typedef struct keymap {
    const char* name;                      /* "us", "it" */
    const char* desc;
    int has_altgr;                         /* right Alt is AltGr (else it is Alt) */
    uint16_t map[KEYMAP_KEYS][4];          /* Unicode code points, 0 = none */
} keymap_t;

/* Look a layout up by name (NULL if unknown). */
const keymap_t* keymap_find(const char* name);
/* Active layout; select by name (0 ok, -1 unknown). */
const keymap_t* keymap_active(void);
int  keymap_select(const char* name);
/* Names of all layouts, space separated (for error messages / the shell). */
const char* keymap_list(void);
/* Code point for key code `kc` in column `col` of the active layout, with Caps
 * Lock applied to letters (`caps`). 0 if the key produces nothing. */
uint32_t keymap_lookup(int kc, int shift, int altgr, int caps);
/* UTF-8 encode `cp` into out (up to 4 bytes); returns the byte count. */
int  keymap_utf8(uint32_t cp, char out[4]);

#endif
