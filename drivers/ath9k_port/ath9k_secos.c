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
#include "ani.h"      /* ath9k_ani_reset / ath9k_hw_ani_monitor / _init */

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
    if (ret == 0) ath9k_hw_ani_init(ah);   /* set up ANI state for sensitivity */
    return ret;
}

/* Probe a few PHY/MAC registers after init so the shell can show the chip state. */
unsigned int ath9k_secos_reg(unsigned int off){ return g_mmio ? *(volatile u32*)(g_mmio + off) : 0; }

/* Dump the RX-critical PHY registers + NF state for side-by-side comparison with
 * a working Alpine boot (ath9k debugfs regdump). These govern RX sensitivity:
 * TIMING5 = ANI weak-signal (CYCPWR/RSSI) thresholds, CCA = measured noise floor,
 * MC_GAIN_CTRL = antenna diversity, AGC_CONTROL = AGC state. */
void ath9k_secos_regdump(void){
    if (!g_ah){ debugcon_writestring("[regdump] run wifi up + wifi chan first\n"); return; }
    static const struct { const char* n; unsigned off; } regs[] = {
        {"SREV       0x4020", 0x4020},
        {"PHY_TIMING5 0x9810", 0x9810},  /* ANI weak-signal thresholds */
        {"PHY_EXT_CCA 0x9830", 0x9830},
        {"PHY_CCA_0  0x9e1c", 0x9e1c},   /* measured NF */
        {"PHY_CCACTL 0x9e20", 0x9e20},
        {"PHY_MCGAIN 0x9e28", 0x9e28},   /* antenna diversity */
        {"PHY_RXOCG  0xa000", 0xa000},
        {"PHY_AGCCTL 0xa2c4", 0xa2c4},   /* AGC control/state */
    };
    debugcon_writestring("[regdump] macRev="); debugcon_print_hex((unsigned long)g_ah->hw_version.macRev);
    debugcon_writestring(" rxmask="); debugcon_print_hex((unsigned long)g_ah->rxchainmask);
    debugcon_writestring(" noise="); debugcon_print_hex((unsigned long)(g_ah->noise & 0xffff));
    debugcon_writestring("\n");
    for (unsigned i = 0; i < sizeof(regs)/sizeof(regs[0]); i++){
        debugcon_writestring("  "); debugcon_writestring(regs[i].n);
        debugcon_writestring(" = "); debugcon_print_hex(*(volatile u32*)(g_mmio + regs[i].off));
        debugcon_writestring("\n");
    }
    /* NF cal history (per chain) */
    if (g_ah->caldata){
        debugcon_writestring("  nfCalHist privNF:");
        for (int c = 0; c < 3; c++){ debugcon_putchar(' ');
            debugcon_print_hex((unsigned long)(g_ah->caldata->nfCalHist[c].privNF & 0xffff)); }
        debugcon_writestring("\n");
    }
}

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
    /* Clear the status region so AR_RxDone is 0 until the hw fills it again
     * (else a recycled buffer reads stale-done and we reprocess garbage). */
    for (int k = 0; k < 64; k++) g_rxpool[i][k] = 0;
    unsigned long phys = ath9k_host_v2p(&g_rxpool[i][0]);
    ath9k_hw_addrxbuf_edma(g_ah, (u32)phys, ATH9K_RX_QUEUE_LP);
}

static unsigned g_rx_frames;   /* total descriptors completed (any frame heard) */
static int g_first_st, g_first_len, g_first_fc;   /* shape of the first frame heard */

static void prmac(const u8* m){
    for(int i=0;i<6;i++){ static const char h[]="0123456789abcdef";
        debugcon_putchar(h[(m[i]>>4)&0xF]); debugcon_putchar(h[m[i]&0xF]);
        if(i<5) debugcon_putchar(':'); }
}

/* Discovered-AP table (deduped by BSSID), shared by scan + connect. */
#define ATH_MAXAP 24
struct ath_ap { u8 bssid[6]; char ssid[33]; u8 chan; int8_t rssi; u8 privacy; };
static struct ath_ap g_aps[ATH_MAXAP];
static int g_nap;

