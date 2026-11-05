/*
 * csprng.h — kernel cryptographic RNG (backs /dev/random and /dev/urandom).
 * SPDX-License-Identifier: MIT
 */
#ifndef CRYPTO_CSPRNG_H
#define CRYPTO_CSPRNG_H
#include <stdint.h>
#include <stddef.h>

void     csprng_init(void);                  /* idempotent; also done on first use */
void     csprng_get(void* out, size_t len);  /* never blocks */
uint32_t csprng_u32(void);
void     csprng_stir_tsc(void);              /* timer IRQ: fold one TSC sample in */

#endif /* CRYPTO_CSPRNG_H */
