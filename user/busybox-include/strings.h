/* SecOS BusyBox overlay: <strings.h>. SPDX-License-Identifier: MIT */
#ifndef _BB_STRINGS_H
#define _BB_STRINGS_H
#include <stddef.h>
int   strcasecmp(const char* a, const char* b);
int   strncasecmp(const char* a, const char* b, size_t n);
void  bzero(void* p, size_t n);
void  bcopy(const void* s, void* d, size_t n);
int   bcmp(const void* a, const void* b, size_t n);
char* index(const char* s, int c);
char* rindex(const char* s, int c);
int   ffs(int v);
int   ffsl(long v);
#endif
