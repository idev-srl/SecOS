/* SecOS freestanding configuration for the lua port (force-included with -include
 * BEFORE every lua translation unit, so these win over lua's own guarded
 * defaults). Routes print() through the stdout FILE (like upstream lua.c, so it
 * stays ordered with io.write) and pins the locale radix to '.'. */
#ifndef LUA_PORT_SECOS_H
#define LUA_PORT_SECOS_H

#include <unistd.h>   /* write   */
#include <stdio.h>    /* dprintf */
#include <signal.h>   /* sig_atomic_t (lua lstate.h) */
#include <stddef.h>

/* print() shares the stdout FILE with io.write (same order); errors → fd 2. */
#define lua_writestring(s,l)      ((void)fwrite((s),1,(l),stdout))
#define lua_writeline()           ((void)(fputc('\n',stdout), fflush(stdout)))
#define lua_writestringerror(s,p) ((void)dprintf(2,(s),(p)))

/* No locale: the decimal radix character is always '.'. */
#define lua_getlocaledecpoint()   '.'

/* io/os libraries: the few stdio/time bits the SecOS libc lacks, implemented
 * in user/lua_main.c (SecOS FILE input is unbuffered; output flushes itself). */
#ifndef _IOFBF
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#endif
int   setvbuf(FILE* f, char* buf, int mode, size_t size);
void  clearerr(FILE* f);
FILE* tmpfile(void);
FILE* freopen(const char* path, const char* mode, FILE* f);

#include <time.h>
#include <locale.h>
#ifndef L_tmpnam
#define L_tmpnam 32
#endif
#ifndef LC_COLLATE
#define LC_COLLATE  3
#define LC_MONETARY 4
#define LC_TIME     5
#endif
/* Broken-down time for the os library. SecOS has no wall clock in user space:
 * time() is seconds since boot, so os.date() shows 1970-01-01 + uptime. */
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
struct tm* gmtime(const time_t* t);
struct tm* localtime(const time_t* t);
struct tm* gmtime_r(const time_t* t, struct tm* r);
struct tm* localtime_r(const time_t* t, struct tm* r);
time_t     mktime(struct tm* tm);
size_t     strftime(char* s, size_t max, const char* fmt, const struct tm* tm);
double     difftime(time_t a, time_t b);
char*      tmpnam(char* s);

#endif
