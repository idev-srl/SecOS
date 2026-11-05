/* SecOS BusyBox overlay: <sys/xattr.h>. */
#ifndef _BB_SYS_XATTR_H
#define _BB_SYS_XATTR_H
#include <sys/types.h>
ssize_t getxattr(const char* p, const char* n, void* v, size_t s);
int setxattr(const char* p, const char* n, const void* v, size_t s, int f);
#endif
