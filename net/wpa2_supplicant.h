/*
 * SecOS - [M39] WPA2-PSK 4-way handshake supplicant (EAPOL-Key state machine).
 *
 * Drives the security half of associating to a WPA2 network on top of the M38/M39
 * crypto (PMK, PTK/PRF, RFC 3394 GTK unwrap, HMAC-SHA1 MIC). Transport-agnostic:
 * the radio (ath9k) feeds received EAPOL-Key frames in and sends the produced
 * replies out. Validated end-to-end at boot by a synthetic handshake (a stub
 * authenticator drives the supplicant through msg1..msg4), so the whole
 * key-exchange logic is proven without a radio in the loop.
 * SPDX-License-Identifier: MIT
 */
#ifndef SECOS_WPA2_SUPPLICANT_H
#define SECOS_WPA2_SUPPLICANT_H
#include <stdint.h>
#include <stddef.h>

typedef enum {
    WPA2_SM_IDLE = 0,
    WPA2_SM_PTK_START,   /* got msg1, sent msg2, awaiting msg3 */
    WPA2_SM_DONE,        /* got msg3 (GTK installed), sent msg4 */
    WPA2_SM_FAILED
} wpa2_sm_state_t;

typedef struct {
    uint8_t  pmk[32];
    uint8_t  sta_mac[6];   /* supplicant (our) MAC */
    uint8_t  ap_mac[6];    /* authenticator (AP) MAC */
    uint8_t  snonce[32];
    uint8_t  anonce[32];
    uint8_t  ptk[48];      /* KCK(16) | KEK(16) | TK(16) */
    uint8_t  gtk[32];      /* group key recovered from msg3 */
    int      gtk_len;
    uint8_t  replay[8];
    wpa2_sm_state_t state;
} wpa2_supplicant_t;

/* Initialise the supplicant with the PMK (from wpa2_pmk) and the two MACs. */
void wpa2_sm_init(wpa2_supplicant_t* sm, const uint8_t pmk[32],
                  const uint8_t sta_mac[6], const uint8_t ap_mac[6]);

/* Feed one received EAPOL-Key frame. If a reply must be sent, it is written to
 * out (capacity out_cap) and *out_len is set; otherwise *out_len = 0. Returns the
 * new state. The supplicant's snonce is generated from `entropy` on msg1. */
wpa2_sm_state_t wpa2_sm_rx(wpa2_supplicant_t* sm, const uint8_t* frame, size_t len,
                           uint64_t entropy, uint8_t* out, size_t out_cap, size_t* out_len);

/* Synthetic full-handshake KAT (acts as both AP and STA). Returns 1 on pass. */
int wpa2_supplicant_selftest(void);

#endif
