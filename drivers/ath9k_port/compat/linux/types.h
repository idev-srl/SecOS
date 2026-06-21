#ifndef _COMPAT_TYPES_H
#define _COMPAT_TYPES_H
typedef unsigned char u8; typedef signed char s8;
typedef unsigned short u16; typedef short s16;
typedef unsigned int u32; typedef int s32;
typedef unsigned long long u64; typedef long long s64;
typedef u16 __le16; typedef u16 __be16; typedef u32 __le32; typedef u32 __be32; typedef u64 __le64;
typedef unsigned long size_t; typedef long ssize_t; typedef long ptrdiff_t;
typedef int bool; typedef u8 __u8; typedef u16 __u16; typedef u32 __u32; typedef u64 __u64;
typedef unsigned gfp_t; typedef u64 dma_addr_t;
#define true 1
#define false 0
#define NULL ((void*)0)
typedef signed char int8_t; typedef short int16_t; typedef int int32_t; typedef long long int64_t;
typedef unsigned char uint8_t; typedef unsigned short uint16_t; typedef unsigned int uint32_t; typedef unsigned long long uint64_t;
typedef u64 __be64; typedef u64 __le64; typedef long long s64; typedef u8 u_int8_t; typedef u16 u_int16_t; typedef u32 u_int32_t;
typedef long intptr_t; typedef unsigned long uintptr_t;
typedef struct { int counter; } atomic_t; typedef s64 ktime_t; typedef u64 cycle_t;
typedef long off_t; typedef long loff_t; typedef int pid_t;
#endif
