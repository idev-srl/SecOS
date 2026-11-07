/*
 * SecOS Kernel - PS/2 Keyboard Driver
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Scancode set 1 -> Linux key codes -> (keymap) the bytes a VT100/xterm-style
 * terminal sends: printable characters (UTF-8 above ASCII), Enter = CR, Backspace = DEL (0x7f), Ctrl+key = C0 control, Alt+key =
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
#include "input.h"    // [M40] evdev node for the PS/2 keyboard
#include "keymap.h"   // [M40] keyboard layouts (us, it)
#include "ps2mouse.h" // [M40] AUX bytes that reach the keyboard IRQ
#include "klock.h"   /* [M50] cpu_wait_irq */

#define KEYBOARD_DATA_PORT 0x60
#define BUFFER_SIZE 256

// [M29] One spinlock guards both the input ring buffer and the modifier state.
// keyboard_handler runs in interrupt context, so the irqsave variants are used
// everywhere to stay atomic against this CPU's own ISR as well as other CPUs.
static spinlock_t kbd_lock = SPINLOCK_INIT_CLASS(LK_S50_KBD, "kbd");

// Circular input buffer
static uint8_t input_buffer[BUFFER_SIZE];
static int buffer_start = 0;
static int buffer_end = 0;

// Modifier key states
static bool shift_l, shift_r;
static bool ctrl_l, ctrl_r;     // [M25] Ctrl for control codes
static bool alt_l, alt_r;       // Alt = meta (ESC prefix); right Alt = AltGr on layouts that have it
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
// Returns 1 when queued.
static int buffer_put_seq_nolock(const char* s, int n) {
    if (n <= 0 || ring_space_nolock() < n) return 0;
    for (int i = 0; i < n; i++) {
        input_buffer[buffer_end] = (uint8_t)s[i];
        buffer_end = (buffer_end + 1) % BUFFER_SIZE;
    }
    return 1;
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
// is attached. Driven from the input-wait path below, never from an ISR.
// [M50 B3] Callable on any CPU at once: it only trylocks the HID and xHCI
// locks, so it returns at once (no keystroke lost, the next poll collects it)
// while another CPU polls or runs a USB Mass Storage transfer.
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
    return keyboard_getc_raw_nopoll();
}
// [M51] The same without the USB HID poll: the console line discipline polls
// the HID with its lock dropped, then drains the ring with this under the lock
// (an xHCI transfer must not run with the console locked and interrupts off).
int keyboard_getc_raw_nopoll(void) {
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
    (void)keyboard_inject_seq_try(s, n);
}
// The whole sequence, or nothing when the ring is full: 1 = queued.
int keyboard_inject_seq_try(const char* s, int n) {
    uint64_t fl = spin_lock_irqsave(&kbd_lock);
    int r = buffer_put_seq_nolock(s, n);
    spin_unlock_irqrestore(&kbd_lock, fl);
    return r;
}

/* ---- [M40] key codes, keymaps, evdev ---------------------------------------
 * Both keyboard paths report Linux key codes (KC_*): the PS/2 handler converts
 * scancode set 1 (+ the 0xE0 prefix), usb_hid.c converts HID usages. Every key
 * event goes to the device's /dev/input/eventN node and, unless a client holds
 * EVIOCGRAB, through keyboard_key_event() into the console: modifier state,
 * the active keymap (drivers/keymap.c), termios ISIG, and the byte ring. */
static input_dev_t g_ps2kbd;
static int g_ps2kbd_ready;

/* Extended (0xE0-prefixed) set-1 make codes -> key codes. */
static int ps2_ext_keycode(uint8_t sc) {
    switch (sc) {
    case 0x1C: return KC_KPENTER;   case 0x1D: return KC_RIGHTCTRL;
    case 0x35: return KC_KPSLASH;   case 0x37: return KC_SYSRQ;
    case 0x38: return KC_RIGHTALT;  case 0x47: return KC_HOME;
    case 0x48: return KC_UP;        case 0x49: return KC_PAGEUP;
    case 0x4B: return KC_LEFT;      case 0x4D: return KC_RIGHT;
    case 0x4F: return KC_END;       case 0x50: return KC_DOWN;
    case 0x51: return KC_PAGEDOWN;  case 0x52: return KC_INSERT;
    case 0x53: return KC_DELETE;    case 0x5B: return KC_LEFTMETA;
    case 0x5C: return KC_RIGHTMETA; case 0x5D: return KC_COMPOSE;
    default:   return 0;
    }
}

