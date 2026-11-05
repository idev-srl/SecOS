/*
 * SecOS Kernel - Input core + evdev (/dev/input/eventN)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Every input device (PS/2 keyboard, PS/2 mouse, USB HID keyboard / mouse /
 * tablet) registers an input_dev_t and reports Linux-style events into it. Each
 * device is exposed as /dev/input/eventN with the Linux evdev ABI (struct
 * input_event, EVIOCG* ioctls, read/poll/O_NONBLOCK), so stock evdev code (the
 * LVGL evdev driver, libinput-style probing) works unchanged.
 *
 * Opening an event node requires CAP_INPUT in the opener's signed manifest
 * (reading input lets a process snoop keystrokes). Every open gets its own ring
 * buffer; overflow drops the queued events and leaves a SYN_DROPPED. EVIOCGRAB
 * gives one client exclusive delivery (and, for a keyboard, takes the keys away
 * from the console TTY).
 *
 * Kernel names use the KC_ prefix for Linux key codes (drivers/keyboard.h
 * already uses KEY_* for terminal keys); the values are Linux's.
 */
#ifndef INPUT_H
#define INPUT_H
#include <stdint.h>

/* Event types */
#define EV_SYN   0x00
#define EV_KEY   0x01
#define EV_REL   0x02
#define EV_ABS   0x03
#define EV_MSC   0x04
#define EV_REP   0x14
#define EV_CNT   0x20
/* EV_SYN codes */
#define SYN_REPORT  0
#define SYN_DROPPED 3
/* EV_REL codes */
#define REL_X       0x00
#define REL_Y       0x01
#define REL_HWHEEL  0x06
#define REL_WHEEL   0x08
#define REL_CNT     0x10
/* EV_ABS codes */
#define ABS_X       0x00
#define ABS_Y       0x01
#define ABS_CNT     0x40
/* Buttons */
#define BTN_MOUSE   0x110
#define BTN_LEFT    0x110
#define BTN_RIGHT   0x111
#define BTN_MIDDLE  0x112
#define BTN_SIDE    0x113
#define BTN_EXTRA   0x114
#define KC_MAX      0x2ff
#define KC_CNT      (KC_MAX + 1)
/* input_id.bustype */
#define BUS_USB     0x03
#define BUS_I8042   0x11
/* INPUT_PROP_* (EVIOCGPROP) */
#define INPUT_PROP_POINTER 0x00
#define INPUT_PROP_DIRECT  0x01

/* Linux key codes (KEY_* in <linux/input-event-codes.h>) used by the kernel. */
enum {
    KC_ESC = 1, KC_1, KC_2, KC_3, KC_4, KC_5, KC_6, KC_7, KC_8, KC_9, KC_0,
    KC_MINUS, KC_EQUAL, KC_BACKSPACE, KC_TAB,
    KC_Q, KC_W, KC_E, KC_R, KC_T, KC_Y, KC_U, KC_I, KC_O, KC_P,
    KC_LEFTBRACE, KC_RIGHTBRACE, KC_ENTER, KC_LEFTCTRL,
    KC_A, KC_S, KC_D, KC_F, KC_G, KC_H, KC_J, KC_K, KC_L,
    KC_SEMICOLON, KC_APOSTROPHE, KC_GRAVE, KC_LEFTSHIFT, KC_BACKSLASH,
    KC_Z, KC_X, KC_C, KC_V, KC_B, KC_N, KC_M, KC_COMMA, KC_DOT, KC_SLASH,
    KC_RIGHTSHIFT, KC_KPASTERISK, KC_LEFTALT, KC_SPACE, KC_CAPSLOCK,
    KC_F1, KC_F2, KC_F3, KC_F4, KC_F5, KC_F6, KC_F7, KC_F8, KC_F9, KC_F10,
    KC_NUMLOCK, KC_SCROLLLOCK,
    KC_KP7, KC_KP8, KC_KP9, KC_KPMINUS, KC_KP4, KC_KP5, KC_KP6, KC_KPPLUS,
    KC_KP1, KC_KP2, KC_KP3, KC_KP0, KC_KPDOT,
    KC_102ND = 86, KC_F11 = 87, KC_F12 = 88,
    KC_KPENTER = 96, KC_RIGHTCTRL = 97, KC_KPSLASH = 98, KC_SYSRQ = 99,
    KC_RIGHTALT = 100, KC_HOME = 102, KC_UP = 103, KC_PAGEUP = 104,
    KC_LEFT = 105, KC_RIGHT = 106, KC_END = 107, KC_DOWN = 108,
    KC_PAGEDOWN = 109, KC_INSERT = 110, KC_DELETE = 111,
    KC_KPEQUAL = 117, KC_PAUSE = 119, KC_LEFTMETA = 125, KC_RIGHTMETA = 126,
    KC_COMPOSE = 127,
};

