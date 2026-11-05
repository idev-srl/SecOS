/* SecOS BusyBox overlay: a private, complete <stdio.h>.
 *
 * The SecOS libc FILE (user/include/stdio.h) is unbuffered on input and has no
 * getline/scanf/popen; BusyBox needs all of that. This header does NOT include
 * the libc one: it declares its own FILE and links every function to a bbio_*
 * symbol (asm labels), implemented in user/busybox_compat.c, so it can coexist
 * with libc.o in the same link. SPDX-License-Identifier: MIT */
#ifndef _BB_STDIO_H
#define _BB_STDIO_H
#define _STDIO_H 1          /* keep the SecOS libc <stdio.h> out */
#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

#define EOF (-1)
#define BUFSIZ 4096
#define FILENAME_MAX 1024
#define FOPEN_MAX 32
#define L_tmpnam 32
#define TMP_MAX 238328
#define P_tmpdir "/tmp"
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

typedef struct bbio_file FILE;
typedef long fpos_t;

#define BBIO_R   1
#define BBIO_W   2
#define BBIO_EOF 4
#define BBIO_ERR 8
struct bbio_file {
    int fd;
    int flags;                 /* BBIO_* */
    int mode;                  /* _IOFBF/_IOLBF/_IONBF */
    int pid;                   /* popen child, or 0 */
    unsigned char* buf;        /* shared read/write buffer */
    size_t size;               /* buffer capacity */
    size_t rpos, rlen;         /* read window [rpos, rlen) */
    size_t wlen;               /* pending write bytes */
    int ungot;                 /* extra ungetc slot (-1 = none) */
    int own_buf;
    struct bbio_file* next;    /* list of open streams (flush at exit) */
    unsigned char sbuf[1];
};

extern FILE* stdin  __asm__("bbio_stdin");
extern FILE* stdout __asm__("bbio_stdout");
extern FILE* stderr __asm__("bbio_stderr");
#define stdin  stdin
#define stdout stdout
#define stderr stderr

