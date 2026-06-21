/* <locale.h> — SecOS libc stub. No locale support; the radix character is always
 * '.' (lua's lua_getlocaledecpoint is overridden to '.'). */
#ifndef _LOCALE_H
#define _LOCALE_H
struct lconv { char* decimal_point; };
#define LC_ALL      0
#define LC_NUMERIC  1
#define LC_CTYPE    2
static inline char* setlocale(int c, const char* l){ (void)c; (void)l; return (char*)""; }
static inline struct lconv* localeconv(void){ static struct lconv lc = { (char*)"." }; return &lc; }
#endif
