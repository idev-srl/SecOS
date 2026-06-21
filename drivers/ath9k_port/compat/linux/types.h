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
#endif
