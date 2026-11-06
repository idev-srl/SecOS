/*
 * SecOS Kernel - /dev/fb0 (Linux fbdev) + console graphics handoff
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * /dev/fb0 exposes the boot framebuffer (GRUB/Multiboot2 or UEFI GOP, linear,
 * 32 bpp) with the Linux fbdev ABI: FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO,
 * FBIOPUT_VSCREENINFO (only the current mode is accepted: there is no mode
 * setting), FBIOPAN_DISPLAY at offset 0, FBIOBLANK (accepted, no-op), read/
 * write at a byte offset, and mmap.
 *
 * mmap maps exactly the framebuffer's physical pages into the caller: a
 * VMA_TYPE_PHYS region, user RW (or RO), NX, MAP_SHARED, Write-Combining. The
 * PTEs carry VMM_FLAG_DEVICE so unmap/exit/fork never hand these frames to
 * the PMM (they are not PMM-owned) and never COW them. Opening the node needs
 * CAP_DISPLAY in the signed manifest (fs/devfs.c checks it at open()).
 *
 * Console handoff: KDSETMODE(KD_GRAPHICS) on the console tty stops the
 * framebuffer console from drawing (the vt still records output in its grid);
 * KD_TEXT, or the owner's exit / crash (vtcon_proc_exit, called from the exit
 * path), repaints the text console. A kernel panic re-initialises the console,
 * which always returns to text mode.
 */
#include "fbdev.h"
#include "fb.h"
#include "fb_console.h"
#include "cap.h"
#include "process.h"
#include "sched.h"
#include "debugcon.h"
#include "kverbose.h"
#include "vmm.h"
#include "vma.h"
#include "../mm/user_copy.h"

#define E_PERM   (-1)
#define E_NOMEM  (-12)
#define E_ACCES  (-13)
#define E_FAULT  (-14)
#define E_BUSY   (-16)
#define E_NODEV  (-19)
#define E_INVAL  (-22)
#define NOIOCTL  (-515)

#define PROT_READ_   0x1
#define PROT_WRITE_  0x2
#define PROT_EXEC_   0x4
#define MAP_SHARED_  0x01
#define MAP_PRIVATE_ 0x02
#define MAP_FIXED_   0x10
#define MAP_ANON_    0x20

/* Linux struct fb_var_screeninfo / fb_fix_screeninfo (x86-64 layout). */
struct k_fb_bitfield { uint32_t offset, length, msb_right; };
struct k_fb_var {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    struct k_fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate, colorspace;
    uint32_t reserved[4];
};
struct k_fb_fix {
    char     id[16];
    uint64_t smem_start;
    uint32_t smem_len, type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len, accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};
_Static_assert(sizeof(struct k_fb_var) == 160, "fb_var_screeninfo ABI");
_Static_assert(sizeof(struct k_fb_fix) == 80, "fb_fix_screeninfo ABI");

static int fb_info(framebuffer_info_t* fi) {
#if ENABLE_FB
    if (!fb_get_info(fi)) return 0;
    if (fi->bpp != 32 || fi->type != 1 || !fi->pitch || !fi->width || !fi->height) return 0;
    return 1;
#else
    (void)fi; return 0;
#endif
}

int fbdev_present(void) { framebuffer_info_t fi; return fb_info(&fi); }

static uint64_t raw_size(const framebuffer_info_t* fi) { return (uint64_t)fi->pitch * fi->height; }
uint64_t fbdev_size(void) {
    framebuffer_info_t fi;
    if (!fb_info(&fi)) return 0;
    return (raw_size(&fi) + 0xFFFULL) & ~0xFFFULL;
}

static uint8_t* fb_kva(const framebuffer_info_t* fi) {
    return (uint8_t*)(uintptr_t)(fi->virt_addr ? fi->virt_addr : fi->addr);
}

int fbdev_read(size_t off, void* kbuf, size_t len) {
    framebuffer_info_t fi;
    if (!fb_info(&fi)) return E_NODEV;
    uint64_t sz = raw_size(&fi);
    if (off >= sz) return 0;
    if (len > sz - off) len = (size_t)(sz - off);
    const volatile uint8_t* src = fb_kva(&fi) + off;
    uint8_t* d = (uint8_t*)kbuf;
    for (size_t i = 0; i < len; i++) d[i] = src[i];
    return (int)len;
}

