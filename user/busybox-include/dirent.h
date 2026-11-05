/* SecOS BusyBox overlay: <dirent.h> = SecOS libc (over SYS_GETDENTS) + extras.
 * SPDX-License-Identifier: MIT */
#ifndef _BB_DIRENT_H
#define _BB_DIRENT_H
#include <sys/types.h>
#include_next <dirent.h>
#define DT_FIFO 1
#define DT_CHR  2
#define DT_BLK  6
#define DT_SOCK 12
#define DT_WHT  14
#define IFTODT(m) (((m) & 0170000) >> 12)
#define DTTOIF(t) ((t) << 12)
int   dirfd(DIR* d);
DIR*  fdopendir(int fd);
void  rewinddir(DIR* d);
long  telldir(DIR* d);
void  seekdir(DIR* d, long pos);
int   scandir(const char* dir, struct dirent*** list,
              int (*sel)(const struct dirent*),
              int (*cmp)(const struct dirent**, const struct dirent**));
int   alphasort(const struct dirent** a, const struct dirent** b);
int   versionsort(const struct dirent** a, const struct dirent** b);
#endif
