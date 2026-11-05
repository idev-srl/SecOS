/* SecOS BusyBox overlay: <sys/sysmacros.h> (Linux dev_t encoding). */
#ifndef _BB_SYS_SYSMACROS_H
#define _BB_SYS_SYSMACROS_H
#define major(d) ((unsigned)((((d) >> 32) & 0xfffff000) | (((d) >> 8) & 0xfff)))
#define minor(d) ((unsigned)((((d) >> 12) & 0xffffff00) | ((d) & 0xff)))
#define makedev(x, y) ((unsigned long)( \
    (((unsigned long)(x) & 0xfffff000UL) << 32) | (((unsigned long)(x) & 0xfffUL) << 8) | \
    (((unsigned long)(y) & 0xffffff00UL) << 12) | ((unsigned long)(y) & 0xffUL)))
#endif
