/*
 * SecOS Kernel - [M38] Atheros ath9k (AR9300 family) WiFi driver — scaffold.
 *
 * Targets the Qualcomm Atheros AR9565 (PCI 168c:0036) found in the ASUS E406S.
 * ath9k-class chips need NO firmware blob (the MAC/baseband run from on-chip ROM),
 * which is what makes a from-scratch driver tractable at all — unlike Intel/
 * Realtek which download megabytes of proprietary firmware every boot.
 *
 * SCOPE (honest): this scaffold does PCI attach + register-space mapping + chip
 * identification (silicon revision) + MAC-address read + a guarded wake. The full
 * bring-up (AR9300 INI register tables, PLL/clock, ADC/IQ calibration, RX/TX DMA
 * rings, the 802.11 MAC, scan/associate, WPA2) is large and needs iterative
 * on-hardware debugging — tracked in docs/devlog/M38.md. The WPA2 crypto building
 * blocks (PBKDF2 + AES-CCM) are implemented and KAT-tested separately (crypto/).
 */
#ifndef SECOS_ATH9K_H
#define SECOS_ATH9K_H

#include <stdint.h>

typedef struct {
    int      present;        /* 1 if the AR9300-family device was found+mapped */
    uint16_t vendor, device; /* PCI ids */
    uint64_t mmio_phys;      /* BAR0 physical base */
    volatile uint8_t* mmio;  /* BAR0 mapped (physmap) */
    uint32_t srev;           /* AR_SREV silicon revision register */
    const char* chip;        /* human chip name */
    uint8_t  mac[6];         /* station MAC (valid only after EEPROM load) */
    int      awake;          /* 1 if AR_SREV read back a sane value */
} ath9k_dev_t;

int ath9k_init(void);                 /* probe + identify; 0 if absent, 1 if found */
const ath9k_dev_t* ath9k_get(void);   /* NULL if not present */

#endif
