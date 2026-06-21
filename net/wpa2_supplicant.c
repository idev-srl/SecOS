/*
 * SecOS - [M39] WPA2-PSK 4-way handshake supplicant. See wpa2_supplicant.h.
 * SPDX-License-Identifier: MIT
 */
#include "wpa2_supplicant.h"
#include "../crypto/wpa2.h"
#include "../crypto/wpa2_eapol.h"
#include "debugcon.h"

static void scpy(void* d, const void* s, size_t n){ uint8_t* a=d; const uint8_t* b=s; while(n--) *a++=*b++; }
static void sset(void* d, int v, size_t n){ uint8_t* a=d; while(n--) *a++=(uint8_t)v; }
static int  scmp(const void* a, const void* b, size_t n){ const uint8_t* x=a,*y=b; for(size_t i=0;i<n;i++) if(x[i]!=y[i]) return (int)x[i]-(int)y[i]; return 0; }

/* EAPOL-Key frame layout (offsets from the EAPOL version byte). */
#define E_VERSION   0
#define E_TYPE      1     /* 3 = EAPOL-Key */
#define E_BODYLEN   2     /* be16 */
#define E_DESCTYPE  4     /* 2 = RSN */
#define E_KEYINFO   5     /* be16 */
#define E_KEYLEN    7     /* be16 */
#define E_REPLAY    9     /* 8 */
#define E_NONCE     17    /* 32 */
#define E_IV        49    /* 16 */
#define E_RSC       65    /* 8 */
#define E_KEYID     73    /* 8 */
#define E_MIC       81    /* 16 */
#define E_DATALEN   97    /* be16 */
#define E_DATA      99
#define EAPOL_HDR_LEN E_DATA

/* key_info bits */
#define KI_VERSION_SHA1 0x0002
#define KI_PAIRWISE     0x0008
#define KI_INSTALL      0x0040
#define KI_ACK          0x0080
#define KI_MIC          0x0100
#define KI_SECURE       0x0200
#define KI_ENCRYPTED    0x1000

static uint16_t be16(const uint8_t* p){ return (uint16_t)((p[0]<<8)|p[1]); }
static void wbe16(uint8_t* p, uint16_t v){ p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }

void wpa2_sm_init(wpa2_supplicant_t* sm, const uint8_t pmk[32],
                  const uint8_t sta_mac[6], const uint8_t ap_mac[6]) {
    sset(sm, 0, sizeof(*sm));
    scpy(sm->pmk, pmk, 32);
    scpy(sm->sta_mac, sta_mac, 6);
    scpy(sm->ap_mac, ap_mac, 6);
    sm->state = WPA2_SM_IDLE;
}

/* Build an EAPOL-Key reply: header + nonce + key_data, then MIC over the whole
 * frame (MIC field zeroed during the computation) with KCK. */
static size_t build_reply(const wpa2_supplicant_t* sm, uint16_t key_info,
                          const uint8_t* nonce, const uint8_t* key_data, uint16_t kd_len,
                          uint8_t* out) {
    size_t total = EAPOL_HDR_LEN + kd_len;
    sset(out, 0, total);
    out[E_VERSION] = 2;            /* 802.1X-2004 */
    out[E_TYPE]    = 3;            /* EAPOL-Key */
    wbe16(out + E_BODYLEN, (uint16_t)(total - 4));
    out[E_DESCTYPE] = 2;          /* RSN */
    wbe16(out + E_KEYINFO, key_info);
    wbe16(out + E_KEYLEN, 16);
    scpy(out + E_REPLAY, sm->replay, 8);
    if (nonce) scpy(out + E_NONCE, nonce, 32);
    wbe16(out + E_DATALEN, kd_len);
    if (kd_len) scpy(out + E_DATA, key_data, kd_len);
    if (key_info & KI_MIC) {
        uint8_t mic[16];
        sset(out + E_MIC, 0, 16);
        wpa2_eapol_mic(sm->ptk /*KCK*/, out, total, mic);
        scpy(out + E_MIC, mic, 16);
    }
    return total;
}

