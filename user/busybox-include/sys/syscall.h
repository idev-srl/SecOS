/* SecOS BusyBox overlay: <sys/syscall.h>. There is no Linux syscall ABI on
 * SecOS; syscall() always fails with ENOSYS. */
#ifndef _BB_SYS_SYSCALL_H
#define _BB_SYS_SYSCALL_H
long syscall(long nr, ...);
#endif
