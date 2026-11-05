/*
 * SecOS Kernel - PS/2 Keyboard Driver
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Scancode set 1 -> the bytes a VT100/xterm-style terminal sends: printable
 * ASCII, Enter = CR, Backspace = DEL (0x7f), Ctrl+key = C0 control, Alt+key =
 * ESC prefix, and ESC [ ... sequences for the cursor/editing/function keys.
 * Bytes land in a ring buffer shared with the USB HID keyboard; the console
 * TTY (kernel/tty.c) consumes them raw and applies termios (ICRNL, ISIG, ...).
 * The kernel shell reads through keyboard_getchar(), which keeps its historic
 * '\n'/'\b' view and drops escape sequences it cannot use.
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
static uint8_t input_buffer[BUFFER_SIZE];
static int buffer_start = 0;
static int buffer_end = 0;

// Modifier key states
static bool shift_l, shift_r;
static bool ctrl_l, ctrl_r;     // [M25] Ctrl for control codes
static bool alt_l, alt_r;       // Alt = meta (ESC prefix)
static bool caps_lock = false;
static bool num_lock = false;
static int  e0_prefix = 0;      // previous byte was 0xE0 (extended key)
static int  e1_skip = 0;        // bytes left of a Pause (0xE1) sequence
static volatile int g_break_pending = 0;  // Ctrl-C with no foreground process (for shell builtins)
int  keyboard_break_pending(void) { return g_break_pending; }
void keyboard_clear_break(void)   { g_break_pending = 0; }

/* termios ISIG decision lives in the TTY (VINTR/VQUIT/VSUSP + ISIG flag). */
extern int  tty_isig_for(char c);
extern void tty_note_isig(int sig);