int fbdev_write(size_t off, const void* kbuf, size_t len) {
    framebuffer_info_t fi;
    if (!fb_info(&fi)) return E_NODEV;
    uint64_t sz = raw_size(&fi);
    if (off >= sz) return -28;                     /* -ENOSPC, as Linux fb_write */
    if (len > sz - off) len = (size_t)(sz - off);
    volatile uint8_t* dst = fb_kva(&fi) + off;
    const uint8_t* s = (const uint8_t*)kbuf;
    for (size_t i = 0; i < len; i++) dst[i] = s[i];
    return (int)len;
}

static void fill_var(const framebuffer_info_t* fi, struct k_fb_var* v) {
    uint8_t* p = (uint8_t*)v;
    for (unsigned i = 0; i < sizeof(*v); i++) p[i] = 0;
    v->xres = v->xres_virtual = fi->width;
    v->yres = v->yres_virtual = fi->height;
    v->bits_per_pixel = fi->bpp;
    v->red.offset = fi->red_mask_pos;     v->red.length = fi->red_mask_size;
    v->green.offset = fi->green_mask_pos; v->green.length = fi->green_mask_size;
    v->blue.offset = fi->blue_mask_pos;   v->blue.length = fi->blue_mask_size;
    v->transp.offset = 0; v->transp.length = 0;    /* XRGB8888: no alpha */
    v->height = v->width = 0xFFFFFFFFu;            /* physical size unknown (-1) */
    v->pixclock = 0; v->vmode = 0;
}

static void fill_fix(const framebuffer_info_t* fi, struct k_fb_fix* f) {
    uint8_t* p = (uint8_t*)f;
    for (unsigned i = 0; i < sizeof(*f); i++) p[i] = 0;
    const char* id = "SecOS FB";
    for (int i = 0; id[i]; i++) f->id[i] = id[i];
    f->smem_start = 0;              /* the physical address is not exposed (mmap offset 0) */
    f->smem_len = (uint32_t)((raw_size(fi) + 0xFFFULL) & ~0xFFFULL);
    f->type = 0;                    /* FB_TYPE_PACKED_PIXELS */
    f->visual = 2;                  /* FB_VISUAL_TRUECOLOR */
    f->line_length = fi->pitch;
    f->accel = 0;                   /* FB_ACCEL_NONE */
}

int fbdev_ioctl(uint64_t cmd, uint64_t arg) {
    framebuffer_info_t fi;
    if (!fb_info(&fi)) return E_NODEV;
    switch (cmd) {
    case FBIOGET_VSCREENINFO: {
        struct k_fb_var v; fill_var(&fi, &v);
        return copy_to_user((void*)arg, &v, sizeof(v)) ? E_FAULT : 0;
    }
    case FBIOGET_FSCREENINFO: {
        struct k_fb_fix f; fill_fix(&fi, &f);
        return copy_to_user((void*)arg, &f, sizeof(f)) ? E_FAULT : 0;
    }
    case FBIOPUT_VSCREENINFO: {
        /* No mode setting: accept a request for exactly the current mode (as
         * fbdev clients often re-set what they read), refuse anything else. */
        struct k_fb_var want, cur;
        if (copy_from_user(&want, (const void*)arg, sizeof(want))) return E_FAULT;
        fill_var(&fi, &cur);
        if (want.xres != cur.xres || want.yres != cur.yres ||
            (want.xres_virtual && want.xres_virtual != cur.xres_virtual) ||
            (want.yres_virtual && want.yres_virtual != cur.yres_virtual) ||
            want.xoffset || want.yoffset ||
            (want.bits_per_pixel && want.bits_per_pixel != cur.bits_per_pixel))
            return E_INVAL;
        return copy_to_user((void*)arg, &cur, sizeof(cur)) ? E_FAULT : 0;
    }
    case FBIOPAN_DISPLAY: {
        struct k_fb_var want;
        if (copy_from_user(&want, (const void*)arg, sizeof(want))) return E_FAULT;
        return (want.xoffset || want.yoffset) ? E_INVAL : 0;
    }
    case FBIOBLANK:
        return 0;
    default:
        return NOIOCTL;
    }
}

