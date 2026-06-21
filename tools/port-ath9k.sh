#!/usr/bin/env bash
# SecOS - build the upstream Linux ath9k_hw radio driver against the SecOS
# Linux-kernel-API compat shim, producing drivers/ath9k_port/ath9k_hw.o for the
# kernel link. Same method as the bash port. SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="${ATH9K_WORK:-/tmp/ath_tree}"
B='https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/wireless/ath'
GCCINC="$(echo /usr/lib/gcc/x86_64-linux-gnu/*/include | tr ' ' '\n' | head -1)"
INC="-I$ROOT/drivers/ath9k_port/compat -I$W/ath9k -I$W -isystem $GCCINC"
CF="-ffreestanding -nostdlib -nostdinc -m64 -O2 -w -fno-pic -mcmodel=kernel -mno-red-zone -mno-sse"
HW="hw ar9003_hw ar9003_phy ar9003_calib ar9003_mac ar9003_eeprom mac calib eeprom"
INITVALS="ar9003_2p2 ar9003_buffalo ar9485 ar9340 ar9330_1p1 ar9330_1p2 ar955x_1p0 ar9580_1p0 \
          ar9462_2p0 ar9462_2p1 ar9565_1p0 ar9565_1p1 ar953x ar956x"

echo "[ath9k] 1/3 fetch sources"
mkdir -p "$W/ath9k"
for f in $HW; do [ -f "$W/ath9k/$f.c" ] || curl -s "$B/ath9k/$f.c" -o "$W/ath9k/$f.c"; done
for f in hw.h reg.h reg_mci.h reg_aic.h reg_wow.h mac.h ar9003_mac.h ar9003_phy.h eeprom.h calib.h \
         ani.h phy.h dynack.h common.h hw-ops.h ar9003_eeprom.h; do
  [ -f "$W/ath9k/$f" ] || curl -s "$B/ath9k/$f" -o "$W/ath9k/$f"; done
for f in ath.h regd.h regd_common.h; do [ -f "$W/$f" ] || curl -s "$B/$f" -o "$W/$f"; done
[ -f "$W/reg.h" ] || cp "$W/ath9k/reg.h" "$W/reg.h"
for iv in $INITVALS; do [ -s "$W/ath9k/${iv}_initvals.h" ] || curl -s "$B/ath9k/${iv}_initvals.h" -o "$W/ath9k/${iv}_initvals.h" 2>/dev/null || echo '/* stub */' > "$W/ath9k/${iv}_initvals.h"; done
# ath9k.h stub (hw.c needs it; only a few register bases)
cat > "$W/ath9k/ath9k.h" <<'EOF'
#ifndef _C_ATH9K_H
#define _C_ATH9K_H
#include "hw.h"
#ifndef AR_STA_ID0
#define AR_STA_ID0 0x8000
#define AR_STA_ID1 0x8004
#define AR_STA_ID1_SADH_MASK 0x0000ffff
#endif
#ifndef WLAN_RC_PHY_OFDM
#define WLAN_RC_PHY_CCK 0
#define WLAN_RC_PHY_OFDM 1
#endif
#endif
EOF

echo "[ath9k] 2/3 cross-compile vs SecOS shim"
OBJ=/tmp/ath9k_obj; rm -rf "$OBJ"; mkdir -p "$OBJ"
for c in $HW; do gcc $CF $INC -c "$W/ath9k/$c.c" -o "$OBJ/$c.o"; done
gcc $CF $INC -c "$ROOT/drivers/ath9k_port/ath9k_glue.c" -o "$OBJ/glue.o"
gcc $CF $INC -c "$ROOT/drivers/ath9k_port/ath9k_secos.c" -o "$OBJ/secos.o"

echo "[ath9k] 3/3 link -> drivers/ath9k_port/ath9k_hw.o"
ld -r "$OBJ"/*.o -o "$ROOT/drivers/ath9k_port/ath9k_hw.o"
echo "[ath9k] DONE: $(ls -la "$ROOT/drivers/ath9k_port/ath9k_hw.o" | awk '{print $5}') bytes, $(nm "$ROOT/drivers/ath9k_port/ath9k_hw.o" | grep -c ' T ') text symbols"
