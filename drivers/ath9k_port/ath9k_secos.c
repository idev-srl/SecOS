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

/* bus ops normally supplied by the PCI layer (ath_pci.c). AR9565 has no
 * physical EEPROM (OTP only), so eeprom_read returns false -> the eeprom probe
 * fails cleanly and the restore falls through to the OTP path. */
static bool sec_eeprom_read(struct ath_common* c, u32 off, u16* data){ (void)c;(void)off;(void)data; return false; }
static void sec_read_cachesize(struct ath_common* c, int* csz){ (void)c; if(csz) *csz = 16; }
static struct ath_bus_ops g_bus_ops = {
    .ath_bus_type   = ATH_PCI,
    .eeprom_read    = sec_eeprom_read,
    .read_cachesize = sec_read_cachesize,
};
static struct ath_hw* g_ah;

/* Run the upstream ath9k_hw bring-up against the live chip. mmio = mapped BAR0,
 * devid = PCI device id (0x0036 for AR9565). Returns ath9k_hw_init's status. */
int ath9k_secos_bringup(volatile void* mmio, unsigned short devid){
    debugcon_writestring("[ath9k-port] S0 enter bringup\n");
    g_mmio = (volatile u8*)mmio;
    debugcon_writestring("[ath9k-port] S1 kzalloc ath_hw...\n");
    struct ath_hw* ah = kzalloc(sizeof(*ah), 0);
    if (!ah) { debugcon_writestring("[ath9k-port] ah alloc fail\n"); return -12; }
    debugcon_writestring("[ath9k-port] S2 ath_hw allocated\n");
    g_ah = ah;
    ah->hw_version.devid = devid;
    /* The upstream PCI probe layer (ath9k_init_softc, which we bypass) sets
     * AH_USE_EEPROM so the AR9300 eeprom restore takes the EEPROM/OTP register
     * path. Without it, ath9k_hw_use_flash() is true and the restore enters the
     * flash path -> ath9k_hw_nvram_read() on a NULL blob -> fault. AR9565 is a
     * PCI card with OTP, so set it. */
    ah->ah_flags |= AH_USE_EEPROM;   /* 0x1 */
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
    common->debug_mask = 0xffffffff;   /* narrate the bring-up on debugcon */

    debugcon_writestring("[ath9k-port] S3 calling ath9k_hw_init (upstream driver)...\n");
    int ret = ath9k_hw_init(ah);
    debugcon_writestring("[ath9k-port] ath9k_hw_init ret=");
    debugcon_print_hex((unsigned long)(long)ret);
    debugcon_writestring("\n");
    return ret;
}

/* Probe a few PHY/MAC registers after init so the shell can show the chip state. */
unsigned int ath9k_secos_reg(unsigned int off){ return g_mmio ? *(volatile u32*)(g_mmio + off) : 0; }

/* Tune the radio to a 2.4GHz channel via the upstream ath9k_hw_reset (full PHY/RF
 * reset + per-channel calibration). chan_num = 1..14. Returns ath9k_hw_reset's
 * status. This is the step that actually powers the RF onto a frequency. */
/* ath9k_hw_reset is declared in hw.h (included above). */
static struct ieee80211_channel g_iec;
static struct ath9k_channel     g_chan;
static struct ath9k_hw_cal_data g_caldata;

int ath9k_secos_tune(int chan_num){
    if (!g_ah) { debugcon_writestring("[ath9k-port] tune: chip not initialized (run wifi up)\n"); return -1; }
    if (chan_num < 1 || chan_num > 14) chan_num = 1;
    int freq = (chan_num == 14) ? 2484 : (2407 + chan_num * 5);
    g_iec.band = NL80211_BAND_2GHZ; g_iec.center_freq = freq; g_iec.hw_value = chan_num;
    g_chan.chan = &g_iec; g_chan.channel = freq; g_chan.channelFlags = 0; /* 2GHz, 20MHz */
    g_ah->curchan = &g_chan;
    debugcon_writestring("[ath9k-port] T0 ath9k_hw_reset ch=");
    debugcon_print_hex((unsigned long)chan_num);
    debugcon_writestring(" freq="); debugcon_print_hex((unsigned long)freq); debugcon_writestring("...\n");
    int r = ath9k_hw_reset(g_ah, &g_chan, &g_caldata, 0);
    debugcon_writestring("[ath9k-port] ath9k_hw_reset ret=");
    debugcon_print_hex((unsigned long)(long)r); debugcon_writestring("\n");
    return r;
}
