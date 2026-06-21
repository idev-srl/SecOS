#ifndef _PORT_INTTYPES_H
#define _PORT_INTTYPES_H
#include <stdint.h>
#define PRId64 "ld"
#define PRIu64 "lu"
#define PRIx64 "lx"
#define PRIdMAX "ld"
#define PRIuMAX "lu"
#define PRIxMAX "lx"
typedef long intmax_t; typedef unsigned long uintmax_t;
intmax_t strtoimax(const char*, char**, int);
uintmax_t strtoumax(const char*, char**, int);
#endif