/* Linux ABI structures (x86-64 layouts). */
struct k_input_event {
    int64_t  sec;       /* struct timeval tv_sec  */
    int64_t  usec;      /* struct timeval tv_usec */
    uint16_t type;
    uint16_t code;
    int32_t  value;
};
struct k_input_id { uint16_t bustype, vendor, product, version; };
struct k_input_absinfo { int32_t value, minimum, maximum, fuzz, flat, resolution; };

typedef struct input_dev {
    char     name[64];
    char     phys[32];
    struct k_input_id id;
    uint32_t evbit;                        /* EV_* bitmap (EV_CNT = 32 bits) */
    uint64_t keybit[KC_CNT / 64];
    uint32_t relbit;                       /* REL_* bitmap (REL_CNT = 16 bits) */
    uint64_t absbit;                       /* ABS_* bitmap (ABS_CNT = 64 bits) */
    uint32_t propbit;                      /* INPUT_PROP_* */
    struct k_input_absinfo absinfo[2];     /* ABS_X, ABS_Y */
    /* runtime state (owned by the input core) */
    uint64_t keystate[KC_CNT / 64];        /* keys/buttons currently down */
    int      index;                        /* N of /dev/input/eventN, -1 = not registered */
    int      pending;                      /* events since the last SYN_REPORT */
    void*    grab;                         /* client holding EVIOCGRAB, or NULL */
    void   (*poll)(void);                  /* polled backend (USB HID), may be NULL */
} input_dev_t;

/* Capability helpers (before input_register). */
void input_set_capability(input_dev_t* d, uint16_t type, uint16_t code);
void input_set_abs_params(input_dev_t* d, uint16_t axis, int32_t min, int32_t max);

/* Register a device: it becomes /dev/input/event<returned index>. -1 if full. */
int  input_register(input_dev_t* d);

/* Report one event (EV_KEY: 1 press / 0 release; a press of a key already down
 * is reported as 2 = autorepeat). input_sync() closes the packet with
 * SYN_REPORT (skipped if nothing was reported since the last one). Safe from
 * interrupt context. */
void input_event(input_dev_t* d, uint16_t type, uint16_t code, int32_t value);
void input_sync(input_dev_t* d);

/* A client holds EVIOCGRAB on this device (its events go only there). */
int  input_grabbed(const input_dev_t* d);

/* Number of registered devices; poll every polled backend (USB HID). */
int  input_count(void);
void input_poll_devices(void);

/* ---- /dev glue (fs/devfs.c, kernel/syscall.c, kernel/kiface.c) ------------- */
struct vfs_inode;
struct process;
typedef void (*input_iter_cb)(const struct vfs_inode* child, void* user);
struct vfs_inode* input_devfs_lookup(const char* rel);        /* "/input", "/input/eventN" */
int  input_devfs_readdir(const char* dir, input_iter_cb cb, void* user);
/* open(): an event node -> CAP_INPUT check (-EACCES) + a fresh per-open client
 * inode substituted into *ino. 0 = not an event node / done, <0 = -errno. */
int  input_open_hook(struct process* c, struct vfs_inode** ino, int flags);
int  input_is_client(const void* ino);                 /* a per-open event client */
void input_obj_ref(void* ino);                         /* dup/fork: one more holder */
void input_obj_unref(void* ino);                       /* close: last holder frees it */
int  input_read(void* ino, void* kbuf, int len, int nonblock); /* bytes, -2 block, -errno */
int  input_ioctl(void* ino, uint64_t cmd, uint64_t arg);       /* 0/len or -errno */
int  input_poll(void* ino);                                     /* POLLIN|POLLRDNORM or 0 */

#endif
