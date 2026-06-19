#pragma once
#include <stdint.h>

// Syscall numbers (phase 1)
#define SYS_YIELD   0
#define SYS_EXIT    1
#define SYS_WRITE   2
#define SYS_READ    3
#define SYS_OPEN    4
#define SYS_CLOSE   5
#define SYS_GETPID  6
#define SYS_DRIVER  7  // driver space mediated hardware access
#define SYS_SPAWN   8  // load+verify a signed ELF from a VFS path, return pid
#define SYS_WAIT    9  // wait for a child pid to become ZOMBIE, return status
#define SYS_GETTICKS 10 // [M13] uptime in timer ticks
#define SYS_MSG_SEND 11 // [M13] send bytes to a kernel IPC channel (chan,buf,len)
#define SYS_MSG_RECV 12 // [M13] receive bytes from a kernel IPC channel (chan,buf,len)
#define SYS_SLEEP    13 // [M17] block the caller for N timer ticks
#define SYS_MMAP     14 // [M18] map anonymous memory (addr,len,prot,flags) -> addr
#define SYS_MUNMAP   15 // [M18] unmap a range (addr,len)
#define SYS_BRK      16 // [M18] set/query the heap break (addr) -> new break
#define SYS_MPROTECT 17 // [M18] change protection of a range (addr,len,prot)
#define SYS_FORK     18 // [M19] copy-on-write fork; returns child pid (parent) / 0 (child)
#define SYS_LSEEK    19 // [M23] reposition a file offset (fd, offset, whence) -> new offset
#define SYS_STAT     20 // [M23] stat a path into a struct secos_stat (path, statbuf)
// [M24] BSD-style sockets (gated by CAP_NET in the signed manifest)
#define SYS_SOCKET   21 // socket(type) -> sockfd
#define SYS_CONNECT  22 // connect(fd, ip, port)  TCP active open / UDP default peer
#define SYS_BIND     23 // bind(fd, port)         local UDP/TCP port
#define SYS_LISTEN   24 // listen(fd, backlog)    TCP passive open
#define SYS_ACCEPT   25 // accept(fd) -> newfd    (blocking)
#define SYS_SEND     26 // send(fd, buf, len)
#define SYS_RECV     27 // recv(fd, buf, len)     (blocking, bounded)
#define SYS_SENDTO   28 // sendto(fd, buf, len, sockaddr*)   UDP
#define SYS_RECVFROM 29 // recvfrom(fd, buf, len, sockaddr*) UDP
#define SYS_SOCKCLOSE 30// close a socket descriptor
#define SYS_PIPE     31 // [M25] pipe(int fds[2]) -> fds[0]=read end, fds[1]=write end

// [M24] socket types
#define SOCK_STREAM 1   // TCP
#define SOCK_DGRAM  2   // UDP

// [M24] address handed to sendto/recvfrom. ip = network-order octets (octet0 in
// the low byte, like the kernel's net_dev_t); port = host order.
struct secos_sockaddr {
    uint32_t ip;
    uint16_t port;
    uint16_t _pad;
};

// [M23] lseek whence
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// [M23] minimal stat node type (st_mode high bits, POSIX-ish)
#define S_IFREG 0x8000
#define S_IFDIR 0x4000

// [M18] mmap protection + flags (POSIX-ish subset)
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

// Flags for open (simplified)
#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_RDWR   0x2

// [M23] stat result handed to user space (ABI struct; libc mirrors this layout).
struct secos_stat {
    uint64_t st_size;   // size in bytes
    uint32_t st_mode;   // S_IFREG | S_IFDIR
    uint32_t st_pad;
};

// Kernel-side dispatcher invoked by asm stub
uint64_t syscall_dispatch(uint64_t num, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4);

// Internal helpers (will be implemented in syscall.c)
int ksys_open(const char* path, int flags);
int ksys_close(int fd);
int ksys_write(int fd, const void* buf, int len);
int ksys_read(int fd, void* buf, int len);
int ksys_getpid(void);
void ksys_exit(int status);
int ksys_spawn(const char* path);   // returns pid (>0) or -1 (no args)
int ksys_spawn_argv(const char* path, int argc, const char* const argv[]); // [M16]
int ksys_wait(int pid);             // returns 0 when pid has exited, -1 if unknown (poll)
// [M18] dynamic memory
uint64_t ksys_brk(uint64_t new_brk);
uint64_t ksys_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd);
int ksys_munmap(uint64_t addr, uint64_t len);
int ksys_mprotect(uint64_t addr, uint64_t len, int prot);
// [M23] file positioning + stat
long ksys_lseek(int fd, long offset, int whence);
int  ksys_stat(const char* path, struct secos_stat* st);
// [M25] anonymous pipe: allocate a pipe, return two fds (kfds[0]=read, kfds[1]=write)
int  ksys_pipe(int kfds[2]);

// Driver interface forward declaration (struct defined in driver_if.h)
struct driver_call;
int driver_syscall(struct driver_call* req);