int64_t fbdev_mmap(struct process* cp, uint64_t len, int prot, int flags, uint64_t off) {
    process_t* c = (process_t*)cp;
    framebuffer_info_t fi;
    if (!c || !fb_info(&fi)) return E_NODEV;
    /* A device mapping is shared by nature (private COW of VRAM makes no sense)
     * and never executable. */
    if (!(flags & MAP_SHARED_) || (flags & MAP_PRIVATE_) || (flags & MAP_ANON_)) return E_INVAL;
    if (prot & PROT_EXEC_) return E_ACCES;
    if (!(prot & (PROT_READ_ | PROT_WRITE_))) return E_INVAL;
    if ((fi.addr & 0xFFF) || (off & 0xFFF) || len == 0) return E_INVAL;
    uint64_t size = (raw_size(&fi) + 0xFFFULL) & ~0xFFFULL;
    len = (len + 0xFFFULL) & ~0xFFFULL;
    if (len == 0 || off >= size || len > size - off) return E_INVAL;   /* beyond the framebuffer */

    uint64_t base = c->mmap_next;
    if (base >= USER_MMAP_END || len > USER_MMAP_END - base) return E_NOMEM;
    if (c->mem_limit && vma_total_bytes(&c->vmas) + len > c->mem_limit) return E_NOMEM;
    uint64_t vf = VMM_FLAG_USER | VMM_FLAG_NOEXEC | ((prot & PROT_WRITE_) ? VMM_FLAG_RW : 0);
    if (vma_add_phys(&c->vmas, base, base + len, vf, fi.addr + off) != 0) return E_NOMEM;
    c->mmap_next = base + len;
    /* Map it eagerly (the #PF path would also fault pages in one by one).
     * PAT slot 1 = Write-Combining for the PWT-tagged PTEs. */
    extern void vmm_pat_init(void);
    vmm_pat_init();
    const vma_t* v = vma_find(&c->vmas, base);
    for (uint64_t va = base; v && va < base + len; va += 0x1000) {
        int r = vma_fault_in(c->space, v, va);
        if (r != 0 && r != -3) break;              /* -3: already mapped */
    }
    if (g_kverbose) {
        debugcon_writestring("[FB] mmap pid="); debugcon_print_hex(c->pid);
        debugcon_writestring(" va="); debugcon_print_hex(base);
        debugcon_writestring(" len="); debugcon_print_hex(len); debugcon_writestring("\n");
    }
    return (int64_t)base;
}

/* ---- console mode (KDSETMODE) ------------------------------------------------------ */
static int g_kd_mode = KD_TEXT;
static uint32_t g_kd_owner;          /* pid that switched to KD_GRAPHICS */

int vtcon_mode(void) { return g_kd_mode; }

static void set_text(const char* why, uint32_t pid) {
    if (g_kd_mode == KD_TEXT) return;
    g_kd_mode = KD_TEXT; g_kd_owner = 0;
#if ENABLE_FB
    fb_console_set_graphics(0);
#endif
    debugcon_writestring("[VT] text console restored (");
    debugcon_writestring(why);
    debugcon_writestring(" pid="); debugcon_print_hex(pid);
    debugcon_writestring(")\n");
}

int vtcon_ioctl(uint64_t cmd, uint64_t arg) {
    process_t* c = sched_get_current();
    switch (cmd) {
    case KDGETMODE: {
        int m = g_kd_mode;
        return copy_to_user((void*)arg, &m, sizeof(m)) ? E_FAULT : 0;
    }
    case KDSETMODE:
        if (arg == KD_GRAPHICS) {
            /* Blanking the console is a display privilege. */
            if (!cap_check_explicit(c, CAP_DISPLAY, 53 /*SYS_IOCTL*/, "KD_GRAPHICS")) return E_PERM;
            uint32_t pid = c ? c->pid : 0;
            if (g_kd_mode == KD_GRAPHICS && g_kd_owner == pid) return 0;
            g_kd_mode = KD_GRAPHICS; g_kd_owner = pid;
#if ENABLE_FB
            fb_console_set_graphics(1);
#endif
            debugcon_writestring("[VT] graphics mode (owner pid="); debugcon_print_hex(pid);
            debugcon_writestring(")\n");
            return 0;
        }
        if (arg == KD_TEXT) {
            /* Only the graphics owner (or another display-privileged program)
             * may take the screen back: any console user forcing text mode
             * would scribble the console over a compositor. */
            uint32_t pid = c ? c->pid : 0;
            if (g_kd_mode == KD_GRAPHICS && pid != g_kd_owner &&
                !cap_check_explicit(c, CAP_DISPLAY, 53 /*SYS_IOCTL*/, "KD_TEXT")) return E_PERM;
            set_text("KD_TEXT", pid);
            return 0;
        }
        return E_INVAL;
    default:
        return NOIOCTL;
    }
}

void vtcon_proc_exit(struct process* pp) {
    process_t* p = (process_t*)pp;
    if (!p || g_kd_mode != KD_GRAPHICS || p->pid != g_kd_owner) return;
    set_text("graphics owner exited", p->pid);
}
