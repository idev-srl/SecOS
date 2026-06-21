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

/* [M39] AR9300 OTP (one-time-programmable cal/EEPROM store). The AR9565 has no
 * external EEPROM chip — MAC + calibration live compressed in on-die OTP, read a
 * 32-bit word at a time through this interface (ath9k ar9003_otp.c). */
#define AR9300_OTP_BASE        0x14000
#define AR9300_OTP_STATUS      0x15f18
#define   AR9300_OTP_STATUS_TYPE   0x7
#define   AR9300_OTP_STATUS_VALID  0x4
#define AR9300_OTP_READ_DATA   0x15f1c
/* PLL/clock + reset (AR9300) */
#define AR_RTC_RESET           0x7040
#define AR_RTC_PLL_CONTROL     0x7014
#define AR_RTC_REG_CONTROL0    0x7090
#define AR_RTC_REG_CONTROL1    0x7094
#define AR_PHY_BASE            0xa000   /* baseband/PHY register block base */

ath9k_dev_t g_ath9k;

static inline uint32_t rd(uint32_t off) { return *(volatile uint32_t*)(g_ath9k.mmio + off); }
static inline void     wr(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_ath9k.mmio + off) = v; }

/* [M39] Read one 32-bit OTP word. Start the access with a dummy read of the OTP
 * data window, then poll STATUS until the type field reads VALID. Returns 1/0. */
int ath9k_otp_read_word(uint32_t addr, uint32_t* out) {
    (void)rd(AR9300_OTP_BASE + (addr * 4));            /* kick the state machine */
    for (int i = 0; i < 2000; i++) {
        uint32_t s = rd(AR9300_OTP_STATUS);
        if ((s & AR9300_OTP_STATUS_TYPE) == AR9300_OTP_STATUS_VALID) {
            *out = rd(AR9300_OTP_READ_DATA);
            return 1;
        }
        for (volatile int d = 0; d < 2000; d++) { }
    }
    return 0;
}

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

/* [M39] Full radio bring-up via the ported upstream ath9k_hw driver
 * (drivers/ath9k_port/). Wakes the chip, then hands the mapped BAR0 + devid to
 * ath9k_secos_bringup -> the real ath9k_hw_init (reset + AR9565 INI tables + PLL
 * + calibration). Returns ath9k_hw_init's status. */
int ath9k_full_bringup(void) {
    extern int ath9k_secos_bringup(volatile void*, unsigned short);
    if (!g_ath9k.present) return -1;
    ath9k_wake_reset();
    return ath9k_secos_bringup(g_ath9k.mmio, g_ath9k.device);
}

/* [M39] Raw register read, exposed for the `wifi diag` shell dump (real-HW
 * bring-up is iterative and blind without QEMU — these reads from the ASUS guide
 * the next steps: OTP layout, PLL/clock state, PHY liveness). */
uint32_t ath9k_reg_read(uint32_t off) { return g_ath9k.present ? rd(off) : 0xFFFFFFFFu; }

static void ath9k_udelay(int us) { for (volatile int i = 0; i < us * 300; i++) { } }

/* [M39] Read `count` bytes from OTP at byte `address`, going DOWNWARD (the AR9300
 * EEPROM image lives at the TOP of OTP, base 0x3ff, read backwards). Ported from
 * ath9k ar9003_read_otp. */
static int ath9k_read_otp_bytes(int address, uint8_t* buf, int count) {
    for (int i = 0; i < count; i++) {
        uint32_t data;
        if (!ath9k_otp_read_word((uint32_t)((address - i) / 4), &data)) return 0;
        buf[i] = (uint8_t)((data >> (8 * ((address - i) % 4))) & 0xff);
    }
    return 1;
}

/* [M39] Extract the station MAC from the AR9300 OTP EEPROM. The image is stored as
 * compressed blocks at the top of OTP: each block has a 4-byte header (code,
 * reference, length), `length` data bytes and a 2-byte checksum. _CompressNone
 * blocks are a full copy; _CompressBlock blocks are (offset,length,data) deltas
 * applied onto a template — the macAddr (byte offset 2 of the EEPROM struct) is
 * written by an early delta, so we recover it without the 16 KB template. Ported
 * (focused) from ath9k ar9003_eeprom_restore_internal. Returns 1 if a non-zero
 * MAC was found. */
