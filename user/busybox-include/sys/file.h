/* SecOS BusyBox overlay: <sys/file.h>. */
#ifndef _BB_SYS_FILE_H
#define _BB_SYS_FILE_H
#include <fcntl.h>
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
int flock(int fd, int op);
#endif