/* Parse a beacon/probe-resp into the AP table (dedup by BSSID). Returns 1 if mgmt. */
static int collect_beacon(const u8* f, int len, int rssi){
    if (len < 36) return 0;
    u8 fc0 = f[0];                          /* mgmt: 0x80 beacon, 0x50 proberesp */
    if (fc0 != 0x80 && fc0 != 0x50) return 0;
    const u8* bssid = f + 16;
    u16 caps = f[34] | (f[35] << 8);        /* capability info (fixed param) */
    const u8* p = f + 36; const u8* end = f + len;
    char ssid[33]; int slen = 0; int chan = 0;
    while (p + 2 <= end){
        u8 tag = p[0], tlen = p[1]; const u8* val = p + 2;
        if (val + tlen > end) break;
        if (tag == 0 && slen == 0){ slen = tlen > 32 ? 32 : tlen; for(int k=0;k<slen;k++) ssid[k]=val[k]; }
        else if (tag == 3 && tlen >= 1){ chan = val[0]; }
        p = val + tlen;
    }
    ssid[slen] = 0;
    for (int i = 0; i < g_nap; i++){            /* dedup */
        int same = 1; for(int k=0;k<6;k++) if(g_aps[i].bssid[k]!=bssid[k]){ same=0; break; }
        if (same){ if(rssi > g_aps[i].rssi) g_aps[i].rssi = (int8_t)rssi; return 1; }
    }
    if (g_nap < ATH_MAXAP){
        struct ath_ap* a = &g_aps[g_nap++];
        for(int k=0;k<6;k++) a->bssid[k]=bssid[k];
        for(int k=0;k<=slen;k++) a->ssid[k]=ssid[k];
        a->chan = (u8)chan; a->rssi = (int8_t)rssi; a->privacy = (caps & 0x10) ? 1 : 0;
    }
    return 1;
}

/* Bring RX up on the current channel (idempotent per scan/connect). */
static void rx_bring_up(void){
    struct ath_common* common = ath9k_hw_common(g_ah);
    g_ah->opmode = NL80211_IFTYPE_STATION;
    common->rx_bufsize = ATH_RXBUF_SZ;
    ath9k_hw_set_rx_bufsize(g_ah, ATH_RXBUF_SZ - g_ah->caps.rx_status_len);
    ath9k_hw_rxena(g_ah);
    g_rx_idx = 0;
    for (int i = 0; i < ATH_NRXBUF; i++) rx_addbuf(i);
    ath9k_hw_setopmode(g_ah);
    ath9k_hw_setrxfilter(g_ah, ATH9K_RX_FILTER_UCAST | ATH9K_RX_FILTER_MCAST |
                               ATH9K_RX_FILTER_BCAST | ATH9K_RX_FILTER_BEACON |
                               ATH9K_RX_FILTER_PROM | ATH9K_RX_FILTER_PROBEREQ);
    ath9k_hw_startpcureceive(g_ah, 1);
}

/* Tight, SILENT RX poll for ~dwell_ms. Scans ALL buffers each round (not strict
 * FIFO order) so a buffer filled out-of-order never stalls the whole poll. */
static void rx_poll_collect(int dwell_ms){
    int rounds = dwell_ms * 4;                  /* ~250us per idle round */
    for (int spin = 0; spin < rounds; spin++){
        if ((spin & 0x3F) == 0) ath9k_hw_ani_monitor(g_ah, &g_chan);  /* periodic ANI */
        int any = 0;
        for (int i = 0; i < ATH_NRXBUF; i++){
            struct ath_rx_status rxs;
            u8* buf = &g_rxpool[i][0];
            int ret = ath9k_hw_process_rxdesc_edma(g_ah, &rxs, buf);
            if (ret == -EINPROGRESS) continue;  /* this buffer not filled yet */
            any = 1; g_rx_frames++;
            if (g_first_len == 0){               /* capture the very first frame's shape */
                const u8* fr = buf + g_ah->caps.rx_status_len;
                g_first_st = rxs.rs_status; g_first_len = rxs.rs_datalen; g_first_fc = fr[0];
            }
            if (ret == 0 && rxs.rs_status == 0 && rxs.rs_datalen >= 24)
                collect_beacon(buf + g_ah->caps.rx_status_len, rxs.rs_datalen, rxs.rs_rssi);
            rx_addbuf(i);                        /* zero status + re-queue */
        }
        if (!any) udelay(250);
    }
}

