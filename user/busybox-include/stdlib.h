/* SecOS BusyBox overlay: <stdlib.h> = SecOS libc + POSIX/GNU extras (shims in
 * user/busybox_compat.c). SPDX-License-Identifier: MIT */
#ifndef _BB_STDLIB_H
#define _BB_STDLIB_H
#include_next <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;
#define MB_CUR_MAX 1
#define alloca(n) __builtin_alloca(n)
long long   llabs(long long x);
div_t       div(int n, int d);
ldiv_t      ldiv(long n, long d);
long double strtold(const char* s, char** end);
int         atexit(void (*fn)(void));
int         mkstemp(char* tmpl);
int         mkstemps(char* tmpl, int suffixlen);
char*       mkdtemp(char* tmpl);
char*       mktemp(char* tmpl);
char*       realpath(const char* path, char* resolved);
long        random(void);
void        srandom(unsigned int seed);
char*       initstate(unsigned seed, char* state, size_t n);
int         rand_r(unsigned* seed);
double      drand48(void);
long        lrand48(void);
void        srand48(long seed);
int         clearenv(void);
int         system(const char* cmd);
char*       ptsname(int fd);
int         ptsname_r(int fd, char* buf, size_t len);
int         grantpt(int fd);
int         unlockpt(int fd);
int         posix_openpt(int flags);
int         getloadavg(double* avg, int n);
int         mbtowc(wchar_t* wc, const char* s, size_t n);
int         wctomb(char* s, wchar_t wc);
size_t      mbstowcs(wchar_t* d, const char* s, size_t n);
size_t      wcstombs(char* d, const wchar_t* s, size_t n);
int         mblen(const char* s, size_t n);
void*       aligned_alloc(size_t al, size_t n);
int         posix_memalign(void** p, size_t al, size_t n);
void*       memalign(size_t al, size_t n);
void*       valloc(size_t n);
int         getsubopt(char** opt, char* const* tokens, char** val);
char*       secure_getenv(const char* name);
void        qsort_r(void* base, size_t n, size_t sz, int (*cmp)(const void*, const void*, void*), void* arg);
#endif
