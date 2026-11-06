#!/usr/bin/env bash
# SecOS - reproducible Dropbear SSH port. Fetches a pinned Dropbear release,
# verifies its SHA-256, configures it with a hand-written config.h +
# localoptions.h (multi-user: public-key or /etc/shadow password auth, root by
# key only; ed25519, curve25519/mlkem768,
# chacha20-poly1305/aes-gcm/aes-ctr, hmac-sha2, no forwarding), CROSS-compiles
# dropbearmulti (dropbear + dropbearkey + dbclient) against SecOS's freestanding
# libc (-nostdinc + user/dropbear-include + user/port-include stubs +
# user/dropbear_port.h force include + user/dropbear_compat.c shims), links it
# with the CAP_NET manifest and user/libcrypt.a (crypt(), musl; built by
# `make libcrypt`), signs it and embeds it as
# crypto/user_dropbear_elf.h. See docs/SSH.md.  SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DV=2026.94
DSHA=e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81
TARBALL=dropbear-$DV.tar.bz2
URL="https://matt.ucc.asn.au/dropbear/releases/$TARBALL"
WORK="${DROPBEAR_PORT_WORK:-/tmp/dropbear-$DV}"
OBJ="${DROPBEAR_PORT_OBJ:-/tmp/secos-dropbearobj}"
PROGS="dropbear dropbearkey dbclient"

GCCINC="$(echo /usr/lib/gcc/x86_64-linux-gnu/*/include | tr ' ' '\n' | head -1)"
DFLAGS=(-ffreestanding -nostdlib -nostdinc -fno-pie -no-pie -mno-red-zone -mcmodel=large -m64 -O2
        -ffunction-sections -fdata-sections -fno-builtin-printf
        ${DROPBEAR_PORT_WFLAGS:--Wno-discarded-qualifiers -Werror=implicit-function-declaration -Werror=int-conversion -Werror=int-to-pointer-cast}
        -U__linux__ -U__linux -Ulinux -U__gnu_linux__
        -DHAVE_CONFIG_H -DLOCALOPTIONS_H_EXISTS -DDROPBEAR_MULTI -DDROPBEAR_SERVER -DDROPBEAR_CLIENT
        -I"$WORK" -I"$WORK/src" -I"$WORK/libtomcrypt/src/headers" -I"$WORK/libtommath"
        -I"$ROOT/user/dropbear-include" -I"$ROOT/user/port-include" -I"$ROOT/user" -I"$ROOT/user/include"
        -isystem "$GCCINC" -include "$ROOT/user/dropbear_port.h" ${DROPBEAR_PORT_CFLAGS:-})
for p in $PROGS; do DFLAGS+=("-DDBMULTI_$p"); done

echo "[port-dropbear] 1/5 fetch + verify $TARBALL"
if [ ! -f "$WORK/.secos-extracted" ]; then
    DL="$(dirname "$WORK")"; mkdir -p "$DL"
    [ -f "$DL/$TARBALL" ] || wget -q -O "$DL/$TARBALL" "$URL"
    echo "$DSHA  $DL/$TARBALL" | sha256sum -c --quiet - \
        || { echo "[port-dropbear] SHA-256 MISMATCH for $TARBALL - refusing to build"; exit 1; }
    rm -rf "$WORK"; ( cd "$DL" && tar xjf "$TARBALL" )
    [ "$DL/dropbear-$DV" = "$WORK" ] || mv "$DL/dropbear-$DV" "$WORK"
    touch "$WORK/.secos-extracted"
fi

echo "[port-dropbear] 2/5 hand-written config.h + localoptions.h (no host configure)"
cp "$ROOT/user/dropbear_config.h" "$WORK/config.h"
cp "$ROOT/user/dropbear_localoptions.h" "$WORK/localoptions.h"
{ printf '/* Generated from src/default_options.h by tools/port-dropbear.sh */\n\n'
  sh "$WORK/src/ifndef_wrapper.sh" < "$WORK/src/default_options.h"; } > "$WORK/default_options_guard.h"
# SecOS policy: no password login for root, whatever the command line says
# (upstream's -g made the default). Root logs in with a key; other accounts
# with a key or their /etc/shadow password.
python3 -I - "$WORK/src/svr-runopts.c" <<'PY'
import sys
f = sys.argv[1]; s = open(f).read()
old = "\tsvr_opts.norootpass = 0;"
if old in s:
    s = s.replace(old, "\tsvr_opts.norootpass = 1; /* SecOS: root password login refused (always -g) */", 1)
    open(f, 'w').write(s)
