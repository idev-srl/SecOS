/* SecOS BusyBox overlay: <string.h> = SecOS libc + POSIX/GNU extras.
 * SPDX-License-Identifier: MIT */
#ifndef _BB_STRING_H
#define _BB_STRING_H
#include_next <string.h>
#include <strings.h>
char*  strndup(const char* s, size_t n);
char*  strsignal(int sig);
char*  strchrnul(const char* s, int c);
char*  strsep(char** sp, const char* delim);
char*  strcasestr(const char* hay, const char* needle);
char*  stpcpy(char* d, const char* s);
char*  stpncpy(char* d, const char* s, size_t n);
void*  mempcpy(void* d, const void* s, size_t n);
void*  memrchr(const void* s, int c, size_t n);
void*  memmem(const void* h, size_t hl, const void* n, size_t nl);
void*  memccpy(void* d, const void* s, int c, size_t n);
void*  rawmemchr(const void* s, int c);
int    strverscmp(const char* a, const char* b);
int    strcoll(const char* a, const char* b);
size_t strxfrm(char* d, const char* s, size_t n);
size_t strlcat(char* d, const char* s, size_t n);
char*  strtok_r(char* s, const char* delim, char** save);
int    strerror_r(int e, char* buf, size_t len);
char*  basename(const char* path);   /* GNU form (string.h) */
void   explicit_bzero(void* p, size_t n);
#endif
