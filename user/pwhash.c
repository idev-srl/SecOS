/* SecOS password-hash helpers on top of crypt() (see user/include/pwhash.h).
 * SPDX-License-Identifier: MIT */
#include <crypt.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "pwhash.h"

#define PWHASH_SALT_CHARS 16

static const char pwhash_alphabet[64] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static void pwhash_wipe(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--)
        *v++ = 0;
}

static int pwhash_random(unsigned char *buf, size_t n)
{
    size_t got = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;
    while (got < n) {
        long r = (long)read(fd, buf + got, n - got);
        if (r <= 0) {
            close(fd);
            return -1;
        }
        got += (size_t)r;
    }
    close(fd);
    return 0;
}

int secos_make_salt(char *out, size_t n)
{
    unsigned char rnd[PWHASH_SALT_CHARS];
    int i;

    if (!out || n < SECOS_SALT_LEN + 1) {
        if (out && n)
            out[0] = '\0';
        return -1;
    }
    out[0] = '\0';
    if (pwhash_random(rnd, sizeof rnd) != 0)
        return -1;
    out[0] = '$';
    out[1] = '6';
    out[2] = '$';
    /* 64-symbol alphabet: the low 6 bits of a uniform byte are uniform. */
    for (i = 0; i < PWHASH_SALT_CHARS; i++)
        out[3 + i] = pwhash_alphabet[rnd[i] & 63];
    out[3 + PWHASH_SALT_CHARS] = '$';
    out[SECOS_SALT_LEN] = '\0';
    pwhash_wipe(rnd, sizeof rnd);
    return 0;
}

int secos_verify_password(const char *plain, const char *hash)
{
    struct crypt_data cd;
    const char *calc;
    size_t len, i;
    unsigned char diff = 0;

    if (!plain || !hash || hash[0] != '$')   /* "", "!", "*", "x": locked */
        return 0;
    memset(&cd, 0, sizeof cd);
    calc = crypt_r(plain, hash, &cd);
    if (!calc || calc[0] == '*') {
        pwhash_wipe(&cd, sizeof cd);
        return 0;
    }
    len = strlen(hash);
    if (strlen(calc) != len) {
        pwhash_wipe(&cd, sizeof cd);
        return 0;
    }
    for (i = 0; i < len; i++)
        diff |= (unsigned char)(calc[i] ^ hash[i]);
    pwhash_wipe(&cd, sizeof cd);
    return diff == 0;
}
