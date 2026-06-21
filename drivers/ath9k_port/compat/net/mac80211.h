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
#endif
