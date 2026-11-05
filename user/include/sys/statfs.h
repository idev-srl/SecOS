/* <sys/statfs.h> — SecOS libc (SYS_STATFS 69, Linux struct statfs layout).
 * SPDX-License-Identifier: MIT */
#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H
typedef struct { int __val[2]; } fsid_t;
struct statfs {
    long f_type;            /* EXT2_SUPER_MAGIC 0xEF53, MSDOS 0x4d44, PROC 0x9fa0, ... */
    long f_bsize;
    unsigned long f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    fsid_t f_fsid;
    long f_namelen, f_frsize, f_flags, f_spare[4];
};
int statfs(const char* path, struct statfs* buf);
int fstatfs(int fd, struct statfs* buf);
#define EXT2_SUPER_MAGIC  0xEF53
#define EXT4_SUPER_MAGIC  0xEF53
#define MSDOS_SUPER_MAGIC 0x4d44
#define PROC_SUPER_MAGIC  0x9fa0
#define SYSFS_MAGIC       0x62656572
#define TMPFS_MAGIC       0x01021994
#define RAMFS_MAGIC       0x858458f6
#endif
