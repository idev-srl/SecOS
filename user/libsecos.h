/* SecOS minimal user libc (POSIX-friendly surface over the custom syscall ABI).
 * SPDX-License-Identifier: MIT */
#ifndef LIBSECOS_H
#define LIBSECOS_H
#include <stddef.h>

typedef long ssize_t;

/* Raw syscall: rax=num, args in rdi,rsi,rdx,rcx,r8; via int 0x80. */
long secos_syscall(long num, long a0, long a1, long a2, long a3, long a4);

/* POSIX-ish wrappers */
ssize_t write(int fd, const void* buf, size_t len);
ssize_t read(int fd, void* buf, size_t len);
int     open(const char* path, int flags);
int     close(int fd);
/* [M23/M26] file positioning + stat. Layout mirrors struct secos_stat. */
struct stat {
    unsigned long st_size;
    unsigned st_mode;          /* full POSIX mode (S_IFMT | perms) */
    unsigned st_nlink;
    unsigned st_uid, st_gid;
    unsigned long st_atime, st_mtime, st_ctime;
};
#define S_IFMT  0xF000
#define S_IFREG 0x8000
#define S_IFDIR 0x4000
#define S_IFLNK 0xA000
#define S_IFCHR 0x2000
#define S_IFBLK 0x6000
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_RDWR   0x2
long    lseek(int fd, long offset, int whence);
int     stat(const char* path, struct stat* st);
/* [M26] VFS maturity: metadata + symlinks */
int     lstat(const char* path, struct stat* st);
int     chmod(const char* path, unsigned mode);
int     chown(const char* path, unsigned uid, unsigned gid);
int     utimes(const char* path, unsigned long atime, unsigned long mtime);
long    readlink(const char* path, char* buf, long len);
int     symlink(const char* target, const char* linkpath);
int     mount(const char* dev, const char* target, const char* fstype);
int     umount(const char* target);
void    _exit(int code) __attribute__((noreturn));
int     getpid(void);
void    sched_yield(void);

/* [M13] uptime + minimal IPC channels */
unsigned long getticks(void);                            /* uptime in timer ticks */
long    msg_send(int chan, const void* buf, long len);   /* bytes accepted, or <0 */
long    msg_recv(int chan, void* buf, long len);         /* blocks if empty (M17); bytes read or <0 */

/* [M16] process control */
int     spawn(const char* path, char* const argv[]);     /* signed child + argv -> pid, or <0 */
int     waitpid(int pid);                                /* block until child exits -> status */
/* [M17] blocking sleep */
void    sleep_ticks(unsigned ticks);

/* [M18] dynamic memory (POSIX-ish). PROT and MAP flags match the kernel ABI. */
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
void*   mmap(void* addr, size_t len, int prot, int flags);
void*   mmap_file(void* addr, size_t len, int prot, int flags, int fd); /* [M20] */
int     munmap(void* addr, size_t len);
int     mprotect(void* addr, size_t len, int prot);
void*   sbrk(long incr);
unsigned long brk_set(unsigned long addr);
void*   malloc(size_t size);
void    free(void* p);

/* [M19] copy-on-write fork */
int     fork(void);

/* [M25] anonymous pipe: fds[0]=read end, fds[1]=write end. 0 on success, -1 on error. */
int     pipe(int fds[2]);

/* [M24] BSD-style sockets (require CAP_NET in the signed manifest). ip is
 * network-order octets (octet0 in the low byte), port is host order. */
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
struct secos_sockaddr { unsigned int ip; unsigned short port; unsigned short _pad; };
int   socket(int type);
int   connect(int fd, unsigned int ip, unsigned short port);
int   bind(int fd, unsigned short port);
int   listen(int fd, int backlog);
int   accept(int fd);
long  send(int fd, const void* buf, long len);
long  recv(int fd, void* buf, long len);
long  sendto(int fd, const void* buf, long len, const struct secos_sockaddr* sa);
long  recvfrom(int fd, void* buf, long len, struct secos_sockaddr* sa);
int   sockclose(int fd);
unsigned int ip4(unsigned a, unsigned b, unsigned c, unsigned d);

/* tiny libc helpers */
size_t  strlen(const char* s);
int     puts(const char* s);   /* writes s + '\n' to stdout (fd 1) */

#endif /* LIBSECOS_H */
