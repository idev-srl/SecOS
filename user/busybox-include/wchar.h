/* SecOS BusyBox overlay: <wchar.h> (C locale only: one byte == one char).
 * SPDX-License-Identifier: MIT */
#ifndef _BB_WCHAR_H
#define _BB_WCHAR_H
#include <stddef.h>
#include <stdarg.h>
typedef unsigned int wint_t;
typedef struct { unsigned __opaque1, __opaque2; } mbstate_t;
#define WEOF 0xffffffffU
#ifndef WCHAR_MIN
#define WCHAR_MIN (-2147483647-1)
#define WCHAR_MAX 2147483647
#endif
size_t  mbrtowc(wchar_t* wc, const char* s, size_t n, mbstate_t* st);
size_t  wcrtomb(char* s, wchar_t wc, mbstate_t* st);
int     mbsinit(const mbstate_t* st);
size_t  mbrlen(const char* s, size_t n, mbstate_t* st);
size_t  mbsrtowcs(wchar_t* d, const char** s, size_t n, mbstate_t* st);
size_t  wcsrtombs(char* d, const wchar_t** s, size_t n, mbstate_t* st);
int     wcwidth(wchar_t wc);
int     wcswidth(const wchar_t* s, size_t n);
size_t  wcslen(const wchar_t* s);
wchar_t* wcschr(const wchar_t* s, wchar_t c);
int     wcscmp(const wchar_t* a, const wchar_t* b);
int     wcsncmp(const wchar_t* a, const wchar_t* b, size_t n);
wchar_t* wcscpy(wchar_t* d, const wchar_t* s);
wint_t  btowc(int c);
int     wctob(wint_t c);
#endif
