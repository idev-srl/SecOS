/* SecOS <linux/input-event-codes.h>: the Linux evdev event codes (values are
 * Linux's, so evdev code written for Linux compiles unchanged). Covers the
 * keyboard, mouse and tablet codes the SecOS input devices can report.
 * SPDX-License-Identifier: MIT */
#ifndef _SECOS_INPUT_EVENT_CODES_H
#define _SECOS_INPUT_EVENT_CODES_H

#define INPUT_PROP_POINTER   0x00
#define INPUT_PROP_DIRECT    0x01
#define INPUT_PROP_MAX       0x1f
#define INPUT_PROP_CNT       (INPUT_PROP_MAX + 1)

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_MSC 0x04
#define EV_SW  0x05
#define EV_LED 0x11
#define EV_SND 0x12
#define EV_REP 0x14
#define EV_FF  0x15
#define EV_MAX 0x1f
#define EV_CNT (EV_MAX + 1)

#define SYN_REPORT    0
#define SYN_CONFIG    1
#define SYN_MT_REPORT 2
#define SYN_DROPPED   3

#define KEY_RESERVED   0
#define KEY_ESC        1
#define KEY_1          2
#define KEY_2          3
#define KEY_3          4
#define KEY_4          5
#define KEY_5          6
#define KEY_6          7
#define KEY_7          8
#define KEY_8          9
#define KEY_9          10
#define KEY_0          11
#define KEY_MINUS      12
#define KEY_EQUAL      13
#define KEY_BACKSPACE  14
#define KEY_TAB        15
#define KEY_Q          16
#define KEY_W          17
#define KEY_E          18
#define KEY_R          19
#define KEY_T          20
#define KEY_Y          21
#define KEY_U          22
#define KEY_I          23
#define KEY_O          24
#define KEY_P          25
#define KEY_LEFTBRACE  26
#define KEY_RIGHTBRACE 27
#define KEY_ENTER      28
#define KEY_LEFTCTRL   29
#define KEY_A          30
#define KEY_S          31
#define KEY_D          32
#define KEY_F          33
#define KEY_G          34
#define KEY_H          35
#define KEY_J          36
#define KEY_K          37
#define KEY_L          38
#define KEY_SEMICOLON  39
#define KEY_APOSTROPHE 40
#define KEY_GRAVE      41
#define KEY_LEFTSHIFT  42
#define KEY_BACKSLASH  43
#define KEY_Z          44
#define KEY_X          45
#define KEY_C          46
#define KEY_V          47
#define KEY_B          48
#define KEY_N          49
#define KEY_M          50
#define KEY_COMMA      51
#define KEY_DOT        52
#define KEY_SLASH      53
#define KEY_RIGHTSHIFT 54
#define KEY_KPASTERISK 55
#define KEY_LEFTALT    56
#define KEY_SPACE      57
#define KEY_CAPSLOCK   58
#define KEY_F1         59
#define KEY_F2         60
#define KEY_F3         61
#define KEY_F4         62
#define KEY_F5         63
#define KEY_F6         64
#define KEY_F7         65
#define KEY_F8         66
#define KEY_F9         67
#define KEY_F10        68
#define KEY_NUMLOCK    69
#define KEY_SCROLLLOCK 70
#define KEY_KP7        71
#define KEY_KP8        72
#define KEY_KP9        73
#define KEY_KPMINUS    74
#define KEY_KP4        75
#define KEY_KP5        76
#define KEY_KP6        77
#define KEY_KPPLUS     78
#define KEY_KP1        79
#define KEY_KP2        80
#define KEY_KP3        81
#define KEY_KP0        82
#define KEY_KPDOT      83
#define KEY_102ND      86
#define KEY_F11        87
#define KEY_F12        88
#define KEY_KPENTER    96
#define KEY_RIGHTCTRL  97
#define KEY_KPSLASH    98
#define KEY_SYSRQ      99
#define KEY_RIGHTALT   100
#define KEY_LINEFEED   101
#define KEY_HOME       102
#define KEY_UP         103
#define KEY_PAGEUP     104
#define KEY_LEFT       105
#define KEY_RIGHT      106
#define KEY_END        107
#define KEY_DOWN       108
#define KEY_PAGEDOWN   109
#define KEY_INSERT     110
#define KEY_DELETE     111
#define KEY_POWER      116
#define KEY_KPEQUAL    117
#define KEY_PAUSE      119
#define KEY_LEFTMETA   125
#define KEY_RIGHTMETA  126
#define KEY_COMPOSE    127
#define KEY_MAX        0x2ff
#define KEY_CNT        (KEY_MAX + 1)

#define BTN_MISC       0x100
#define BTN_0          0x100
#define BTN_MOUSE      0x110
#define BTN_LEFT       0x110
#define BTN_RIGHT      0x111
#define BTN_MIDDLE     0x112
#define BTN_SIDE       0x113
#define BTN_EXTRA      0x114
#define BTN_FORWARD    0x115
#define BTN_BACK       0x116
#define BTN_TASK       0x117
#define BTN_TOOL_PEN   0x140
#define BTN_TOUCH      0x14a
#define BTN_STYLUS     0x14b

#define REL_X       0x00
#define REL_Y       0x01
#define REL_Z       0x02
#define REL_HWHEEL  0x06
#define REL_DIAL    0x07
#define REL_WHEEL   0x08
#define REL_MISC    0x09
#define REL_MAX     0x0f
#define REL_CNT     (REL_MAX + 1)

#define ABS_X          0x00
#define ABS_Y          0x01
#define ABS_Z          0x02
#define ABS_PRESSURE   0x18
#define ABS_MT_SLOT        0x2f
#define ABS_MT_POSITION_X  0x35
#define ABS_MT_POSITION_Y  0x36
#define ABS_MT_TRACKING_ID 0x39
#define ABS_MAX        0x3f
#define ABS_CNT        (ABS_MAX + 1)

#define MSC_SCAN    0x04
#define REP_DELAY   0x00
#define REP_PERIOD  0x01

#endif