int ath9k_read_mac_from_otp(uint8_t mac[6]) {
    static uint8_t mptr[4096];
    static uint8_t word[2048];
    for (int i = 0; i < 6; i++) mac[i] = 0;
    for (int i = 0; i < 16; i++) mptr[i] = 0;     /* only need the MAC region */

    int cptr = 0x3ff;                              /* AR9300_BASE_ADDR */
    uint8_t hdr[4];
    if (!ath9k_read_otp_bytes(cptr, hdr, 4)) return 0;
    uint32_t h = hdr[0] | (hdr[1]<<8) | (hdr[2]<<16) | ((uint32_t)hdr[3]<<24);
    if (h == 0 || h == 0xffffffff) {               /* try the 512-byte base */
        cptr = 0x1ff;
        if (!ath9k_read_otp_bytes(cptr, hdr, 4)) return 0;
        h = hdr[0] | (hdr[1]<<8) | (hdr[2]<<16) | ((uint32_t)hdr[3]<<24);
        if (h == 0 || h == 0xffffffff) return 0;
    }

    for (int it = 0; it < 100 && cptr > 4; it++) {
        if (!ath9k_read_otp_bytes(cptr, word, 4)) break;
        uint32_t w0 = word[0] | (word[1]<<8) | (word[2]<<16) | ((uint32_t)word[3]<<24);
        if (w0 == 0 || w0 == 0xffffffff) break;
        int code      = (word[0] >> 5) & 0x7;
        int length    = ((word[1] << 4) & 0x7f0) | ((word[2] >> 4) & 0xf);
        if (length >= 1024 || length > cptr) { cptr -= 4; continue; }
        if (length + 6 > (int)sizeof(word)) break;
        if (!ath9k_read_otp_bytes(cptr, word, 4 + length + 2)) break;
        if (code == 0) {                           /* _CompressNone: full copy */
            if (length >= 8) for (int j = 0; j < 6; j++) mptr[2+j] = word[4+2+j];
        } else {                                   /* _CompressBlock: apply deltas */
            int spot = 0;
            for (int t = 0; t + 2 <= length; ) {
                int off = word[4+t] & 0xff; spot += off;
                int len = word[4+t+1] & 0xff;
                if (len > 0 && spot >= 0 && spot + len <= (int)sizeof(mptr)) {
                    if (spot < 16) for (int j = 0; j < len && spot+j < 16; j++) mptr[spot+j] = word[4+t+2+j];
                } else if (len > 0) break;
                spot += len; t += len + 2;
            }
        }
        cptr -= (4 + length + 2);
    }
    for (int i = 0; i < 6; i++) mac[i] = mptr[2+i];
    return (mac[0]|mac[1]|mac[2]|mac[3]|mac[4]|mac[5]) ? 1 : 0;
}

/* [M39] AR9300 power-on wake + RTC reset. On the ASUS the chip wakes only in the
 * always-on domain (SREV/OTP respond) while RTC/MAC/PHY read 0xDEADBEEF (clock
 * gated). This runs the documented ath9k power-on sequence: force-wake, then
 * toggle the RTC out of reset, then poll RTC_STATUS for ON. Returns the final
 * RTC_STATUS (caller checks the ON bit). Low-risk: the chip is non-functional
 * until this succeeds anyway. */
#define AR_RTC_FORCE_WAKE_ON_INT_BIT 0x00000002
uint32_t ath9k_wake_reset(void) {
    /* 1. Force the chip awake (keep it awake across the reset). */
    wr(AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN | AR_RTC_FORCE_WAKE_ON_INT);
    ath9k_udelay(10);
    /* 2. Power-on reset the RTC: drop then raise AR_RTC_RESET. */
    wr(AR_RTC_RESET, 0);
    ath9k_udelay(2);
    wr(AR_RTC_RESET, 1);
    /* 3. Poll RTC_STATUS for the ON state (bit 1 of the low nibble). */
    uint32_t st = 0;
    for (int i = 0; i < 2000; i++) {
        st = rd(AR_RTC_STATUS);
        if ((st & AR_RTC_STATUS_M) == AR_RTC_STATUS_ON) break;
        ath9k_udelay(10);
    }
    /* Re-assert force-wake (some chips clear it across reset). */
    wr(AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN | AR_RTC_FORCE_WAKE_ON_INT);
    ath9k_udelay(10);
    /* Refresh the cached SREV. */
    g_ath9k.srev = rd(AR_SREV);
    /* [M39] Now that the MAC block responds, read the real station MAC from the
     * OTP EEPROM (the registers are 0 until the driver programs them) and write it
     * into STA_ID0/1, preserving the STA_ID1 flag bits. */
    uint8_t mac[6];
    if (ath9k_read_mac_from_otp(mac)) {
        wr(AR_STA_ID0, mac[0] | (mac[1]<<8) | (mac[2]<<16) | ((uint32_t)mac[3]<<24));
        wr(AR_STA_ID1, (rd(AR_STA_ID1) & 0xFFFF0000u) | mac[4] | (mac[5]<<8));
        for (int i = 0; i < 6; i++) g_ath9k.mac[i] = mac[i];
    } else {
        uint32_t id0 = rd(AR_STA_ID0), id1 = rd(AR_STA_ID1);
        g_ath9k.mac[0]=id0&0xff; g_ath9k.mac[1]=(id0>>8)&0xff; g_ath9k.mac[2]=(id0>>16)&0xff;
        g_ath9k.mac[3]=(id0>>24)&0xff; g_ath9k.mac[4]=id1&0xff; g_ath9k.mac[5]=(id1>>8)&0xff;
    }
    return st;
}
