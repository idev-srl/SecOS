/* SecOS <linux/kd.h>: console mode switching on the console tty, plus the
 * SecOS keyboard-layout requests (no Linux equivalent: Linux loads whole
 * keymaps entry by entry with KDSKBENT).
 *   ioctl(tty, KDSETMODE, (void*)KD_GRAPHICS)  needs CAP_DISPLAY; the text
 *   console stops drawing until KD_TEXT or until the caller exits / crashes.
 *   ioctl(tty, KDSKBLAYOUT, "it")  selects a layout ("us", "it").
 * SPDX-License-Identifier: MIT */
#ifndef _SECOS_LINUX_KD_H
#define _SECOS_LINUX_KD_H

#define KDSETMODE   0x4B3A
#define KDGETMODE   0x4B3B
#define KD_TEXT     0x00
#define KD_GRAPHICS 0x01

#define KDSKBLAYOUT 0x4BF0      /* const char* name (<= 15 chars) */
#define KDGKBLAYOUT 0x4BF1      /* char name[16] */

#endif
