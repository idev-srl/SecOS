/*
 * SecOS Kernel - [M38] Atheros ath9k (AR9300 family) WiFi driver — scaffold.
 * See ath9k.h for scope. Probe + identify + MAC read; full bring-up is TODO.
 */
#include "ath9k.h"
#include "pci.h"
#include "debugcon.h"
#include "../mm/vmm.h"

/* --- AR9300 register offsets (subset; from the open ath9k driver) --- */
#define AR_SREV            0x4020   /* silicon revision */
#define AR_STA_ID0         0x8000   /* MAC addr [31:0] */
#define AR_STA_ID1         0x8004   /* MAC addr [47:32] + flags */
#define AR_RTC_RC          0x7000   /* RTC reset control */
#define AR_RTC_FORCE_WAKE  0x7110   /* AR9300: force the chip awake */
#define   AR_RTC_FORCE_WAKE_EN   0x00000001
#define   AR_RTC_FORCE_WAKE_ON_INT 0x00000002
#define AR_RTC_STATUS      0x7044   /* AR9300 RTC status */
#define   AR_RTC_STATUS_M        0x0000000f
#define   AR_RTC_STATUS_ON       0x00000002

/* AR_SREV value -> chip name. The version is in bits [11:4]/[3:0]. */
#define AR_SREV_VERSION(s)   (((s) & 0xFF0) >> 4)
#define AR_SREV_VER_AR9565   0x2C0   /* >>4 = 0x2C */

ath9k_dev_t g_ath9k;

static inline uint32_t rd(uint32_t off) { return *(volatile uint32_t*)(g_ath9k.mmio + off); }
static inline void     wr(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_ath9k.mmio + off) = v; }

/* AR9300-family PCI device ids (vendor 0x168c). */
static const struct { uint16_t dev; const char* name; } k_chips[] = {
    { 0x0030, "AR9380" }, { 0x0032, "AR9485" }, { 0x0033, "AR9580" },
    { 0x0034, "AR9462" }, { 0x0036, "AR9565" }, { 0x0037, "AR1111" },
    { 0, 0 }
};

int ath9k_init(void) {
    pci_device_t pd;
    const char* name = 0;
    int i;
    for (i = 0; k_chips[i].dev; i++) {
        if (pci_find(0x168c, k_chips[i].dev, &pd) == 0) { name = k_chips[i].name; break; }
    }
    if (!name) return 0;   /* no AR9300-family WiFi present */

    g_ath9k.present = 1;
    g_ath9k.vendor = pd.vendor_id;
    g_ath9k.device = pd.device_id;
    g_ath9k.chip   = name;

    /* Map BAR0 (the register space) through the physmap and enable MMIO + bus
     * master. AR9300 BAR0 is a 32-bit memory BAR. */
    pci_enable_mem_and_busmaster(&pd);
    uint64_t bar = pci_bar_mem(&pd, 0);
    if (!bar) { debugcon_writestring("[ath9k] BAR0 unmapped\n"); g_ath9k.present = 0; return 0; }
    g_ath9k.mmio_phys = bar;
    g_ath9k.mmio = (volatile uint8_t*)phys_to_virt(bar);

    debugcon_writestring("[ath9k] found "); debugcon_writestring(name);
    debugcon_writestring(" (168c:"); debugcon_print_hex(pd.device_id);
    debugcon_writestring(") BAR0="); debugcon_print_hex(bar); debugcon_writestring("\n");

    /* Guarded wake: ask the RTC to force the chip awake so register reads are
     * valid. This is a single documented write; if the chip is already awake
     * (BIOS left it on) it is harmless. */
    wr(AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN | AR_RTC_FORCE_WAKE_ON_INT);
    for (volatile int d = 0; d < 100000; d++) { } /* brief settle */

    /* Identify the silicon revision. A sane (non 0/0xFFFFFFFF) value means the
     * register space responds and the chip is awake. */
    g_ath9k.srev = rd(AR_SREV);
    g_ath9k.awake = (g_ath9k.srev != 0 && g_ath9k.srev != 0xFFFFFFFF);
    debugcon_writestring("[ath9k] AR_SREV="); debugcon_print_hex(g_ath9k.srev);
    debugcon_writestring(g_ath9k.awake ? " (chip responds)\n" : " (no response — chip asleep/uninit)\n");

    /* Read the station MAC. On AR9300 this is loaded from EEPROM/OTP during init,
     * which the scaffold does not yet do — so it may read back zero/all-ones on a
     * cold chip. We report whatever the registers hold. */
    if (g_ath9k.awake) {
        uint32_t id0 = rd(AR_STA_ID0);
        uint32_t id1 = rd(AR_STA_ID1);
        g_ath9k.mac[0] = id0 & 0xff; g_ath9k.mac[1] = (id0 >> 8) & 0xff;
        g_ath9k.mac[2] = (id0 >> 16) & 0xff; g_ath9k.mac[3] = (id0 >> 24) & 0xff;
        g_ath9k.mac[4] = id1 & 0xff; g_ath9k.mac[5] = (id1 >> 8) & 0xff;
    }

    /* TODO (real-HW iterative): AR9300 INI register tables -> PLL/clock ->
     * reset MAC/baseband -> EEPROM/OTP parse (MAC + calibration + regulatory) ->
     * ADC/DC-offset/IQ calibration -> RX/TX DMA descriptor rings -> 802.11 MAC ->
     * scan (set channel, collect beacons) -> associate -> WPA2 (PBKDF2+CCMP, the
     * crypto is already implemented in crypto/wpa2.c). See docs/devlog/M38.md. */
    return 1;
}

const ath9k_dev_t* ath9k_get(void) { return g_ath9k.present ? &g_ath9k : 0; }
