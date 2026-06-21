/* SecOS - SHA-1 (for WPA2 PBKDF2/HMAC; not for new security uses). [M38] */
#ifndef SECOS_SHA1_H
#define SECOS_SHA1_H
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; int n; } sha1_ctx;
void sha1_init(sha1_ctx* c);
void sha1_update(sha1_ctx* c, const void* data, size_t len);
void sha1_final(sha1_ctx* c, uint8_t out[20]);
void sha1(const void* data, size_t len, uint8_t out[20]);
#endif
