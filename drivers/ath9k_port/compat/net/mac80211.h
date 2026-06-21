#ifndef _C_MAC80211_H
#define _C_MAC80211_H
#include <linux/types.h>
#include <linux/kernel.h>
#include <net/cfg80211.h>
struct ieee80211_supported_band {
    struct ieee80211_channel* channels; int n_channels;
    void* bitrates; int n_bitrates; enum nl80211_band band;
};
struct ieee80211_conf { struct cfg80211_chan_def chandef; int power_level; u32 flags; };
struct ieee80211_hw { struct ieee80211_conf conf; struct wiphy* wiphy; void* priv; };
struct ieee80211_channel; struct ieee80211_vif; struct ieee80211_sta; struct sk_buff;
#define IEEE80211_HT_CAP_SM_PS 0x000C
#define RX_ENC_FLAG_SHORT_GI 0x4
#define RX_ENC_FLAG_40MHZ 0x8
#define RX_ENC_FLAG_SHORTPRE 0x2
enum mac80211_rx_encoding { RX_ENC_LEGACY=0, RX_ENC_HT, RX_ENC_VHT, RX_ENC_HE };
#define RX_ENC_FLAG_STBC_SHIFT 6
#define RX_ENC_FLAG_STBC_MASK (3<<6)
#define RX_ENC_FLAG_LDPC 0x10
#endif
