/*
 * SecOS Kernel - PS/2 Keyboard Driver
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "keyboard.h"
#include "serial.h"
#include "spinlock.h"
#include "signal.h"   // [M30] Ctrl-C/Ctrl-Z -> SIGINT/SIGTSTP to the foreground group

#define KEYBOARD_DATA_PORT 0x60
#define BUFFER_SIZE 256

// [M29] One spinlock guards both the input ring buffer and the modifier state.
// keyboard_handler runs in interrupt context, so the irqsave variants are used
// everywhere to stay atomic against this CPU's own ISR as well as other CPUs.
static spinlock_t kbd_lock = SPINLOCK_INIT;

// Circular input buffer
static char input_buffer[BUFFER_SIZE];
static int buffer_start = 0;
static int buffer_end = 0;

// Modifier key states
static bool shift_pressed = false;
static bool caps_lock = false;
static bool ctrl_pressed = false;   // [M25] Left Ctrl (scancode 0x1D) for control codes

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

// Push char into buffer (lock-free leaf — caller holds kbd_lock).
static void buffer_put_nolock(char c) {
    int next = (buffer_end + 1) % BUFFER_SIZE;
    if (next != buffer_start) {
        input_buffer[buffer_end] = c;
        buffer_end = next;
    }
}

// Pop char from buffer (lock-free leaf — caller holds kbd_lock).
static char buffer_get_nolock(void) {
    if (buffer_start == buffer_end) {
        return 0;
    }
    char c = input_buffer[buffer_start];
    buffer_start = (buffer_start + 1) % BUFFER_SIZE;
    return c;
}

// Push char into buffer.
static void buffer_put(char c) {
    uint64_t fl = spin_lock_irqsave(&kbd_lock);
    buffer_put_nolock(c);
    spin_unlock_irqrestore(&kbd_lock, fl);
}

// Pop char from buffer.
static char buffer_get(void) {
    uint64_t fl = spin_lock_irqsave(&kbd_lock);
    char c = buffer_get_nolock();
    spin_unlock_irqrestore(&kbd_lock, fl);
    return c;
}

// [M22] Poll the USB HID keyboard (if present). Safe no-op when no USB keyboard
// is attached. Driven from the input-wait path below, never from an ISR, so it
// never races the event ring against an in-flight MSC/control transfer.
extern void usb_hid_poll(void);

// Check if buffer has characters
bool keyboard_has_char(void) {
    // usb_hid_poll() can call keyboard_inject_char()->buffer_put(), which takes
    // kbd_lock — so it must run OUTSIDE the lock (kbd_lock is not recursive).
    usb_hid_poll();
    uint64_t fl = spin_lock_irqsave(&kbd_lock);
    bool has = (buffer_start != buffer_end);
    spin_unlock_irqrestore(&kbd_lock, fl);
    return has || serial_has_char();
}

// [M22] Inject a character from another input source (USB HID keyboard).
void keyboard_inject_char(char c) {
    if (c) buffer_put(c);
}

// Keyboard interrupt handler
void keyboard_handler(void) {
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    // [M29] Guard modifier state + ring buffer. Use buffer_put_nolock below since
    // we already hold kbd_lock (it is not recursive). Every return path unlocks.
    uint64_t fl = spin_lock_irqsave(&kbd_lock);

    // Handle key release (bit7 = 1)
    if (scancode & 0x80) {
        scancode &= 0x7F;
    // Shift release
        if (scancode == 0x2A || scancode == 0x36) {
            shift_pressed = false;
        }
        if (scancode == 0x1D) {                  // [M25] Ctrl release
            ctrl_pressed = false;
        }
        spin_unlock_irqrestore(&kbd_lock, fl);
        return;
    }

    // Handle special keys
    if (scancode == 0x2A || scancode == 0x36) {  // Left/Right Shift
        shift_pressed = true;
        spin_unlock_irqrestore(&kbd_lock, fl);
        return;
    }
    if (scancode == 0x1D) {                       // [M25] Left Ctrl
        ctrl_pressed = true;
        spin_unlock_irqrestore(&kbd_lock, fl);
        return;
    }

    if (scancode == 0x3A) {  // Caps Lock
        caps_lock = !caps_lock;
        spin_unlock_irqrestore(&kbd_lock, fl);
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

    // [M25] Ctrl+letter -> control code (Ctrl+C=0x03, Ctrl+D=0x04, ...). This is
    // what the TTY line discipline expects (EOF/interrupt). Without this the PS/2
    // path produced the bare letter.
    if (ctrl_pressed && ((ascii >= 'a' && ascii <= 'z') || (ascii >= 'A' && ascii <= 'Z'))) {
        ascii = (char)(ascii & 0x1F);
    }

    // [M30] Job control: when a foreground process group owns the console, the
    // control keys are delivered as signals instead of being buffered for a
    // read() — Ctrl-C => SIGINT, Ctrl-Z => SIGTSTP. At the shell prompt there is
    // no foreground group (fg==0), so they fall through and are buffered as the
    // line-editing control codes (the shell cancels the line on 0x03). Post after
    // releasing kbd_lock to avoid nesting it under proc_lock.
    {
        uint32_t fg = signal_get_foreground_pgid();
        int sig = 0;
        if (fg) {
            if (ascii == 0x03) sig = SIGINT;        // Ctrl-C
            else if (ascii == 0x1A) sig = SIGTSTP;  // Ctrl-Z
        }
        if (sig) {
            spin_unlock_irqrestore(&kbd_lock, fl);
            signal_post_pgid(fg, sig);
            return;
        }
    }

    if (ascii) {
        buffer_put_nolock(ascii);
    }
    spin_unlock_irqrestore(&kbd_lock, fl);
}

// Initialize keyboard state
void keyboard_init(void) {
    buffer_start = 0;
    buffer_end = 0;
    shift_pressed = false;
    caps_lock = false;
    ctrl_pressed = false;
}

// Read a character (blocking)
char keyboard_getchar(void) {
    while (!keyboard_has_char()) {
    __asm__ volatile ("hlt");  // Wait for interrupt (timer wakes ~1kHz to poll serial)
    }
    // PS/2 keystrokes take priority; otherwise drain a serial byte.
    if (buffer_start != buffer_end) return buffer_get();
    int sc = serial_poll_char();
    return (sc >= 0) ? (char)sc : 0;
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