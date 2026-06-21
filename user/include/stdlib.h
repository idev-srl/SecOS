/* <stdlib.h> — SecOS libc. SPDX-License-Identifier: MIT */
#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>

void*  malloc(size_t size);
void   free(void* p);
void*  calloc(size_t nmemb, size_t size);
void*  realloc(void* p, size_t size);

int    atoi(const char* s);
long   atol(const char* s);
double atof(const char* s);
long   strtol(const char* s, char** endptr, int base);
unsigned long strtoul(const char* s, char** endptr, int base);
double strtod(const char* s, char** endptr);
float  strtof(const char* s, char** endptr);
long long strtoll(const char* s, char** endptr, int base);
unsigned long long strtoull(const char* s, char** endptr, int base);

int    abs(int x);
long   labs(long x);

void   qsort(void* base, size_t nmemb, size_t size,
             int (*cmp)(const void*, const void*));
void*  bsearch(const void* key, const void* base, size_t nmemb, size_t size,
               int (*cmp)(const void*, const void*));

int    rand(void);
void   srand(unsigned seed);
#define RAND_MAX 0x7fffffff

char*  getenv(const char* name);
int    setenv(const char* name, const char* value, int overwrite);   /* [M39] */
int    unsetenv(const char* name);
int    putenv(char* s);
extern char** environ;

void   exit(int code) __attribute__((noreturn));
void   abort(void) __attribute__((noreturn));

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#endif
