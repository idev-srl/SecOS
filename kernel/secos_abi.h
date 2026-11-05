/* SecOS kernel <-> user-space ABI constants. SPDX-License-Identifier: MIT
 *
 * Included by BOTH the kernel (kernel/syscall.h) and the user libc
 * (user/libsecos.h), so it must stay freestanding: no includes, no kernel types.
 *
 * Every struct the kernel copies out to user space has its size (and key
 * offsets) pinned here, and each side static-asserts its own definition against
 * these numbers. Growing a struct therefore forces a deliberate bump here, which
 * breaks the build of whichever side was not updated — instead of the kernel
 * silently writing past a too-small user buffer (a stale `stat` applet reserving
 * 56 bytes for an 88-byte struct had its return address overwritten).
 *
 * When you bump SECOS_ABI_VERSION / a size here, rebuild every embedded program
 * (`make user-progs`, `make port-bash`, `make port-lua`) and commit the
 * regenerated crypto/user_*_elf.h; `make check-user-progs` verifies they match.
 */
#ifndef SECOS_ABI_H
#define SECOS_ABI_H

#define SECOS_ABI_VERSION 1

/* struct secos_stat (kernel) == struct stat (libc): filled by SYS_STAT/LSTAT/FSTAT */
#define SECOS_STAT_SIZE       88
#define SECOS_STAT_OFF_SIZE   0
#define SECOS_STAT_OFF_MODE   8
#define SECOS_STAT_OFF_ATIME  24
#define SECOS_STAT_OFF_INO    56
#define SECOS_STAT_OFF_BLOCKS 80

#define SECOS_ABI_CHECK_STAT(type)                                                   \
    _Static_assert(sizeof(type) == SECOS_STAT_SIZE, "stat ABI size mismatch");        \
    _Static_assert(__builtin_offsetof(type, st_size) == SECOS_STAT_OFF_SIZE,         \
                   "stat ABI: st_size offset");                                       \
    _Static_assert(__builtin_offsetof(type, st_mode) == SECOS_STAT_OFF_MODE,         \
                   "stat ABI: st_mode offset");                                       \
    _Static_assert(__builtin_offsetof(type, st_atime) == SECOS_STAT_OFF_ATIME,       \
                   "stat ABI: st_atime offset");                                      \
    _Static_assert(__builtin_offsetof(type, st_ino) == SECOS_STAT_OFF_INO,           \
                   "stat ABI: st_ino offset");                                        \
    _Static_assert(__builtin_offsetof(type, st_blocks) == SECOS_STAT_OFF_BLOCKS,     \
                   "stat ABI: st_blocks offset")

#endif /* SECOS_ABI_H */
