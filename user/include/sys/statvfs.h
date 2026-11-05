/* <sys/statvfs.h> — SecOS libc (POSIX view of statfs). SPDX: MIT */
#ifndef _SYS_STATVFS_H
#define _SYS_STATVFS_H
struct statvfs {
    unsigned long f_bsize, f_frsize;
    unsigned long f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail;
    unsigned long f_fsid, f_flag, f_namemax;
};
#define ST_RDONLY 1
#define ST_NOSUID 2
int statvfs(const char* path, struct statvfs* buf);
int fstatvfs(int fd, struct statvfs* buf);
#endif
