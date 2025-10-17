/*
 * SecOS Kernel - PS/2 Keyboard Driver
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "keyboard.h"

#define KEYBOARD_DATA_PORT 0x60
#define BUFFER_SIZE 256

// Circular input buffer
static char input_buffer[BUFFER_SIZE];
static int buffer_start = 0;
static int buffer_end = 0;

// Modifier key states
static bool shift_pressed = false;
static bool caps_lock = false;

// Inline port I/O
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Scancode map US QWERTY -> ASCII (no shift)
static const char scancode_to_ascii[] = {
    0,  0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

// Scancode map US QWERTY -> ASCII (with shift)
static const char scancode_to_ascii_shift[] = {
    0,  0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' '
};

// Push char into buffer
static void buffer_put(char c) {
    int next = (buffer_end + 1) % BUFFER_SIZE;
    if (next != buffer_start) {
        input_buffer[buffer_end] = c;
        buffer_end = next;
    }
}

// Pop char from buffer
static char buffer_get(void) {
    if (buffer_start == buffer_end) {
        return 0;
    }
    char c = input_buffer[buffer_start];
    buffer_start = (buffer_start + 1) % BUFFER_SIZE;
    return c;
}

// Check if buffer has characters
bool keyboard_has_char(void) {
    return buffer_start != buffer_end;
}

// Keyboard interrupt handler
void keyboard_handler(void) {
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);
    
    // Handle key release (bit7 = 1)
    if (scancode & 0x80) {
        scancode &= 0x7F;
    // Shift release
        if (scancode == 0x2A || scancode == 0x36) {
            shift_pressed = false;
        }
        return;
    }
    
    // Handle special keys
    if (scancode == 0x2A || scancode == 0x36) {  // Left/Right Shift
        shift_pressed = true;
        return;
    }
    
    if (scancode == 0x3A) {  // Caps Lock
        caps_lock = !caps_lock;
        return;
    }
    
    // Convert scancode to ASCII
    char ascii = 0;
    if (scancode < sizeof(scancode_to_ascii)) {
        if (shift_pressed) {
            ascii = scancode_to_ascii_shift[scancode];
        } else {
            ascii = scancode_to_ascii[scancode];
            // Apply Caps Lock for letters
            if (caps_lock && ascii >= 'a' && ascii <= 'z') {
                ascii -= 32;  // Convert to uppercase
            }
        }
    }
    
    if (ascii) {
        buffer_put(ascii);
    }
}

// Initialize keyboard state
void keyboard_init(void) {
    buffer_start = 0;
    buffer_end = 0;
    shift_pressed = false;
    caps_lock = false;
}

// Read a character (blocking)
char keyboard_getchar(void) {
    while (!keyboard_has_char()) {
    __asm__ volatile ("hlt");  // Wait for interrupt
    }
    return buffer_get();
}

// Read a full line (until Enter)
void keyboard_readline(char* buffer, int max_len) {
    int pos = 0;
    
    while (pos < max_len - 1) {
        char c = keyboard_getchar();
        
        if (c == '\n') {
            buffer[pos] = '\0';
            return;
        } else if (c == '\b') {
            if (pos > 0) {
                pos--;
            }
        } else {
            buffer[pos++] = c;
        }
    }
    
    buffer[pos] = '\0';
}