/* Layout-independent keys -> ASCII control / KEY_* terminal key; 0 = ask the
 * keymap, -1 = the key produces nothing. */
static int special_key(int kc, int shift) {
    switch (kc) {
    case KC_ESC:       return 0x1B;
    case KC_BACKSPACE: return 0x7F;
    case KC_TAB:       return shift ? KEY_BACKTAB : '\t';
    case KC_ENTER: case KC_KPENTER: return '\r';
    case KC_UP:    return KEY_UP;    case KC_DOWN:  return KEY_DOWN;
    case KC_LEFT:  return KEY_LEFT;  case KC_RIGHT: return KEY_RIGHT;
    case KC_HOME:  return KEY_HOME;  case KC_END:   return KEY_END;
    case KC_PAGEUP: return KEY_PGUP; case KC_PAGEDOWN: return KEY_PGDN;
    case KC_INSERT: return KEY_INS;  case KC_DELETE: return KEY_DEL;
    case KC_KPSLASH: return '/';     case KC_KPASTERISK: return '*';
    case KC_KPMINUS: return '-';     case KC_KPPLUS: return '+';
    case KC_F11: return KEY_F11;     case KC_F12: return KEY_F12;
    default: break;
    }
    if (kc >= KC_F1 && kc <= KC_F10) return KEY_F1 + (kc - KC_F1);
    if (kc >= KC_KP7 && kc <= KC_KPDOT) {
        /* Keypad: digits with Num Lock on, else the navigation keys. */
        static const char digits[] = "789-456+1230.";
        if (num_lock) return digits[kc - KC_KP7];
        switch (kc) {
        case KC_KP7: return KEY_HOME;  case KC_KP8: return KEY_UP;    case KC_KP9: return KEY_PGUP;
        case KC_KP4: return KEY_LEFT;  case KC_KP6: return KEY_RIGHT;
        case KC_KP1: return KEY_END;   case KC_KP2: return KEY_DOWN;  case KC_KP3: return KEY_PGDN;
        case KC_KP0: return KEY_INS;   case KC_KPDOT: return KEY_DEL;
        default: return -1;            /* KP5 with Num Lock off: nothing */
        }
    }
    return 0;
}

void keyboard_key_event(int kc, int value) {
    uint64_t fl = spin_lock_irqsave(&kbd_lock);
    switch (kc) {                                  /* modifiers: make and break */
    case KC_LEFTSHIFT:  shift_l = value != 0; goto out;
    case KC_RIGHTSHIFT: shift_r = value != 0; goto out;
    case KC_LEFTCTRL:   ctrl_l  = value != 0; goto out;
    case KC_RIGHTCTRL:  ctrl_r  = value != 0; goto out;
    case KC_LEFTALT:    alt_l   = value != 0; goto out;
    case KC_RIGHTALT:   alt_r   = value != 0; goto out;
    case KC_LEFTMETA: case KC_RIGHTMETA: goto out;
    default: break;
    }
    if (!value) goto out;                          /* releases end here */
    if (kc == KC_CAPSLOCK) { caps_lock = !caps_lock; goto out; }
    if (kc == KC_NUMLOCK)  { num_lock = !num_lock; goto out; }
    if (kc == KC_SCROLLLOCK || kc == KC_SYSRQ || kc == KC_PAUSE || kc == KC_COMPOSE) goto out;

    int shift = shift_l || shift_r;
    int ctrl  = ctrl_l || ctrl_r;
    const keymap_t* km = keymap_active();
    int altgr = alt_r && km->has_altgr;            /* right Alt = AltGr on such layouts */
    int alt   = alt_l || (alt_r && !km->has_altgr);

    char seq[8];
    int n = 0;
    int key = special_key(kc, shift);
    if (key < 0) goto out;
    if (key) {
        int mods = (ctrl ? KMOD_CTRL : 0) | (alt ? KMOD_ALT : 0);
        if (key >= 0x100 && key != KEY_BACKTAB && shift) mods |= KMOD_SHIFT;
        n = keyboard_key_bytes(key, mods, seq);
    } else {
        uint32_t cp = keymap_lookup(kc, shift, altgr, caps_lock);
        if (!cp) goto out;
        /* Windows sends AltGr as LeftCtrl+RightAlt: an AltGr symbol never
         * becomes a control code. */
        if (altgr) ctrl = 0;
        if (cp < 0x80) {
            n = keyboard_key_bytes((int)cp, (ctrl ? KMOD_CTRL : 0) | (alt ? KMOD_ALT : 0), seq);
        } else {
            if (alt) seq[n++] = 0x1B;              /* meta prefix */
            n += keymap_utf8(cp, seq + n);         /* the console is UTF-8 */
        }
    }
    if (n <= 0) goto out;

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
out:
    spin_unlock_irqrestore(&kbd_lock, fl);
}

