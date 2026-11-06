/* SecOS - stand-in for musl's internal <features.h>, included by the vendored
 * third_party/musl-crypt/src/include/crypt.h. It only has to supply the
 * `hidden` visibility macro musl uses for its internal __crypt_* symbols.
 * SPDX-License-Identifier: MIT */
#ifndef SECOS_MUSL_CRYPT_FEATURES_H
#define SECOS_MUSL_CRYPT_FEATURES_H

#define hidden __attribute__((__visibility__("hidden")))

#endif
