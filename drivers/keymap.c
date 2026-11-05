/*
 * SecOS Kernel - Keyboard layouts (keymaps)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Tables are indexed by Linux key code (drivers/input.h). Columns: plain,
 * Shift, AltGr, Shift+AltGr. The Italian table follows the standard "it"
 * layout (xkb it / Windows Italian): AltGr+è/+ give [ ], Shift+AltGr+è/+ give
 * { }, AltGr+ò/à give @ #, AltGr+'/ì give ` ~, AltGr+e gives the Euro sign, and
 * the 102nd key (left of Z) gives < >.
 */
#include "keymap.h"
#include "input.h"
#include <stddef.h>

#define K(a, b) { (a), (b), 0, 0 }
#define KA(a, b, c, d) { (a), (b), (c), (d) }

/* Letters are identical in both layouts. */
#define LETTERS \
    [KC_Q] = K('q', 'Q'), [KC_W] = K('w', 'W'), [KC_R] = K('r', 'R'), \
    [KC_T] = K('t', 'T'), [KC_Y] = K('y', 'Y'), [KC_U] = K('u', 'U'), [KC_I] = K('i', 'I'), \
    [KC_O] = K('o', 'O'), [KC_P] = K('p', 'P'), [KC_A] = K('a', 'A'), [KC_S] = K('s', 'S'), \
    [KC_D] = K('d', 'D'), [KC_F] = K('f', 'F'), [KC_G] = K('g', 'G'), [KC_H] = K('h', 'H'), \
    [KC_J] = K('j', 'J'), [KC_K] = K('k', 'K'), [KC_L] = K('l', 'L'), [KC_Z] = K('z', 'Z'), \
    [KC_X] = K('x', 'X'), [KC_C] = K('c', 'C'), [KC_V] = K('v', 'V'), [KC_B] = K('b', 'B'), \
    [KC_N] = K('n', 'N'), [KC_M] = K('m', 'M'), [KC_SPACE] = K(' ', ' ')

static const keymap_t km_us = {
    "us", "US English (QWERTY)", 0, {
        LETTERS,
        [KC_E] = K('e', 'E'),
        [KC_1] = K('1', '!'), [KC_2] = K('2', '@'), [KC_3] = K('3', '#'), [KC_4] = K('4', '$'),
        [KC_5] = K('5', '%'), [KC_6] = K('6', '^'), [KC_7] = K('7', '&'), [KC_8] = K('8', '*'),
        [KC_9] = K('9', '('), [KC_0] = K('0', ')'), [KC_MINUS] = K('-', '_'), [KC_EQUAL] = K('=', '+'),
        [KC_LEFTBRACE] = K('[', '{'), [KC_RIGHTBRACE] = K(']', '}'),
        [KC_SEMICOLON] = K(';', ':'), [KC_APOSTROPHE] = K('\'', '"'), [KC_GRAVE] = K('`', '~'),
        [KC_BACKSLASH] = K('\\', '|'), [KC_COMMA] = K(',', '<'), [KC_DOT] = K('.', '>'),
        [KC_SLASH] = K('/', '?'),
        [KC_102ND] = K('<', '>'),          /* ISO keyboards: the extra key left of Z */
    }
};

static const keymap_t km_it = {
    "it", "Italian", 1, {
        LETTERS,
        [KC_E] = KA('e', 'E', 0x20AC, 0),                       /* AltGr+e = EUR */
        [KC_1] = K('1', '!'), [KC_2] = K('2', '"'), [KC_3] = K('3', 0x00A3 /* £ */),
        [KC_4] = K('4', '$'), [KC_5] = KA('5', '%', 0x20AC, 0), [KC_6] = K('6', '&'),
        [KC_7] = K('7', '/'), [KC_8] = K('8', '('), [KC_9] = K('9', ')'), [KC_0] = K('0', '='),
        [KC_MINUS] = KA('\'', '?', '`', 0),                       /* ' ?  AltGr: ` */
        [KC_EQUAL] = KA(0x00EC /* ì */, '^', '~', 0),             /* ì ^  AltGr: ~ */
        [KC_LEFTBRACE] = KA(0x00E8 /* è */, 0x00E9 /* é */, '[', '{'),
        [KC_RIGHTBRACE] = KA('+', '*', ']', '}'),
        [KC_SEMICOLON] = KA(0x00F2 /* ò */, 0x00E7 /* ç */, '@', 0),
        [KC_APOSTROPHE] = KA(0x00E0 /* à */, 0x00B0 /* ° */, '#', 0),
        [KC_GRAVE] = KA('\\', '|', 0x00AC /* ¬ */, 0),
        [KC_BACKSLASH] = K(0x00F9 /* ù */, 0x00A7 /* § */),       /* ISO key left of Enter */
        [KC_COMMA] = K(',', ';'), [KC_DOT] = K('.', ':'), [KC_SLASH] = K('-', '_'),
        [KC_102ND] = KA('<', '>', 0x00AB /* « */, 0x00BB /* » */),
    }
};

static const keymap_t* const g_maps[] = { &km_us, &km_it };
#define NMAPS ((int)(sizeof(g_maps) / sizeof(g_maps[0])))

#ifndef SECOS_KEYMAP_DEFAULT
#define SECOS_KEYMAP_DEFAULT "us"
#endif

static const keymap_t* g_active;

static int s_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

const keymap_t* keymap_find(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < NMAPS; i++) if (s_eq(g_maps[i]->name, name)) return g_maps[i];
    return NULL;
}

const keymap_t* keymap_active(void) {
    if (!g_active) {
        g_active = keymap_find(SECOS_KEYMAP_DEFAULT);
        if (!g_active) g_active = &km_us;
    }
    return g_active;
}

int keymap_select(const char* name) {
    const keymap_t* m = keymap_find(name);
    if (!m) return -1;
    g_active = m;
    return 0;
}

const char* keymap_list(void) { return "us it"; }

static int is_lower_letter(uint32_t cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7);
}

uint32_t keymap_lookup(int kc, int shift, int altgr, int caps) {
    const keymap_t* m = keymap_active();
    if (kc <= 0 || kc >= KEYMAP_KEYS) return 0;
    const uint16_t* e = m->map[kc];
    if (altgr && m->has_altgr) {
        uint32_t cp = shift ? e[KM_SHIFT_ALTGR] : e[KM_ALTGR];
        return cp;                         /* AltGr with no symbol: nothing */
    }
    uint32_t plain = e[KM_PLAIN];
    /* Caps Lock flips the case of letters only (digits/punctuation unaffected). */
    if (caps && is_lower_letter(plain)) shift = !shift;
    return shift ? e[KM_SHIFT] : plain;
}

int keymap_utf8(uint32_t cp, char out[4]) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}
