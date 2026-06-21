#ifndef _C_BO_H
#define _C_BO_H
#include <linux/types.h>
#define cpu_to_le32(x) ((__le32)(x))
#define cpu_to_le16(x) ((__le16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define le16_to_cpu(x) ((u16)(x))
#define cpu_to_be32(x) (__builtin_bswap32(x))
#define be32_to_cpu(x) (__builtin_bswap32(x))
#define cpu_to_be16(x) (__builtin_bswap16(x))
#define be16_to_cpu(x) (__builtin_bswap16(x))
#endif