// Inline port I/O
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Scancode map US QWERTY -> ASCII (no shift). Enter = CR, Backspace = DEL,
// Esc = 0x1B (the bytes a terminal sends).
static const char scancode_to_ascii[] = {
    0,  0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0x7F,
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

// Scancode map US QWERTY -> ASCII (with shift)
static const char scancode_to_ascii_shift[] = {
    0,  0x1B, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0x7F,
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' '
};

/* ---- key -> bytes (shared with usb_hid.c) --------------------------------- */
static int put_str(char* out, const char* s) { int n = 0; while (s[n]) { out[n] = s[n]; n++; } return n; }

int keyboard_key_bytes(int key, int mods, char out[8]) {
    int n = 0;
    if (key >= 0x100) {
        const char* seq = 0;
        switch (key) {
        case KEY_UP:    seq = "\033[A"; break;
        case KEY_DOWN:  seq = "\033[B"; break;
        case KEY_RIGHT: seq = "\033[C"; break;
        case KEY_LEFT:  seq = "\033[D"; break;
        case KEY_HOME:  seq = "\033[H"; break;
        case KEY_END:   seq = "\033[F"; break;
        case KEY_PGUP:  seq = "\033[5~"; break;
        case KEY_PGDN:  seq = "\033[6~"; break;
        case KEY_INS:   seq = "\033[2~"; break;
        case KEY_DEL:   seq = "\033[3~"; break;
        case KEY_BACKTAB: seq = "\033[Z"; break;
        case KEY_F1:  seq = "\033OP"; break;
        case KEY_F2:  seq = "\033OQ"; break;
        case KEY_F3:  seq = "\033OR"; break;
        case KEY_F4:  seq = "\033OS"; break;
        case KEY_F5:  seq = "\033[15~"; break;
        case KEY_F6:  seq = "\033[17~"; break;
        case KEY_F7:  seq = "\033[18~"; break;
        case KEY_F8:  seq = "\033[19~"; break;
        case KEY_F9:  seq = "\033[20~"; break;
        case KEY_F10: seq = "\033[21~"; break;
        case KEY_F11: seq = "\033[23~"; break;
        case KEY_F12: seq = "\033[24~"; break;
        default: return 0;
        }
        /* xterm modifier form for Ctrl/Alt/Shift + arrows/Home/End:
         * ESC [ 1 ; <1+mods> X  (e.g. Ctrl-Right = ESC[1;5C). */
        int m = ((mods & KMOD_SHIFT) ? 1 : 0) | ((mods & KMOD_ALT) ? 2 : 0) | ((mods & KMOD_CTRL) ? 4 : 0);
        if (m && key <= KEY_END) {
            out[n++] = 0x1B; out[n++] = '['; out[n++] = '1'; out[n++] = ';';
            out[n++] = (char)('1' + m); out[n++] = seq[2];
            return n;
        }
        return put_str(out, seq);
    }
    int c = key & 0xFF;
    if (c == 0) return 0;
    if (mods & KMOD_CTRL) {
        if (c >= 'a' && c <= 'z') c &= 0x1F;
        else if (c >= '@' && c <= '_') c &= 0x1F;     /* @ A..Z [ \ ] ^ _ */
        else if (c == ' ' || c == '2') c = 0x00;
        else if (c == '6') c = 0x1E;
        else if (c == '-' || c == '/') c = 0x1F;
        else if (c == '?') c = 0x7F;
        else if (c == 0x7F) c = 0x08;                 /* Ctrl-Backspace = ^H */
    }
    if (mods & KMOD_ALT) out[n++] = 0x1B;             /* meta sends escape */
    out[n++] = (char)c;
    return n;
}

/* ---- ring buffer ------------------------------------------------------------ */
static int ring_space_nolock(void) {
    int used = (buffer_end - buffer_start + BUFFER_SIZE) % BUFFER_SIZE;
    return BUFFER_SIZE - 1 - used;
}
// Push a whole sequence or nothing (a half-queued ESC [ A would be misparsed).
static void buffer_put_seq_nolock(const char* s, int n) {
    if (n <= 0 || ring_space_nolock() < n) return;
    for (int i = 0; i < n; i++) {
        input_buffer[buffer_end] = (uint8_t)s[i];
        buffer_end = (buffer_end + 1) % BUFFER_SIZE;
    }
}
// Pop one byte (-1 if empty). Caller holds kbd_lock.
static int buffer_get_nolock(void) {
    if (buffer_start == buffer_end) return -1;
    int c = input_buffer[buffer_start];
    buffer_start = (buffer_start + 1) % BUFFER_SIZE;
    return c;
}
static int buffer_get(void) {
    uint64_t fl = spin_lock_irqsave(&kbd_lock);
    int c = buffer_get_nolock();
    spin_unlock_irqrestore(&kbd_lock, fl);
    return c;
}
static int buffer_peek(void) {
    uint64_t fl = spin_lock_irqsave(&kbd_lock);
    int c = (buffer_start == buffer_end) ? -1 : input_buffer[buffer_start];
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

// Input already waiting in the PS/2 ring buffer or the serial RX register.
// Unlike keyboard_has_char() it never polls the USB HID keyboard, so it is safe
// from the timer ISR (used to wake a blocked TTY reader promptly).
bool keyboard_input_pending_noirq(void) {
    uint64_t fl = spin_lock_irqsave(&kbd_lock);
    bool has = (buffer_start != buffer_end);
    spin_unlock_irqrestore(&kbd_lock, fl);
    return has || serial_has_char();
}

// [TTY] Raw byte for the line discipline: PS/2/USB ring first, then serial
// exactly as received (CR stays CR, DEL stays DEL; termios maps them).
int keyboard_getc_raw(void) {
    usb_hid_poll();
    int c = buffer_get();
    if (c >= 0) return c;
    return serial_poll_raw();
}

// [M22] Inject a character from another input source (USB HID keyboard).
void keyboard_inject_char(char c) {
    uint64_t fl = spin_lock_irqsave(&kbd_lock);
    buffer_put_seq_nolock(&c, 1);
    spin_unlock_irqrestore(&kbd_lock, fl);
}
void keyboard_inject_seq(const char* s, int n) {
    uint64_t fl = spin_lock_irqsave(&kbd_lock);
    buffer_put_seq_nolock(s, n);
    spin_unlock_irqrestore(&kbd_lock, fl);
}

/* Extended (0xE0-prefixed) make codes -> KEY_*. */
static int ext_key(uint8_t sc) {
    switch (sc) {
    case 0x48: return KEY_UP;    case 0x50: return KEY_DOWN;
    case 0x4D: return KEY_RIGHT; case 0x4B: return KEY_LEFT;
    case 0x47: return KEY_HOME;  case 0x4F: return KEY_END;
    case 0x49: return KEY_PGUP;  case 0x51: return KEY_PGDN;
    case 0x52: return KEY_INS;   case 0x53: return KEY_DEL;
    case 0x1C: return '\r';      /* keypad Enter */
    case 0x35: return '/';       /* keypad / */
    default:   return 0;
    }
}
/* Keypad (no 0xE0) with Num Lock off acts as the navigation keys. */
static int keypad_key(uint8_t sc) {
    static const char digits[] = "789-456+1230.";   /* 0x47..0x53 */
    if (sc < 0x47 || sc > 0x53) return 0;
    if (num_lock || sc == 0x4A || sc == 0x4E) return digits[sc - 0x47];
    switch (sc) {
    case 0x47: return KEY_HOME;  case 0x48: return KEY_UP;   case 0x49: return KEY_PGUP;
    case 0x4B: return KEY_LEFT;  case 0x4C: return 0;        case 0x4D: return KEY_RIGHT;
    case 0x4F: return KEY_END;   case 0x50: return KEY_DOWN; case 0x51: return KEY_PGDN;
    case 0x52: return KEY_INS;   case 0x53: return KEY_DEL;
    default: return 0;
    }
}
static int fkey(uint8_t sc) {
    if (sc >= 0x3B && sc <= 0x44) return KEY_F1 + (sc - 0x3B);
    if (sc == 0x57) return KEY_F11;
    if (sc == 0x58) return KEY_F12;
    return 0;
}

// Keyboard interrupt handler
void keyboard_handler(void) {
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    // [M29] Guard modifier state + ring buffer. Use the _nolock helpers below
    // since we already hold kbd_lock (it is not recursive). Every return path unlocks.
    uint64_t fl = spin_lock_irqsave(&kbd_lock);

    if (e1_skip > 0) { e1_skip--; spin_unlock_irqrestore(&kbd_lock, fl); return; }
    if (scancode == 0xE1) { e1_skip = 5; spin_unlock_irqrestore(&kbd_lock, fl); return; }  // Pause
    if (scancode == 0xE0) { e0_prefix = 1; spin_unlock_irqrestore(&kbd_lock, fl); return; }
    int ext = e0_prefix; e0_prefix = 0;

    int release = (scancode & 0x80) != 0;
    uint8_t sc = scancode & 0x7F;

    // Modifiers (make and break).
    if (!ext && (sc == 0x2A || sc == 0x36)) {      // Left/Right Shift
        if (sc == 0x2A) shift_l = !release; else shift_r = !release;
        spin_unlock_irqrestore(&kbd_lock, fl); return;
    }
    if (ext && (sc == 0x2A || sc == 0x36)) {       // fake shifts around extended keys
        spin_unlock_irqrestore(&kbd_lock, fl); return;
    }
    if (sc == 0x1D) {                              // [M25] Ctrl (E0 1D = right)
        if (ext) ctrl_r = !release; else ctrl_l = !release;
        spin_unlock_irqrestore(&kbd_lock, fl); return;
    }
    if (sc == 0x38) {                              // Alt (E0 38 = right / AltGr)
        if (ext) alt_r = !release; else alt_l = !release;
        spin_unlock_irqrestore(&kbd_lock, fl); return;
    }
    if (release) { spin_unlock_irqrestore(&kbd_lock, fl); return; }
    if (!ext && sc == 0x3A) { caps_lock = !caps_lock; spin_unlock_irqrestore(&kbd_lock, fl); return; }
    if (!ext && sc == 0x45) { num_lock = !num_lock; spin_unlock_irqrestore(&kbd_lock, fl); return; }

    int shift = shift_l || shift_r;
    int mods = (shift ? KMOD_SHIFT : 0) | ((ctrl_l || ctrl_r) ? KMOD_CTRL : 0) |
               ((alt_l || alt_r) ? KMOD_ALT : 0);

    int key = 0;
    if (ext) key = ext_key(sc);
    else if ((key = fkey(sc)) != 0) { }
    else if ((key = keypad_key(sc)) != 0) { }
    else if (sc < sizeof(scancode_to_ascii)) {
        char a;
        if (shift) a = scancode_to_ascii_shift[sc];
        else {
            a = scancode_to_ascii[sc];
            if (caps_lock && a >= 'a' && a <= 'z') a -= 32;  // Caps Lock for letters
        }
        if (shift && caps_lock && a >= 'A' && a <= 'Z') a += 32;
        key = (unsigned char)a;
        if (key == '\t' && shift) { key = KEY_BACKTAB; mods &= ~KMOD_SHIFT; }
    }
    if (!key) { spin_unlock_irqrestore(&kbd_lock, fl); return; }
    if (key >= 0x100 || key < 0x20 || key == 0x7F) {
        if (key < 0x100) mods &= ~KMOD_SHIFT;
    }

    char seq[8];
    int n = keyboard_key_bytes(key, mods, seq);
    if (n <= 0) { spin_unlock_irqrestore(&kbd_lock, fl); return; }

    // [M30] Job control: with termios ISIG on and a foreground process group
    // owning the console, VINTR/VQUIT/VSUSP are delivered as signals instead of
    // being buffered (Ctrl-C => SIGINT, Ctrl-\ => SIGQUIT, Ctrl-Z => SIGTSTP).
    // With no foreground group (kernel shell prompt) they are buffered and the
    // line editor handles them. Post after releasing kbd_lock to avoid nesting
    // it under proc_lock.
    if (n == 1) {
        uint32_t fg = signal_get_foreground_pgid();
        int sig = tty_isig_for(seq[0]);
        if (fg && sig) {
            spin_unlock_irqrestore(&kbd_lock, fl);
            tty_note_isig(sig);
            signal_post_pgid(fg, sig);
            return;
        }
        // No foreground process (shell prompt / a shell builtin is running):
        // record the Ctrl-C so a long-running builtin loop (e.g. cat /dev/random)
        // can poll keyboard_break_pending() and abort. Still buffered below so the
        // prompt line-editor cancels the current line as before.
        if (!fg && seq[0] == 0x03) g_break_pending = 1;
    }

    buffer_put_seq_nolock(seq, n);
    spin_unlock_irqrestore(&kbd_lock, fl);
}

// Initialize keyboard state
void keyboard_init(void) {
    buffer_start = 0;
    buffer_end = 0;
    shift_l = shift_r = ctrl_l = ctrl_r = alt_l = alt_r = false;
    caps_lock = false;
    num_lock = false;
    e0_prefix = 0; e1_skip = 0;
}

/* Kernel shell view of one raw byte: CR -> '\n', DEL -> '\b'. */
static int cook_legacy(int c) {
    if (c == '\r') return '\n';
    if (c == 0x7F) return '\b';
    return c;
}
static int raw_wait(void) {
    for (;;) {
        int c = keyboard_getc_raw();
        if (c >= 0) return c;
        __asm__ volatile ("hlt");  // Wait for interrupt (timer wakes ~1kHz to poll serial)
    }
}

// Read a character (blocking) for the kernel shell / pager. Escape sequences
// (arrow keys etc.) are consumed whole and ignored: the kernel line editor has
// no use for them and would otherwise insert "[A" into the command.
char keyboard_getchar(void) {
    for (;;) {
        int c = raw_wait();
        if (c != 0x1B) return (char)cook_legacy(c);
        // ESC: if a sequence follows (PS/2/USB queue it atomically; serial bytes
        // arrive back to back), swallow ESC [ params final / ESC O x.
        int nx = buffer_peek();
        if (nx < 0) {
            for (int spin = 0; spin < 20 && !keyboard_input_pending_noirq(); spin++)
                __asm__ volatile ("hlt");
            if (!keyboard_input_pending_noirq()) return 0x1B;
        }
        int c2 = raw_wait();
        if (c2 == '[') {
            int f;
            do { f = raw_wait(); } while (f >= 0x20 && f < 0x40);
        } else if (c2 == 'O') {
            (void)raw_wait();
        } else {
            return (char)cook_legacy(c2);     // Alt+key: deliver the key
        }
    }
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
