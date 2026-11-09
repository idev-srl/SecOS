#!/usr/bin/env bash
# SecOS - Mozilla CA bundle (U10): fetch curl's extract of Mozilla's root
# certificate set and embed it as kernel/ca_bundle.h (for the TLS userland:
# /etc/ssl/certs/ca-certificates.crt).
#
#   1. fetch https://curl.se/ca/cacert-$DATE.pem into build/dl, check it
#      against the SHA-256 pinned here AND against curl.se's published
#      cacert-$DATE.pem.sha256 (refuse on any mismatch);
#   2. tools/cabundle2h.py (run with python3 -I) validates the PEM text and
#      writes kernel/ca_bundle.h (ca_bundle_pem[], CA_BUNDLE_DATE, ...).
#
# Updating: set DATE/SHA to the newest file listed on
# https://curl.se/docs/caextract.html and run this script again.
# The certificate data is MPL-2.0 (docs/licenses/mozilla-ca-bundle-NOTICE).
# SPDX-License-Identifier: MIT (this script)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DATE=2026-11-05
SHA=427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285
URL="https://curl.se/ca/cacert-$DATE.pem"
DL="${SECOS_DL:-$ROOT/build/dl}"
F="$DL/cacert-$DATE.pem"

mkdir -p "$DL"
for x in "" .sha256; do
    if [ ! -f "$F$x" ]; then
        echo "[fetch-cabundle] downloading $URL$x"
        curl -fsSL -o "$F$x.part" "$URL$x" || { rm -f "$F$x.part"; echo "[fetch-cabundle] download failed"; exit 3; }
        mv "$F$x.part" "$F$x"
    fi
done
echo "$SHA  $F" | sha256sum -c --quiet - \
    || { echo "[fetch-cabundle] SHA-256 MISMATCH for cacert-$DATE.pem - refusing"; exit 1; }
pub="$(awk 'NR==1 {print $1}' "$F.sha256")"
[ "$pub" = "$SHA" ] || { echo "[fetch-cabundle] curl.se's .sha256 ($pub) differs from the pin - refusing"; exit 1; }
echo "[fetch-cabundle] cacert-$DATE.pem: pinned SHA-256 OK, matches curl.se's .sha256"
python3 -I "$ROOT/tools/cabundle2h.py" "$F" "$ROOT/kernel/ca_bundle.h"