// PS/2 keyboard interrupt handler (IRQ1)
void keyboard_handler(void) {
    // A byte from the AUX (mouse) port can sit in the shared output buffer:
    // hand it to the mouse driver instead of misreading it as a scancode.
    uint8_t status = inb(0x64);
    if (!(status & 0x01)) return;                  /* spurious: nothing to read */
    if (status & 0x20) { ps2mouse_byte(inb(KEYBOARD_DATA_PORT)); return; }
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    uint64_t fl = spin_lock_irqsave(&kbd_lock);
    if (e1_skip > 0) { e1_skip--; spin_unlock_irqrestore(&kbd_lock, fl); return; }
    if (scancode == 0xE1) { e1_skip = 5; spin_unlock_irqrestore(&kbd_lock, fl); return; }  // Pause
    if (scancode == 0xE0) { e0_prefix = 1; spin_unlock_irqrestore(&kbd_lock, fl); return; }
    int ext = e0_prefix; e0_prefix = 0;
    spin_unlock_irqrestore(&kbd_lock, fl);

    int release = (scancode & 0x80) != 0;
    uint8_t sc = scancode & 0x7F;
    int kc;
    if (ext) {
        if (sc == 0x2A || sc == 0x36) return;      /* fake shifts around extended keys */
        kc = ps2_ext_keycode(sc);
    } else {
        /* Set-1 make codes ARE the Linux key codes for the main block. */
        kc = (sc <= KC_KPDOT || sc == KC_102ND || sc == KC_F11 || sc == KC_F12) ? sc : 0;
    }
    if (!kc) return;
    if (g_ps2kbd_ready) {
        input_event(&g_ps2kbd, EV_KEY, (uint16_t)kc, release ? 0 : 1);
        input_sync(&g_ps2kbd);
        if (input_grabbed(&g_ps2kbd)) return;      /* EVIOCGRAB: the console does not see it */
    }
    keyboard_key_event(kc, release ? 0 : 1);
}

// Initialize keyboard state
void keyboard_init(void) {
    buffer_start = 0;
    buffer_end = 0;
    shift_l = shift_r = ctrl_l = ctrl_r = alt_l = alt_r = false;
    caps_lock = false;
    num_lock = false;
    e0_prefix = 0; e1_skip = 0;
    (void)keymap_active();                         /* resolve the build-time default */
}

// [M40] Expose the PS/2 keyboard as an evdev device (/dev/input/eventN).
void keyboard_register_input(void) {
    if (g_ps2kbd_ready) return;
    input_dev_t* d = &g_ps2kbd;
    const char* nm = "AT Translated Set 2 keyboard";
    int i = 0;
    for (; nm[i]; i++) d->name[i] = nm[i];
    d->name[i] = 0;
    const char* ph = "isa0060/serio0/input0";
    for (i = 0; ph[i]; i++) d->phys[i] = ph[i];
    d->phys[i] = 0;
    d->id.bustype = BUS_I8042; d->id.vendor = 0x0001; d->id.product = 0x0001; d->id.version = 0xab41;
    input_set_capability(d, EV_REP, 0);
    for (int k = 1; k < 128; k++) input_set_capability(d, EV_KEY, (uint16_t)k);
    if (input_register(d) >= 0) g_ps2kbd_ready = 1;
}

/* Active keymap name / select (console tty ioctl, `loadkeys`). */
const char* keyboard_keymap_name(void) { return keymap_active()->name; }
int keyboard_set_keymap(const char* name) {
    uint64_t fl = spin_lock_irqsave(&kbd_lock);
    int r = keymap_select(name);
    spin_unlock_irqrestore(&kbd_lock, fl);
    return r;
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
        cpu_wait_irq();  // Wait for interrupt (timer wakes ~1kHz to poll serial)
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
                cpu_wait_irq();
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
