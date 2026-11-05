/* SecOS BusyBox overlay: <sys/types.h> = SecOS libc + the POSIX types BusyBox
 * expects. SPDX-License-Identifier: MIT */
#ifndef _BB_SYS_TYPES_H
#define _BB_SYS_TYPES_H
#include_next <sys/types.h>
#include <stdint.h>
typedef unsigned long  dev_t;
typedef unsigned long  ino_t;
typedef unsigned long  ino64_t;
typedef unsigned int   nlink_t;
typedef long           blksize_t;
typedef long           blkcnt_t;
typedef unsigned long  fsblkcnt_t;
typedef unsigned long  fsfilcnt_t;
typedef unsigned int   id_t;
typedef int            key_t;
typedef unsigned int   useconds_t;
typedef long           suseconds_t;
typedef char*          caddr_t;
typedef long           off64_t;
typedef long           loff_t;
typedef int            clockid_t;
typedef void*          timer_t;
typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned int   u_int;
typedef unsigned long  u_long;
typedef unsigned short ushort;
typedef unsigned int   uint;
typedef unsigned long  ulong;
typedef int8_t   int8;
typedef uint8_t  u_int8_t;
typedef uint16_t u_int16_t;
typedef uint32_t u_int32_t;
typedef uint64_t u_int64_t;
typedef long long quad_t;
typedef unsigned long long u_quad_t;
typedef long     register_t;
#endif