wpa2_sm_state_t wpa2_sm_rx(wpa2_supplicant_t* sm, const uint8_t* frame, size_t len,
                           uint64_t entropy, uint8_t* out, size_t out_cap, size_t* out_len) {
    *out_len = 0;
    if (len < EAPOL_HDR_LEN || frame[E_TYPE] != 3) return sm->state;
    uint16_t ki = be16(frame + E_KEYINFO);
    scpy(sm->replay, frame + E_REPLAY, 8);

    /* msg1: ACK set, MIC clear, pairwise -> derive PTK, reply msg2. */
    if ((ki & KI_ACK) && !(ki & KI_MIC) && (ki & KI_PAIRWISE)) {
        scpy(sm->anonce, frame + E_NONCE, 32);
        /* SNonce from caller-provided entropy (radio supplies a real RNG seed). */
        for (int i = 0; i < 32; i++) {
            entropy ^= entropy << 13; entropy ^= entropy >> 7; entropy ^= entropy << 17;
            sm->snonce[i] = (uint8_t)(entropy ^ (i * 0x9eULL));
        }
        wpa2_derive_ptk(sm->pmk, sm->ap_mac, sm->sta_mac, sm->anonce, sm->snonce, sm->ptk, 48);
        /* msg2: SNonce + (minimal) RSN IE as key_data, MIC. */
        static const uint8_t rsn_ie[] = {
            0x30,0x14,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x04,
            0x01,0x00,0x00,0x0f,0xac,0x02,0x00,0x00 };
        size_t total = build_reply(sm, KI_VERSION_SHA1|KI_PAIRWISE|KI_MIC,
                                   sm->snonce, rsn_ie, sizeof(rsn_ie), out);
        if (total > out_cap) { sm->state = WPA2_SM_FAILED; return sm->state; }
        *out_len = total;
        sm->state = WPA2_SM_PTK_START;
        return sm->state;
    }

    /* msg3: ACK+MIC+SECURE+INSTALL, encrypted key_data (wrapped GTK) -> verify MIC,
     * unwrap GTK with KEK, reply msg4. */
    if ((ki & KI_ACK) && (ki & KI_MIC) && (ki & KI_INSTALL) && sm->state == WPA2_SM_PTK_START) {
        /* Verify the MIC (zero the MIC field, HMAC-SHA1(KCK)). */
        uint8_t tmp[256]; if (len > sizeof(tmp)) { sm->state = WPA2_SM_FAILED; return sm->state; }
        scpy(tmp, frame, len);
        uint8_t rx_mic[16]; scpy(rx_mic, tmp + E_MIC, 16);
        sset(tmp + E_MIC, 0, 16);
        uint8_t calc[16]; wpa2_eapol_mic(sm->ptk, tmp, len, calc);
        if (scmp(calc, rx_mic, 16) != 0) { sm->state = WPA2_SM_FAILED; return sm->state; }
        /* Unwrap the GTK KDE from the encrypted key_data with the KEK (ptk+16). */
        uint16_t kdl = be16(frame + E_DATALEN);
        if (kdl >= 16 && (kdl % 8) == 0 && (size_t)(E_DATA + kdl) <= len) {
            uint8_t plain[64];
            int nblk = (kdl / 8) - 1;
            if (nblk >= 1 && nblk <= 8 &&
                aes_key_unwrap(sm->ptk + 16, frame + E_DATA, nblk, plain)) {
                /* plain = GTK KDE: dd len 00 0f ac 01 keyid.. GTK ...  (skip 8-byte hdr) */
                int glen = nblk * 8;
                int hdr = (plain[0] == 0xdd) ? 8 : 0;   /* KDE header if present */
                if (glen - hdr > 0 && glen - hdr <= 32) {
                    sm->gtk_len = glen - hdr;
                    scpy(sm->gtk, plain + hdr, sm->gtk_len);
                }
            }
        }
        size_t total = build_reply(sm, KI_VERSION_SHA1|KI_PAIRWISE|KI_MIC|KI_SECURE,
                                   0, 0, 0, out);
        if (total > out_cap) { sm->state = WPA2_SM_FAILED; return sm->state; }
        *out_len = total;
        sm->state = WPA2_SM_DONE;
        return sm->state;
    }
    return sm->state;
}

/* --- Synthetic full-handshake KAT: a stub authenticator drives the supplicant
 * through msg1..msg4 and we check the supplicant derived the AP's PTK and
 * recovered the GTK. This validates the whole key-exchange end to end. --- */