elif "SecOS: root password login refused" not in s:
    sys.exit("[port-dropbear] svr-runopts.c: norootpass default not found")
PY

# Object lists straight from the upstream makefiles (no configure needed).
mkvar() { python3 -I - "$1" "$2" <<'PY'
import re, sys
s = open(sys.argv[1]).read().replace('\\\n', ' ')
for name in sys.argv[2].split(','):
    m = re.search(r'^' + re.escape(name) + r'\s*=(.*)$', s, re.M)
    print(m.group(1) if m else '')
PY
}
DB_OBJS="$(mkvar "$WORK/Makefile.in" _COMMONOBJS,_CLISVROBJS,_SVROBJS,_CLIOBJS,_KEYOBJS | tr '\n' ' ') dbmulti.o"
# aes_enc.o is an ENCRYPT_ONLY variant of aes.c; radix_to_bin needs the generic
# math descriptor's mp_* names. Neither is used by Dropbear.
LTC_OBJS="$(mkvar "$WORK/libtomcrypt/Makefile.in" OBJECTS | tr ' ' '\n' | grep -vE 'aes_enc\.o|radix_to_bin\.o' | tr '\n' ' ')"
LTM_OBJS="$(mkvar "$WORK/libtommath/Makefile.in" OBJECTS)"

echo "[port-dropbear] 3/5 cross-compile against SecOS libc"
rm -rf "$OBJ"; mkdir -p "$OBJ"/{db,ltc,ltm}
fail=0
cc1() { gcc "${DFLAGS[@]}" "$@" || fail=1; }
for o in $DB_OBJS;  do c="$WORK/src/${o%.o}.c";        cc1 -c "$c" -o "$OBJ/db/$(basename "$o")"; done
for o in $LTC_OBJS; do c="$WORK/libtomcrypt/${o%.o}.c"; cc1 -c "$c" -o "$OBJ/ltc/$(echo "${o%.o}" | tr / _).o"; done
for o in $LTM_OBJS; do c="$WORK/libtommath/${o%.o}.c";  cc1 -c "$c" -o "$OBJ/ltm/$(basename "$o")"; done
[ "$fail" = 0 ] || { echo "[port-dropbear] compile FAILED"; exit 1; }

echo "[port-dropbear] 4/5 compile SecOS shims + link"
gcc "${DFLAGS[@]}" -c "$ROOT/user/dropbear_compat.c" -o "$OBJ/zz_compat.o"
gcc -ffreestanding -nostdlib -fno-pie -no-pie -mno-red-zone -mcmodel=large -m64 -c "$ROOT/user/setjmp.S" -o "$OBJ/zz_setjmp.o"
gcc -ffreestanding -nostdlib -fno-pie -no-pie -mno-red-zone -mcmodel=large -m64 -c "$ROOT/user/note_dropbear.S" -o "$OBJ/zz_note.o"
rm -f "$OBJ"/libtom*.a; ar rcs "$OBJ/libtomcrypt.a" "$OBJ"/ltc/*.o; ar rcs "$OBJ/libtommath.a" "$OBJ"/ltm/*.o
ld --gc-sections -T "$ROOT/user/user.ld" -o "$ROOT/user/dropbearmulti.elf" \
   "$ROOT/user/crt0.o" "$OBJ/zz_note.o" "$OBJ"/db/*.o "$OBJ"/zz_compat.o "$OBJ"/zz_setjmp.o \
   "$OBJ/libtomcrypt.a" "$OBJ/libtommath.a" \
   "$ROOT/user/libsecos.o" "$ROOT/user/libc.o" "$ROOT/user/libm.o" "$ROOT/user/libcrypt.a"

echo "[port-dropbear] 5/5 sign + embed -> crypto/user_dropbear_elf.h"
cd "$ROOT"   # run python from the repo (a pyenv-local interpreter has 'cryptography')
python3 "$ROOT/tools/secos-sign" "$ROOT/user/dropbearmulti.elf" --dev
python3 "$ROOT/tools/elf2h.py" "$ROOT/user/dropbearmulti.elf" user_dropbear_elf "$ROOT/crypto/user_dropbear_elf.h"
echo "[port-dropbear] DONE: $(stat -c %s "$ROOT/user/dropbearmulti.elf") bytes -> crypto/user_dropbear_elf.h"
