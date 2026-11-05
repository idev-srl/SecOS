/* SecOS BusyBox overlay: <sys/mount.h> (Linux MS_* flags; the SecOS mount
 * syscall takes dev/target/fstype only). SPDX-License-Identifier: MIT */
#ifndef _BB_SYS_MOUNT_H
#define _BB_SYS_MOUNT_H
#include <sys/ioctl.h>
#define MS_RDONLY      1
#define MS_NOSUID      2
#define MS_NODEV       4
#define MS_NOEXEC      8
#define MS_SYNCHRONOUS 16
#define MS_REMOUNT     32
#define MS_MANDLOCK    64
#define MS_DIRSYNC     128
#define MS_NOSYMFOLLOW 256
#define MS_NOATIME     1024
#define MS_NODIRATIME  2048
#define MS_BIND        4096
#define MS_MOVE        8192
#define MS_REC         16384
#define MS_SILENT      32768
#define MS_POSIXACL    (1<<16)
#define MS_UNBINDABLE  (1<<17)
#define MS_PRIVATE     (1<<18)
#define MS_SLAVE       (1<<19)
#define MS_SHARED      (1<<20)
#define MS_RELATIME    (1<<21)
#define MS_KERNMOUNT   (1<<22)
#define MS_I_VERSION   (1<<23)
#define MS_STRICTATIME (1<<24)
#define MS_LAZYTIME    (1<<25)
#define MS_MGC_VAL     0xc0ed0000
#define MS_MGC_MSK     0xffff0000
#define MNT_FORCE      1
#define MNT_DETACH     2
#define MNT_EXPIRE     4
#define UMOUNT_NOFOLLOW 8
#define BLKROSET   _IO(0x12, 93)
#define BLKROGET   _IO(0x12, 94)
#define BLKGETSIZE _IO(0x12, 96)
#define BLKSSZGET  _IO(0x12, 104)
#define BLKGETSIZE64 _IOR(0x12, 114, unsigned long)
int sbb_mount(const char* src, const char* tgt, const char* type, unsigned long flags, const void* data);
int sbb_umount2(const char* tgt, int flags);
int umount2(const char* tgt, int flags);
#endif