int wpa2_supplicant_selftest(void) {
    uint8_t pmk[32];
    wpa2_pmk("password", "IEEE", pmk);                 /* reuse the M38 PMK vector */
    static const uint8_t ap_mac[6]  = {0x00,0x11,0x22,0x33,0x44,0x55};
    static const uint8_t sta_mac[6] = {0x66,0x77,0x88,0x99,0xaa,0xbb};

    wpa2_supplicant_t sm;
    wpa2_sm_init(&sm, pmk, sta_mac, ap_mac);

    /* Authenticator builds msg1 (ANonce, ACK, no MIC). */
    uint8_t anonce[32]; for (int i = 0; i < 32; i++) anonce[i] = (uint8_t)(0xA0 + i);
    uint8_t msg1[EAPOL_HDR_LEN]; sset(msg1, 0, sizeof(msg1));
    msg1[E_VERSION]=2; msg1[E_TYPE]=3; wbe16(msg1+E_BODYLEN, EAPOL_HDR_LEN-4);
    msg1[E_DESCTYPE]=2; wbe16(msg1+E_KEYINFO, KI_VERSION_SHA1|KI_PAIRWISE|KI_ACK);
    wbe16(msg1+E_KEYLEN,16); uint8_t rc[8]={0,0,0,0,0,0,0,1}; scpy(msg1+E_REPLAY, rc, 8);
    scpy(msg1+E_NONCE, anonce, 32);

    uint8_t reply[256]; size_t rlen = 0;
    wpa2_sm_rx(&sm, msg1, sizeof(msg1), 0x0123456789abcdefULL, reply, sizeof(reply), &rlen);
    int ok2 = (sm.state == WPA2_SM_PTK_START && rlen > 0);

    /* Authenticator recomputes the PTK from the SNonce it received in msg2. */
    uint8_t snonce[32]; scpy(snonce, reply + E_NONCE, 32);
    uint8_t ap_ptk[48];
    wpa2_derive_ptk(pmk, ap_mac, sta_mac, anonce, snonce, ap_ptk, 48);
    int ptk_match = (scmp(ap_ptk, sm.ptk, 48) == 0);

    /* Verify msg2's MIC the way the AP would (proves the supplicant's KCK). */
    uint8_t m2[256]; scpy(m2, reply, rlen);
    uint8_t m2_mic[16]; scpy(m2_mic, m2 + E_MIC, 16); sset(m2 + E_MIC, 0, 16);
    uint8_t m2_calc[16]; wpa2_eapol_mic(ap_ptk, m2, rlen, m2_calc);
    int mic2 = (scmp(m2_mic, m2_calc, 16) == 0);

    /* Authenticator builds msg3: wrap a known GTK under the KEK (ap_ptk+16). */
    uint8_t gtk[16]; for (int i = 0; i < 16; i++) gtk[i] = (uint8_t)(0xC0 + i);
    uint8_t kde[24]; sset(kde,0,sizeof(kde));
    kde[0]=0xdd; kde[1]=0x16; kde[2]=0x00; kde[3]=0x0f; kde[4]=0xac; kde[5]=0x01; /* GTK KDE hdr */
    scpy(kde+8, gtk, 16);                      /* 8-byte hdr + 16 GTK = 24 = 3 blocks */
    uint8_t wrapped[32];                        /* unwrap n=3 -> in (n+1)*8 = 32 */
    aes_key_wrap(ap_ptk + 16, kde, 3, wrapped);

    uint8_t msg3[EAPOL_HDR_LEN + 32]; sset(msg3, 0, sizeof(msg3));
    msg3[E_VERSION]=2; msg3[E_TYPE]=3; wbe16(msg3+E_BODYLEN, sizeof(msg3)-4);
    msg3[E_DESCTYPE]=2;
    wbe16(msg3+E_KEYINFO, KI_VERSION_SHA1|KI_PAIRWISE|KI_INSTALL|KI_ACK|KI_MIC|KI_SECURE|KI_ENCRYPTED);
    wbe16(msg3+E_KEYLEN,16); uint8_t rc3[8]={0,0,0,0,0,0,0,2}; scpy(msg3+E_REPLAY, rc3, 8);
    scpy(msg3+E_NONCE, anonce, 32);
    wbe16(msg3+E_DATALEN, 32); scpy(msg3+E_DATA, wrapped, 32);
    uint8_t mic3[16]; wpa2_eapol_mic(ap_ptk, msg3, sizeof(msg3), mic3); scpy(msg3+E_MIC, mic3, 16);

    wpa2_sm_rx(&sm, msg3, sizeof(msg3), 0, reply, sizeof(reply), &rlen);
    int ok4 = (sm.state == WPA2_SM_DONE && rlen > 0);
    int gtk_ok = (sm.gtk_len == 16 && scmp(sm.gtk, gtk, 16) == 0);

    int pass = ok2 && ptk_match && mic2 && ok4 && gtk_ok;
    debugcon_writestring(pass ? "[WPA2] 4-way handshake supplicant KAT PASS (PTK match, GTK unwrapped)\n"
                              : "[WPA2] 4-way handshake supplicant KAT FAIL\n");
    return pass;
}
