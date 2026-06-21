#ifndef _PORT_WCHAR_H
#define _PORT_WCHAR_H
#include <stddef.h>
typedef int wchar_t; typedef int wint_t;
typedef struct { int __c; } mbstate_t;
#define WEOF ((wint_t)-1)
#endif
