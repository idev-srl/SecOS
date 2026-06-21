# ath9k Linux driver port (Atheros AR9565 WiFi) — work in progress

Goal: run the Linux **ath9k** driver on SecOS largely **as-is**, via a Linux
kernel-API compatibility shim — the same method used to port GNU bash (source +
shim), and the foundation for porting other Linux drivers. The shim
(`compat/`) is **reusable**.

## Validated on the ASUS E406S (real AR9565)
- **Chip wake**: `wifi wake` runs the AR9300 power-on + RTC reset; RTC goes ON and
  the RTC/MAC/PHY register blocks come alive (were clock-gated / 0xDEADBEEF).
- **EEPROM/OTP**: the real station MAC (AzureWave OUI 80:C5:F2:…) is read from the
  on-die OTP and programmed into STA_ID — proves we can read the chip's
  calibration data (`drivers/ath9k.c`, ported from ath9k ar9003_eeprom).

## Port state
- `compat/` — ~50 Linux kernel-API header stubs (types, io, delay, slab,
  byteorder, unaligned, minimal net/cfg80211 + net/mac80211 structs, …) wired so
  `REG_READ/REG_WRITE` route through the driver's `reg_ops` to SecOS MMIO.
- ath9k_hw sources (hw.c, ar9003_hw.c, ar9003_phy.c, ar9003_calib.c,
  ar9565_1p0_initvals.h — the INI tables — etc.) compile against the shim **past
  all system-header dependencies, down to real code errors** (struct fields /
  symbols) — the system-integration boundary is solved.

## Remaining (the iterative driver grind)
1. Resolve the per-file code errors against the shim across the ~19 ath9k_hw files
   (struct ath_hw/ath_common field stubs, helper symbols).
2. Link + wire `reg_ops` to `ath9k.c`'s MMIO + SecOS heap (kzalloc -> kmalloc).
3. Call the bring-up: ath9k_hw_init -> reset -> apply INI -> PLL -> calibration ->
   set channel. Re-run `wifi diag` to confirm the PHY is configured.
4. RX/TX DMA descriptor rings (ar9003_mac).
5. Bridge to SecOS's existing 802.11 + KAT'd WPA2 supplicant for scan/associate
   (ath9k is a soft-MAC driver; we supply the 802.11 logic our supplicant already
   has, instead of porting all of mac80211).
