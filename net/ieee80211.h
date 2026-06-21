/*
 * SecOS - [M38] IEEE 802.11 frame structures (for the ath9k WiFi path).
 * Minimal subset needed to scan (parse beacons) and associate. The radio
 * bring-up that fills/sends these is M38 TODO; see docs/devlog/M38.md.
 */
#ifndef SECOS_IEEE80211_H
#define SECOS_IEEE80211_H
#include <stdint.h>

/* Frame Control field types/subtypes. */
#define IEEE80211_FTYPE_MGMT   0x00
#define IEEE80211_FTYPE_CTL    0x04
#define IEEE80211_FTYPE_DATA   0x08
#define IEEE80211_STYPE_ASSOC_REQ   0x00
#define IEEE80211_STYPE_ASSOC_RESP  0x10
#define IEEE80211_STYPE_PROBE_REQ   0x40
#define IEEE80211_STYPE_PROBE_RESP  0x50
#define IEEE80211_STYPE_BEACON      0x80
#define IEEE80211_STYPE_AUTH        0xB0
#define IEEE80211_STYPE_DEAUTH      0xC0

/* Generic 802.11 MAC header (3-address; data frames use addr4 in WDS). */
typedef struct __attribute__((packed)) {
    uint16_t frame_control;
    uint16_t duration_id;
    uint8_t  addr1[6];   /* RA / DA */
    uint8_t  addr2[6];   /* TA / SA */
    uint8_t  addr3[6];   /* BSSID */
    uint16_t seq_ctrl;
} ieee80211_hdr_t;

/* Beacon/probe-response fixed parameters (followed by tagged IEs). */
typedef struct __attribute__((packed)) {
    uint64_t timestamp;
    uint16_t beacon_interval;
    uint16_t capability;
} ieee80211_beacon_fixed_t;

/* Tagged information element header. */
typedef struct __attribute__((packed)) {
    uint8_t id;          /* 0=SSID, 1=rates, 3=DS(channel), 48=RSN(WPA2), ... */
    uint8_t len;
} ieee80211_ie_t;

#define IEEE80211_IE_SSID    0
#define IEEE80211_IE_RATES   1
#define IEEE80211_IE_DS      3
#define IEEE80211_IE_RSN     48   /* WPA2 RSN information element */

/* A discovered access point (one scan result). */
typedef struct {
    uint8_t  bssid[6];
    char     ssid[33];
    uint8_t  channel;
    int8_t   rssi;
    uint16_t capability;
    int      rsn;        /* 1 if it advertises WPA2/RSN */
} wifi_ap_t;

#endif
