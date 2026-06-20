/* <stdio.h> — SecOS libc. A small buffered FILE layer over the raw fd syscalls.
 * SPDX-License-Identifier: MIT */
#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>

#ifndef EOF
#define EOF (-1)
#endif
#define BUFSIZ 1024

typedef struct _FILE {
    int   fd;
    int   flags;        /* readable/writable/eof/error */
    int   ungot;        /* ungetc one-char pushback, or -1 */
    unsigned char wbuf[BUFSIZ];
    size_t wlen;
} FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

/* formatted output */
int printf(const char* fmt, ...);
int fprintf(FILE* f, const char* fmt, ...);
int dprintf(int fd, const char* fmt, ...);
int sprintf(char* buf, const char* fmt, ...);
int snprintf(char* buf, size_t size, const char* fmt, ...);
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);
int vfprintf(FILE* f, const char* fmt, va_list ap);

/* character / line I/O */
int   fputc(int c, FILE* f);
int   putc(int c, FILE* f);
int   putchar(int c);
int   fputs(const char* s, FILE* f);
int   puts(const char* s);
int   fgetc(FILE* f);
int   getc(FILE* f);
int   getchar(void);
int   ungetc(int c, FILE* f);
char* fgets(char* buf, int size, FILE* f);

/* block I/O */
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* f);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* f);

/* open/close/flush */
FILE* fopen(const char* path, const char* mode);
FILE* fdopen(int fd, const char* mode);
int   fclose(FILE* f);
int   fflush(FILE* f);
int   feof(FILE* f);
int   ferror(FILE* f);
long  ftell(FILE* f);
int   fseek(FILE* f, long off, int whence);
void  perror(const char* s);

#endif
