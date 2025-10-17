#pragma once
#include <stdint.h>

// Syscall numbers (phase 1)
#define SYS_EXIT    1
#define SYS_WRITE   2
#define SYS_READ    3
#define SYS_OPEN    4
#define SYS_CLOSE   5
#define SYS_GETPID  6
#define SYS_DRIVER  7  // driver space mediated hardware access

// Flags for open (simplified)
#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_RDWR   0x2

// Kernel-side dispatcher invoked by asm stub
uint64_t syscall_dispatch(uint64_t num, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4);

// Internal helpers (will be implemented in syscall.c)
int ksys_open(const char* path, int flags);
int ksys_close(int fd);
int ksys_write(int fd, const void* buf, int len);
int ksys_read(int fd, void* buf, int len);
int ksys_getpid(void);
void ksys_exit(int status);

// Driver interface forward declaration (struct defined in driver_if.h)
struct driver_call;
int driver_syscall(struct driver_call* req);
