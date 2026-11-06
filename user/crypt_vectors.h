/* SecOS - crypt() known-answer vectors, shared by the host test
 * (tools/crypt_kat/) and the in-guest test (user/crypt_test.c).
 *
 * SHA-256-crypt / SHA-512-crypt: the test vectors from Ulrich Drepper's
 * "Unix crypt using SHA-256 and SHA-512" specification
 * (https://www.akkadia.org/drepper/SHA-crypt.txt).
 * bcrypt: vectors from Openwall's crypt_blowfish wrapper.c test table;
 * the "$2b$" entry is the same input with the $2b$ prefix (identical output for
 * passwords under 255 bytes).
 * SPDX-License-Identifier: MIT */
#ifndef SECOS_CRYPT_VECTORS_H
#define SECOS_CRYPT_VECTORS_H

struct crypt_vector {
    const char *setting;
    const char *key;
    const char *expected;
};

#define CRYPT_LONG_KEY \
    "a very much longer text to encrypt.  This one even stretches over more" \
    "than one line."

static const struct crypt_vector crypt_vectors[] = {
    /* ---- SHA-512-crypt ($6$), Drepper spec ---- */
    { "$6$saltstring", "Hello world!",
      "$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJu"
      "esI68u4OTLiBFdcbYEdFCoEOfaS35inz1" },
    { "$6$rounds=10000$saltstringsaltstring", "Hello world!",
      "$6$rounds=10000$saltstringsaltst$OW1/O6BYHV6BcXZu8QVeXbDWra3Oeqh0sbH"
      "bbMCVNSnCM/UrjmM0Dp8vOuZeHBy/YTBmSK6H9qs/y3RnOaw5v." },
    { "$6$rounds=5000$toolongsaltstring", "This is just a test",
      "$6$rounds=5000$toolongsaltstrin$lQ8jolhgVRVhY4b5pZKaysCLi0QBxGoNeKQz"
      "Q3glMhwllF7oGDZxUhx1yxdYcz/e1JSbq3y6JMxxl8audkUEm0" },
    { "$6$rounds=1400$anotherlongsaltstring", CRYPT_LONG_KEY,
      "$6$rounds=1400$anotherlongsalts$POfYwTEok97VWcjxIiSOjiykti.o/pQs.wPv"
      "MxQ6Fm7I6IoYN3CmLs66x9t0oSwbtEW7o7UmJEiDwGqd8p4ur1" },
    { "$6$rounds=77777$short", "we have a short salt string but not a short password",
      "$6$rounds=77777$short$WuQyW2YR.hBNpjjRhpYD/ifIw05xdfeEyQoMxIXbkvr0gge"
      "1a1x3yRULJ5CCaUeOxFmtlcGZelFl5CxtgfiAc0" },
    { "$6$rounds=123456$asaltof16chars..", "a short string",
      "$6$rounds=123456$asaltof16chars..$BtCwjqMJGx5hrJhZywWvt0RLE8uZ4oPwce"
      "lCjmw2kSYu.Ec6ycULevoBK25fs2xXgMNrCzIMVcgEJAstJeonj1" },
    { "$6$rounds=10$roundstoolow", "the minimum number is still observed",
      "$6$rounds=1000$roundstoolow$kUMsbe306n21p9R.FRkW3IGn.S9NPN0x50YhH1xh"
      "LsPuWGsUSklZt58jaTfF4ZEQpyUNGc0dqbpBYYBaHHrsX." },
    /* ---- SHA-256-crypt ($5$), Drepper spec ---- */
    { "$5$saltstring", "Hello world!",
      "$5$saltstring$5B8vYYiY.CVt1RlTTf8KbXBH3hsxY/GNooZaBBGWEc5" },
    { "$5$rounds=10000$saltstringsaltstring", "Hello world!",
      "$5$rounds=10000$saltstringsaltst$3xv.VbSHBb41AL9AvLeujZkZRBAwqFMz2.opqey6IcA" },
    { "$5$rounds=5000$toolongsaltstring", "This is just a test",
      "$5$rounds=5000$toolongsaltstrin$Un/5jzAHMgOGZ5.mWJpuVolil07guHPvOW8mGRcvxa5" },
    { "$5$rounds=1400$anotherlongsaltstring", CRYPT_LONG_KEY,
      "$5$rounds=1400$anotherlongsalts$Rx.j8H.h8HjEDGomFU8bDkXm3XIUnzyxf12oP84Bnq1" },
    { "$5$rounds=77777$short", "we have a short salt string but not a short password",
      "$5$rounds=77777$short$JiO1O3ZpDAxGJeaDIuqCoEFysAe1mZNJRs3pw0KQRd/" },
    { "$5$rounds=123456$asaltof16chars..", "a short string",
      "$5$rounds=123456$asaltof16chars..$gP3VQ/6X7UUEW3HkBn2w1/Ptq2jxPyzV/cZKmF/wJvD" },
    { "$5$rounds=10$roundstoolow", "the minimum number is still observed",
      "$5$rounds=1000$roundstoolow$yfvwcWrQ8l/K0DAWyuPMDNHpIVlTQebY9l/gL972bIC" },
    /* ---- bcrypt ($2a$/$2b$), Openwall crypt_blowfish ---- */
    { "$2a$05$CCCCCCCCCCCCCCCCCCCCC.", "U*U",
      "$2a$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW" },
    { "$2b$05$CCCCCCCCCCCCCCCCCCCCC.", "U*U",
      "$2b$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW" },
    { "$2a$05$abcdefghijklmnopqrstuu",
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "0123456789chars after 72 are ignored",
      "$2a$05$abcdefghijklmnopqrstuu5s2v8.iXieOjg/.AySBTTZIIVFJeBui" },
};

#define CRYPT_NVECTORS ((int)(sizeof crypt_vectors / sizeof crypt_vectors[0]))

#endif
