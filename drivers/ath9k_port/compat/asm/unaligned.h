#ifndef _C_UA_H
#define _C_UA_H
#include <linux/types.h>
static inline u16 get_unaligned_le16(const void*p){const u8*b=p;return b[0]|(b[1]<<8);}
static inline u32 get_unaligned_le32(const void*p){const u8*b=p;return b[0]|(b[1]<<8)|(b[2]<<16)|((u32)b[3]<<24);}
static inline u16 get_unaligned_be16(const void*p){const u8*b=p;return (b[0]<<8)|b[1];}
static inline void put_unaligned_le16(u16 v,void*p){u8*b=p;b[0]=v;b[1]=v>>8;}
static inline void put_unaligned_le32(u32 v,void*p){u8*b=p;b[0]=v;b[1]=v>>8;b[2]=v>>16;b[3]=v>>24;}
#endif
