#ifndef _PORT_LANGINFO_H
#define _PORT_LANGINFO_H
typedef int nl_item;
#define CODESET 14
#define RADIXCHAR 0x10000
#define THOUSEP   0x10001
#define D_T_FMT 1
char* nl_langinfo(nl_item);
#endif
