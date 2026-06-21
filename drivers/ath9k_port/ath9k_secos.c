/*
 * SecOS - integration bridge between the ported ath9k_hw driver and SecOS.
 * Compiled WITH the Linux-compat shim (so it sees struct ath_hw), it sets up the
 * driver's reg_ops to route MMIO through the SecOS-mapped BAR0, allocates +
 * minimally populates struct ath_hw, and calls the upstream ath9k_hw_init bring-up.
 * Exposes ath9k_secos_bringup() as a plain C entry point the SecOS kernel
 * (drivers/ath9k.c, `wifi up`) calls. SPDX-License-Identifier: MIT
 */
#include "hw.h"
#include <linux/slab.h>

extern void debugcon_writestring(const char*);
extern void debugcon_print_hex(unsigned long);

static volatile u8* g_mmio;   /* SecOS-mapped BAR0 (physmap VA) */

static unsigned int sec_read(void* p, u32 reg){ (void)p; return *(volatile u32*)(g_mmio + reg); }
static void sec_write(void* p, u32 val, u32 reg){ (void)p; *(volatile u32*)(g_mmio + reg) = val; }
static u32 sec_rmw(void* p, u32 reg, u32 set, u32 clr){ (void)p;
    u32 v = *(volatile u32*)(g_mmio + reg); v &= ~clr; v |= set; *(volatile u32*)(g_mmio + reg) = v; return v; }
static void sec_multi_read(void* p, u32* addr, u32* val, u16 cnt){ (void)p;
    for (u16 i = 0; i < cnt; i++) val[i] = *(volatile u32*)(g_mmio + addr[i]); }
static void sec_noop(void* p){ (void)p; }

static struct ath_bus_ops g_bus_ops = { .ath_bus_type = ATH_PCI };
static struct ath_hw* g_ah;

/* Run the upstream ath9k_hw bring-up against the live chip. mmio = mapped BAR0,
 * devid = PCI device id (0x0036 for AR9565). Returns ath9k_hw_init's status. */
int ath9k_secos_bringup(volatile void* mmio, unsigned short devid){
    g_mmio = (volatile u8*)mmio;
    struct ath_hw* ah = kzalloc(sizeof(*ah), 0);
    if (!ah) { debugcon_writestring("[ath9k-port] ah alloc fail\n"); return -12; }
    g_ah = ah;
    ah->hw_version.devid = devid;
    ah->reg_ops.read        = sec_read;
    ah->reg_ops.write       = sec_write;
    ah->reg_ops.rmw         = sec_rmw;
    ah->reg_ops.multi_read  = sec_multi_read;
    ah->reg_ops.enable_write_buffer = sec_noop;
    ah->reg_ops.write_flush         = sec_noop;
    ah->reg_ops.enable_rmw_buffer   = sec_noop;
    ah->reg_ops.rmw_flush           = sec_noop;

    struct ath_common* common = ath9k_hw_common(ah);
    common->bus_ops = &g_bus_ops;
    common->ah = ah;

    debugcon_writestring("[ath9k-port] calling ath9k_hw_init (upstream driver)...\n");
    int ret = ath9k_hw_init(ah);
    debugcon_writestring("[ath9k-port] ath9k_hw_init ret=");
    debugcon_print_hex((unsigned long)(long)ret);
    debugcon_writestring("\n");
    return ret;
}

/* Probe a few PHY/MAC registers after init so the shell can show the chip state. */
unsigned int ath9k_secos_reg(unsigned int off){ return g_mmio ? *(volatile u32*)(g_mmio + off) : 0; }
