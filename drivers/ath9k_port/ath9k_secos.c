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
#include <linux/delay.h>
#include <linux/errno.h>
#include "mac.h"
#include "hw-ops.h"   /* static-inline ath9k_hw_rxena() etc. */

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

/* ---------------------------------------------------------------------------
 * RX scan (AR9300 EDMA). Sets the chip to receive on the current channel and
 * polls the EDMA RX FIFO for 802.11 management frames (beacons / probe resp),
 * extracting SSID + BSSID. This replaces the recv.c ring management (not ported)
 * with a minimal poll loop. DMA buffer phys addrs come from the SecOS side.
 * ------------------------------------------------------------------------- */
#define ATH_NRXBUF   24
#define ATH_RXBUF_SZ 2048
static u8  g_rxpool[ATH_NRXBUF][ATH_RXBUF_SZ] __attribute__((aligned(256)));
static int g_rx_idx;
extern unsigned long ath9k_host_v2p(void* v);   /* SecOS kvirt_to_phys */

static void rx_addbuf(int i){
    unsigned long phys = ath9k_host_v2p(&g_rxpool[i][0]);
    ath9k_hw_addrxbuf_edma(g_ah, (u32)phys, ATH9K_RX_QUEUE_LP);
}

static void prmac(const u8* m){
    for(int i=0;i<6;i++){ static const char h[]="0123456789abcdef";
        debugcon_putchar(h[(m[i]>>4)&0xF]); debugcon_putchar(h[m[i]&0xF]);
        if(i<5) debugcon_putchar(':'); }
}

/* Parse a beacon/probe-resp; returns 1 if it was a mgmt beacon/probe-resp. */
static int parse_beacon(const u8* f, int len, int rssi){
    if (len < 36) return 0;
    u8 fc0 = f[0];                          /* type=mgmt(00); subtype 8=beacon, 5=proberesp */
    if (fc0 != 0x80 && fc0 != 0x50) return 0;
    const u8* bssid = f + 16;
    const u8* p = f + 36;                    /* 24 hdr + 12 fixed (ts/interval/caps) */
    const u8* end = f + len;
    char ssid[33]; int slen = 0; int chan = 0;
    while (p + 2 <= end){
        u8 tag = p[0], tlen = p[1]; const u8* val = p + 2;
        if (val + tlen > end) break;
        if (tag == 0 && slen == 0){ slen = tlen > 32 ? 32 : tlen; for(int k=0;k<slen;k++) ssid[k]=val[k]; }
        else if (tag == 3 && tlen >= 1){ chan = val[0]; }   /* DS param: channel */
        p = val + tlen;
    }
    ssid[slen] = 0;
    debugcon_writestring("  SSID '"); debugcon_writestring(ssid);
    debugcon_writestring("'  bssid "); prmac(bssid);
    debugcon_writestring("  ch="); debugcon_print_hex((unsigned long)chan);
    debugcon_writestring("  rssi="); debugcon_print_hex((unsigned long)(rssi & 0xff));
    debugcon_writestring("\n");
    return 1;
}

/* Listen on the current channel for ~2s, print beacons seen. Returns count. */
int ath9k_secos_scan(void){
    if (!g_ah) { debugcon_writestring("[ath9k-port] scan: run wifi up + wifi chan first\n"); return -1; }
    struct ath_common* common = ath9k_hw_common(g_ah);
    g_ah->opmode = NL80211_IFTYPE_STATION;
    common->rx_bufsize = ATH_RXBUF_SZ;

    debugcon_writestring("[ath9k-port] S: set_rx_bufsize\n");
    ath9k_hw_set_rx_bufsize(g_ah, ATH_RXBUF_SZ - g_ah->caps.rx_status_len);
    debugcon_writestring("[ath9k-port] S: rxena\n");
    ath9k_hw_rxena(g_ah);
    debugcon_writestring("[ath9k-port] S: addrxbuf\n");
    g_rx_idx = 0;
    for (int i = 0; i < ATH_NRXBUF; i++) rx_addbuf(i);
    debugcon_writestring("[ath9k-port] S: setopmode\n");
    ath9k_hw_setopmode(g_ah);
    debugcon_writestring("[ath9k-port] S: setrxfilter\n");
    ath9k_hw_setrxfilter(g_ah, ATH9K_RX_FILTER_UCAST | ATH9K_RX_FILTER_MCAST |
                               ATH9K_RX_FILTER_BCAST | ATH9K_RX_FILTER_BEACON |
                               ATH9K_RX_FILTER_PROM | ATH9K_RX_FILTER_PROBEREQ);
    debugcon_writestring("[ath9k-port] S: startpcureceive\n");
    ath9k_hw_startpcureceive(g_ah, 1);

    /* --- diagnostics: is RX actually armed? --- */
    #define RD(off) (*(volatile u32*)(g_mmio + (off)))
    debugcon_writestring("[diag] buf0 phys="); debugcon_print_hex(ath9k_host_v2p(&g_rxpool[0][0]));
    debugcon_writestring(" rxchainmask="); debugcon_print_hex((unsigned long)g_ah->rxchainmask);
    debugcon_writestring(" rxstatuslen="); debugcon_print_hex((unsigned long)g_ah->caps.rx_status_len);
    debugcon_writestring("\n[diag] AR_CR="); debugcon_print_hex(RD(0x0008));
    debugcon_writestring(" DIAG_SW="); debugcon_print_hex(RD(0x8048));
    debugcon_writestring(" RXFILT="); debugcon_print_hex(RD(0x803C));
    debugcon_writestring(" LP_RXDP="); debugcon_print_hex(RD(0x0078));
    debugcon_writestring("\n[ath9k-port] RX live, listening ~2s...\n");

    int found = 0, n_done = 0, n_inval = 0;
    for (int spin = 0; spin < 40000 && found < 64; spin++){
        struct ath_rx_status rxs;
        u8* buf = &g_rxpool[g_rx_idx][0];
        int ret = ath9k_hw_process_rxdesc_edma(g_ah, &rxs, buf);
        if (ret == -EINPROGRESS){ udelay(50); continue; }   /* not filled yet */
        if (ret == 0){ n_done++;
            if (rxs.rs_status == 0 && rxs.rs_datalen >= 24)
                found += parse_beacon(buf + g_ah->caps.rx_status_len, rxs.rs_datalen, rxs.rs_rssi);
        } else { n_inval++; }
        rx_addbuf(g_rx_idx);                                /* recycle + advance */
        g_rx_idx = (g_rx_idx + 1) % ATH_NRXBUF;
    }
    debugcon_writestring("[diag] done="); debugcon_print_hex((unsigned long)n_done);
    debugcon_writestring(" inval="); debugcon_print_hex((unsigned long)n_inval);
    debugcon_writestring(" matched="); debugcon_print_hex((unsigned long)found);
    debugcon_writestring("\n[diag] buf0[0..15]:");
    for(int k=0;k<16;k++){ debugcon_putchar(' '); static const char h[]="0123456789abcdef";
        debugcon_putchar(h[(g_rxpool[0][k]>>4)&0xF]); debugcon_putchar(h[g_rxpool[0][k]&0xF]); }
    debugcon_writestring("\n");
    return found;
}
