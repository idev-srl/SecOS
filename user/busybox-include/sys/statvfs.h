/* SecOS BusyBox overlay: <sys/statvfs.h>. SPDX-License-Identifier: MIT */
#ifndef _BB_SYS_STATVFS_H
#define _BB_SYS_STATVFS_H
#include <sys/types.h>
struct statvfs {
    unsigned long f_bsize, f_frsize;
    fsblkcnt_t f_blocks, f_bfree, f_bavail;
    fsfilcnt_t f_files, f_ffree, f_favail;
    unsigned long f_fsid, f_flag, f_namemax;
    int __reserved[6];
};
#define ST_RDONLY 1
#define ST_NOSUID 2
int statvfs(const char* path, struct statvfs* b);
int fstatvfs(int fd, struct statvfs* b);
#endif
