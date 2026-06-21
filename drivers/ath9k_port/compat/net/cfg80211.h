#ifndef _C_CFG80211_H
#define _C_CFG80211_H
#include <linux/types.h>
#include <linux/kernel.h>
enum nl80211_band { NL80211_BAND_2GHZ=0, NL80211_BAND_5GHZ=1, NL80211_BAND_60GHZ=2, NUM_NL80211_BANDS };
enum nl80211_iftype { NL80211_IFTYPE_UNSPECIFIED=0, NL80211_IFTYPE_STATION, NL80211_IFTYPE_AP,
                      NL80211_IFTYPE_ADHOC, NL80211_IFTYPE_MONITOR, NL80211_IFTYPE_MESH_POINT,
                      NL80211_IFTYPE_AP_VLAN, NL80211_IFTYPE_WDS, NL80211_IFTYPE_P2P_CLIENT,
                      NL80211_IFTYPE_P2P_GO, NL80211_IFTYPE_OCB, NUM_NL80211_IFTYPES };
enum ieee80211_channel_flags { IEEE80211_CHAN_DISABLED=1<<0, IEEE80211_CHAN_NO_IR=1<<1,
                               IEEE80211_CHAN_RADAR=1<<3, IEEE80211_CHAN_NO_HT40PLUS=1<<4,
                               IEEE80211_CHAN_NO_HT40MINUS=1<<5 };
struct ieee80211_channel {
    enum nl80211_band band;
    u32 center_freq; u16 hw_value; u32 flags;
    int max_antenna_gain; int max_power; int max_reg_power;
    bool beacon_found; u32 orig_flags;
    int orig_mag, orig_mpwr;
};
struct cfg80211_chan_def { struct ieee80211_channel* chan; u32 width; u32 center_freq1, center_freq2; };
struct wiphy { void* priv; };
#define IEEE80211_CHAN_MAX 64
#endif
