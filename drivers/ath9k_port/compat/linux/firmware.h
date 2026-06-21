#ifndef _C_FW_H
#define _C_FW_H
#include <linux/types.h>
struct firmware { size_t size; const u8* data; };
struct device;
static inline int request_firmware(const struct firmware**f,const char*n,struct device*d){(void)f;(void)n;(void)d;return -1;}
static inline void release_firmware(const struct firmware*f){(void)f;}
#endif
