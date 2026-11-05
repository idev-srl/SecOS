/* SecOS BusyBox overlay: <sys/statfs.h>. No statfs syscall yet (shim returns
 * ENOSYS; TODO(integration): SYS_STATFS). SPDX-License-Identifier: MIT */
#ifndef _BB_SYS_STATFS_H
#define _BB_SYS_STATFS_H
#include <sys/types.h>
typedef struct { int __val[2]; } fsid_t;
struct statfs {
    long f_type, f_bsize;
    fsblkcnt_t f_blocks, f_bfree, f_bavail;
    fsfilcnt_t f_files, f_ffree;
    fsid_t f_fsid;
    long f_namelen, f_frsize, f_flags, f_spare[4];
};
int statfs(const char* path, struct statfs* b);
int fstatfs(int fd, struct statfs* b);
#endif
