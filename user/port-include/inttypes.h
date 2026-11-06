#ifndef _PORT_INTTYPES_H
#define _PORT_INTTYPES_H
#include <stdint.h>
#define PRId64 "ld"
#define PRIu64 "lu"
#define PRIx64 "lx"
#define PRIX64 "lX"
#define PRIi64 "li"
#define PRId32 "d"
#define PRIi32 "i"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"
#define PRId16 "d"
#define PRIu16 "u"
#define PRId8 "d"
#define PRIu8 "u"
#define PRIdPTR "ld"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#define PRIdMAX "ld"
#define PRIuMAX "lu"
#define PRIxMAX "lx"
typedef long intmax_t; typedef unsigned long uintmax_t;
intmax_t strtoimax(const char*, char**, int);
uintmax_t strtoumax(const char*, char**, int);
#endif
