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
/* Broken-down time for the os library comes from <time.h> (real UTC wall
 * clock: os.time()/os.date() report the RTC-anchored CLOCK_REALTIME). */
char*      tmpnam(char* s);

#endif