/* The AR9003 initial calibrations (IQ/ADC/NF) started by ath9k_hw_reset complete
 * ASYNCHRONOUSLY; the upstream driver polls ath9k_hw_calibrate from a timer until
 * they finish. Without that the receiver stays half-calibrated and barely decodes.
 * Drive it to completion here (longcal => also runs NF + ANI). */
static void complete_cal(void){
    for (int i = 0; i < 40; i++){
        if (ath9k_hw_calibrate(g_ah, &g_chan, g_ah->rxchainmask, 1)) break;
        udelay(10000);   /* 10 ms between cal polls */
    }
}

/* Quiet channel tune (no per-step prints) used by the sweep. */
static int tune_quiet(int chan_num){
    if (chan_num < 1 || chan_num > 14) chan_num = 1;
    int freq = (chan_num == 14) ? 2484 : (2407 + chan_num * 5);
    g_iec.band = NL80211_BAND_2GHZ; g_iec.center_freq = freq; g_iec.hw_value = chan_num;
    g_chan.chan = &g_iec; g_chan.channel = freq; g_chan.channelFlags = 0;
    g_ah->curchan = &g_chan;
    int r = ath9k_hw_reset(g_ah, &g_chan, &g_caldata, 0);
    if (r == 0){
        ath9k_hw_setpower(g_ah, ATH9K_PM_AWAKE);
        complete_cal();
        ath9k_ani_reset(g_ah, 0);   /* set sensitive default weak-signal thresholds */
    }
    return r;
}

/* Sweep channels 1..13 (re-tune + listen ~400ms each); concise summary.
 * Reports total frames heard (the key RX-health number) + the AP table. */
int ath9k_secos_scan(void){
    if (!g_ah) { debugcon_writestring("[wifi] run 'wifi up' first\n"); return -1; }
    struct ath_common* common = ath9k_hw_common(g_ah);
    common->debug_mask = 0;                 /* silence the per-channel reset spam */
    g_nap = 0; g_rx_frames = 0; g_first_len = 0; g_first_fc = 0; g_first_st = 0;
    debugcon_writestring("[wifi] scanning channels 1-13");
    for (int ch = 1; ch <= 13; ch++){
        if (tune_quiet(ch) != 0) continue;  /* tune + complete calibration */
        rx_bring_up();
        rx_poll_collect(500);
        debugcon_putchar('.');
    }
    debugcon_writestring("\n[wifi] heard "); debugcon_print_hex(g_rx_frames);
    debugcon_writestring(" frames, NF="); debugcon_print_hex((unsigned long)(g_ah->noise & 0xffff));
    debugcon_writestring(" first(fc="); debugcon_print_hex((unsigned long)g_first_fc);
    debugcon_writestring(",len="); debugcon_print_hex((unsigned long)g_first_len);
    debugcon_writestring(",st="); debugcon_print_hex((unsigned long)g_first_st);
    debugcon_writestring("), networks: "); debugcon_print_hex((unsigned long)g_nap); debugcon_writestring("\n");
    for (int i = 0; i < g_nap; i++){
        debugcon_writestring("  "); prmac(g_aps[i].bssid);
        debugcon_writestring(g_aps[i].privacy ? " [WPA] " : " [open] ");
        debugcon_writestring("ch="); debugcon_print_hex((unsigned long)g_aps[i].chan);
        debugcon_writestring(" '"); debugcon_writestring(g_aps[i].ssid); debugcon_writestring("'\n");
    }
    return g_nap;
}
