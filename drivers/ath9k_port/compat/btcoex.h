#ifndef _C_BTCOEX_H
#define _C_BTCOEX_H
#include <linux/types.h>
struct ath_btcoex_hw { int enabled; u32 bt_coex_mode, bt_coex_mode2, bt_coex_mode3; u8 scheme; };
enum ath_btcoex_scheme { ATH_BTCOEX_CFG_NONE, ATH_BTCOEX_CFG_2WIRE, ATH_BTCOEX_CFG_3WIRE, ATH_BTCOEX_CFG_MCI };
#endif
