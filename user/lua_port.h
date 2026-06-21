/* SecOS freestanding configuration for the lua port (force-included with -include
 * BEFORE every lua translation unit, so these win over lua's own guarded
 * defaults). Routes lua output through raw write() (no stdio FILE needed) and
 * pins the locale radix to '.'. */
#ifndef LUA_PORT_SECOS_H
#define LUA_PORT_SECOS_H

#include <unistd.h>   /* write   */
#include <stdio.h>    /* dprintf */
#include <signal.h>   /* sig_atomic_t (lua lstate.h) */

/* print / io.write output → fd 1; errors → fd 2. */
#define lua_writestring(s,l)      ((void)write(1,(s),(l)))
#define lua_writeline()           ((void)write(1,"\n",1))
#define lua_writestringerror(s,p) ((void)dprintf(2,(s),(p)))

/* No locale: the decimal radix character is always '.'. */
#define lua_getlocaledecpoint()   '.'

#endif
