/*
 * bkl.c — the kernel lock (M48) on top of klock (M50). See bkl.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "bkl.h"
#include "klock.h"
#include "debugcon.h"

klock_t g_bkl = KLOCK_INIT_RECURSIVE(LK_K0_BKL, "bkl");

/* [M50] A task spinning for the BKL (interrupts on) polls the USB HID
 * keyboard, at most once a millisecond system-wide. The console reader is the
 * only regular poller (keyboard_has_char / keyboard_getc_raw), and it must
 * win the BKL first; with fork, exec (signature check) and disk syscalls
 * holding the BKL for 10-30 ms back to back, the poll gap grew past the
 * ~30 ms a key stays pressed, and QEMU's usb-kbd (like a real keyboard
 * report) then never shows that key: keystrokes were lost under load (M50 B3
 * gate). usb_hid_poll() only trylocks the HID and xHCI locks, so it never
 * waits here; nothing is held while it runs (the BKL is the outermost lock). */
extern void usb_hid_poll(void);
extern uint64_t tsc_read(void);
extern uint64_t tsc_hz(void);
static volatile uint64_t g_bkl_poll_tsc;
void bkl_wait_poll(void) {
    uint64_t hz = tsc_hz(), now = tsc_read(), last = g_bkl_poll_tsc;
    if (!hz || now - last < hz / 1000) return;
    if (!__sync_bool_compare_and_swap(&g_bkl_poll_tsc, last, now)) return;   /* another CPU polls */
    usb_hid_poll();
}

void bkl_lock(void)            { klock_lock(&g_bkl); }
void bkl_lock_enable_irq(void) { klock_lock_irq_on(&g_bkl); }
void bkl_unlock(void)          { klock_unlock(&g_bkl); }

uint32_t bkl_suspend(void)          { return klock_suspend(&g_bkl); }
void     bkl_resume(uint32_t depth) { klock_resume(&g_bkl, depth); }
void     bkl_drop_for(struct process* p) { klock_drop_owner(&g_bkl, p); }
int      bkl_held_by(struct process* p)  { return klock_owned_by(&g_bkl, p); }

/* ---- [M50] mode + per-syscall mask ---- */
int g_bkl_mode = 1;

/* Every syscall still needs the BKL: phase D of the BKL removal clears bits
 * here one subsystem at a time, once that subsystem has its own locks. */
#define ALL64 0xFFFFFFFFFFFFFFFFULL
uint64_t syscall_bkl_mask[SYSCALL_BKL_WORDS] = { ALL64, ALL64, ALL64, ALL64 };
typedef char syscall_bkl_mask_init_covers_all[(SYSCALL_BKL_WORDS == 4) ? 1 : -1];

void bkl_init_mode(void) {
    extern int kernel_cmdline_has(const char* word);
    if (kernel_cmdline_has("bkl=off")) g_bkl_mode = 0;
    else g_bkl_mode = 1;                 /* bkl=on, or nothing: the default */
    if (!g_bkl_mode) {
        debugcon_writestring("[BKL] WARNING: bkl=off requested. The BKL removal is not "
                             "complete: syscalls in syscall_bkl_mask (");
        debugcon_print_hex(syscall_bkl_mask_count());
        debugcon_writestring(" of 0x100) still take the kernel lock, as do faults, "
                             "teardown and the kernel shell.\n");
    } else {
        debugcon_writestring("[BKL] kernel lock mode: bkl=on\n");
    }
}

int syscall_needs_bkl(uint64_t num) {
    if (g_bkl_mode || num >= SYSCALL_BKL_NR) return 1;
    return (int)((syscall_bkl_mask[num >> 6] >> (num & 63)) & 1);
}

uint32_t syscall_bkl_mask_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < SYSCALL_BKL_WORDS; i++)
        for (uint64_t w = syscall_bkl_mask[i]; w; w &= w - 1) n++;
    return n;
}

int bkl_syscall_enter(uint64_t num) {
    if (!syscall_needs_bkl(num)) return 0;
    klock_lock_irq_on(&g_bkl);
    return 1;
}

void bkl_syscall_exit(int taken) {
    if (taken) klock_unlock_to_user(&g_bkl);    /* unlock + check in one step */
    else klock_check_user_return("syscall exit");
}
