/*
 * SecOS Kernel - /dev/fb0 (Linux fbdev) + console graphics handoff
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FBDEV_H
#define FBDEV_H
#include <stdint.h>
#include <stddef.h>

struct process;

/* Linux <linux/fb.h> ioctl numbers. */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY     0x4606
#define FBIOBLANK           0x4611
/* Linux <linux/kd.h> console mode. */
#define KDSETMODE   0x4B3A
#define KDGETMODE   0x4B3B
#define KD_TEXT     0x00
#define KD_GRAPHICS 0x01
/* SecOS keyboard layout selection on the console tty (no Linux equivalent:
 * Linux loads whole keymaps entry by entry with KDSKBENT). The argument is a
 * NUL-terminated layout name ("us", "it"), at most 15 characters. */
#define KDSKBLAYOUT 0x4BF0      /* set:  const char* name */
#define KDGKBLAYOUT 0x4BF1      /* get:  char name[16]   */

int  fbdev_present(void);                       /* a linear 32-bpp framebuffer exists */
uint64_t fbdev_size(void);                      /* smem_len (page aligned) */
int  fbdev_read(size_t off, void* kbuf, size_t len);
int  fbdev_write(size_t off, const void* kbuf, size_t len);
int  fbdev_ioctl(uint64_t cmd, uint64_t arg);   /* -515 = not an fb ioctl */
/* mmap of /dev/fb0 into the current process: MAP_SHARED only, never PROT_EXEC,
 * [off, off+len) inside the framebuffer. Returns the user address or -errno. */
int64_t fbdev_mmap(struct process* c, uint64_t len, int prot, int flags, uint64_t off);

/* Console handoff (KDSETMODE on the console tty). KD_GRAPHICS needs
 * CAP_DISPLAY and makes the caller the graphics owner; the text console is
 * restored by KD_TEXT or automatically when the owner exits or dies. */
int  vtcon_ioctl(uint64_t cmd, uint64_t arg);   /* -515 = not a console-mode ioctl */
void vtcon_proc_exit(struct process* p);
int  vtcon_mode(void);

#endif