#define BBIO(ret, name, args) ret name args __asm__("bbio_" #name)
BBIO(FILE*, fopen, (const char* path, const char* mode));
BBIO(FILE*, fdopen, (int fd, const char* mode));
BBIO(FILE*, freopen, (const char* path, const char* mode, FILE* f));
BBIO(FILE*, popen, (const char* cmd, const char* mode));
BBIO(int, pclose, (FILE* f));
BBIO(FILE*, tmpfile, (void));
BBIO(FILE*, fmemopen, (void* buf, size_t size, const char* mode));
BBIO(FILE*, open_memstream, (char** ptr, size_t* size));
BBIO(int, fclose, (FILE* f));
BBIO(int, fflush, (FILE* f));
BBIO(int, fileno, (FILE* f));
BBIO(int, feof, (FILE* f));
BBIO(int, ferror, (FILE* f));
BBIO(void, clearerr, (FILE* f));
BBIO(int, setvbuf, (FILE* f, char* buf, int mode, size_t size));
BBIO(void, setbuf, (FILE* f, char* buf));
BBIO(void, setbuffer, (FILE* f, char* buf, size_t size));
BBIO(void, setlinebuf, (FILE* f));
BBIO(int, fgetc, (FILE* f));
BBIO(int, getc, (FILE* f));
BBIO(int, getchar, (void));
BBIO(int, ungetc, (int c, FILE* f));
BBIO(char*, fgets, (char* s, int n, FILE* f));
BBIO(ssize_t, getline, (char** line, size_t* n, FILE* f));
BBIO(ssize_t, getdelim, (char** line, size_t* n, int delim, FILE* f));
BBIO(size_t, fread, (void* p, size_t sz, size_t n, FILE* f));
BBIO(size_t, fwrite, (const void* p, size_t sz, size_t n, FILE* f));
BBIO(int, fputc, (int c, FILE* f));
BBIO(int, putc, (int c, FILE* f));
BBIO(int, putchar, (int c));
BBIO(int, fputs, (const char* s, FILE* f));
BBIO(int, fseek, (FILE* f, long off, int whence));
BBIO(int, fseeko, (FILE* f, off_t off, int whence));
BBIO(long, ftell, (FILE* f));
BBIO(off_t, ftello, (FILE* f));
BBIO(void, rewind, (FILE* f));
BBIO(int, fgetpos, (FILE* f, fpos_t* p));
BBIO(int, fsetpos, (FILE* f, const fpos_t* p));
BBIO(int, printf, (const char* fmt, ...));
BBIO(int, fprintf, (FILE* f, const char* fmt, ...));
BBIO(int, dprintf, (int fd, const char* fmt, ...));
BBIO(int, sprintf, (char* s, const char* fmt, ...));
BBIO(int, snprintf, (char* s, size_t n, const char* fmt, ...));
BBIO(int, asprintf, (char** s, const char* fmt, ...));
BBIO(int, vprintf, (const char* fmt, va_list ap));
BBIO(int, vfprintf, (FILE* f, const char* fmt, va_list ap));
BBIO(int, vdprintf, (int fd, const char* fmt, va_list ap));
BBIO(int, vsprintf, (char* s, const char* fmt, va_list ap));
BBIO(int, vsnprintf, (char* s, size_t n, const char* fmt, va_list ap));
BBIO(int, vasprintf, (char** s, const char* fmt, va_list ap));
BBIO(int, scanf, (const char* fmt, ...));
BBIO(int, fscanf, (FILE* f, const char* fmt, ...));
BBIO(int, sscanf, (const char* s, const char* fmt, ...));
BBIO(int, vscanf, (const char* fmt, va_list ap));
BBIO(int, vfscanf, (FILE* f, const char* fmt, va_list ap));
BBIO(int, vsscanf, (const char* s, const char* fmt, va_list ap));
BBIO(void, perror, (const char* s));
BBIO(char*, tmpnam, (char* s));
BBIO(char*, tempnam, (const char* dir, const char* pfx));
BBIO(char*, ctermid, (char* s));
/* puts/rename/remove are labelled in busybox_port.h (libsecos.h declares them) */
int puts(const char* s);
int rename(const char* from, const char* to);
int remove(const char* path);
int renameat(int od, const char* from, int nd, const char* to);

/* glibc unlocked variants (BusyBox uses them): same functions, same symbols */
int    getc_unlocked(FILE* f)               __asm__("bbio_getc");
int    getchar_unlocked(void)               __asm__("bbio_getchar");
int    putc_unlocked(int c, FILE* f)        __asm__("bbio_putc");
int    putchar_unlocked(int c)              __asm__("bbio_putchar");
int    fgetc_unlocked(FILE* f)              __asm__("bbio_fgetc");
int    fputc_unlocked(int c, FILE* f)       __asm__("bbio_fputc");
char*  fgets_unlocked(char* s, int n, FILE* f) __asm__("bbio_fgets");
int    fputs_unlocked(const char* s, FILE* f)  __asm__("bbio_fputs");
int    fflush_unlocked(FILE* f)             __asm__("bbio_fflush");
size_t fread_unlocked(void* p, size_t sz, size_t n, FILE* f)        __asm__("bbio_fread");
size_t fwrite_unlocked(const void* p, size_t sz, size_t n, FILE* f) __asm__("bbio_fwrite");
int    feof_unlocked(FILE* f)               __asm__("bbio_feof");
int    ferror_unlocked(FILE* f)             __asm__("bbio_ferror");
void   clearerr_unlocked(FILE* f)           __asm__("bbio_clearerr");
int    fileno_unlocked(FILE* f)             __asm__("bbio_fileno");
#define flockfile(f)            ((void)0)
#define funlockfile(f)          ((void)0)
#endif
