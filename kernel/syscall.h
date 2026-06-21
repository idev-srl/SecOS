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
// [M26] VFS maturity: metadata, symlinks, mount control
#define SYS_CHMOD    32 // chmod(path, mode)
#define SYS_CHOWN    33 // chown(path, uid, gid)
#define SYS_UTIMES   34 // utimes(path, atime, mtime)  (unix seconds)
#define SYS_READLINK 35 // readlink(path, buf, len) -> length
#define SYS_SYMLINK  36 // symlink(target, linkpath)
#define SYS_LSTAT    37 // lstat(path, statbuf)  (does not follow a final symlink)
#define SYS_MOUNT    38 // mount(dev, target, fstype)
#define SYS_UMOUNT   39 // umount(target)
// [M31] file management (for a real userland / coreutils)
#define SYS_GETDENTS 40 // getdents(path, buf, buflen) -> packed dir entries, bytes
#define SYS_CREATE   41 // create(path) -> create an empty file, 0/-1
#define SYS_MKDIR    42 // mkdir(path) -> 0/-1
#define SYS_UNLINK   43 // unlink(path) -> remove a file/dir, 0/-1
// [M30] Signals + job control
#define SYS_SIGACTION   44 // sigaction(sig, handler, restorer) -> 0/-1
#define SYS_SIGRETURN   45 // sigreturn() -> restore the interrupted context (no return value)
#define SYS_KILL        46 // kill(pid, sig) -> 0/-1 (pid<=0 = caller's process group)
#define SYS_SIGPROCMASK 47 // sigprocmask(how, set, oldset*) -> 0/-1
#define SYS_SETPGID     48 // setpgid(pid, pgid) -> 0/-1 (0 = use the target's own pid)

// [M39] POSIX shell-from-source foundation (dup2/cwd/ioctl/exec/env).
#define SYS_DUP2        49 // dup2(oldfd, newfd) -> newfd / -1
#define SYS_DUP         50 // dup(oldfd) -> lowest free fd / -1
#define SYS_CHDIR       51 // chdir(path) -> 0/-1 (sets per-process cwd)
#define SYS_GETCWD      52 // getcwd(buf, len) -> length / -1
#define SYS_IOCTL       53 // ioctl(fd, cmd, arg) -> 0/-1 (termios TCGETS/TCSETS, TIOCGWINSZ)
#define SYS_GETPPID     54 // getppid() -> parent pid
#define SYS_WAITANY     55 // waitany(int* status_out, int options) -> child pid / 0 / -ECHILD
// Note: execve is emulated in libc (spawn+waitpid+_exit) — no kernel syscall.

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

// [M23/M26] stat result handed to user space (ABI struct; libc mirrors this).
// st_size@0 and st_mode@8 kept stable; M26 appended owner/timestamps. st_mode is
// now the full POSIX mode (S_IFMT type bits | permission bits).
struct secos_stat {
    uint64_t st_size;   // size in bytes              (offset 0)
    uint32_t st_mode;   // S_IFMT | perms             (offset 8)
    uint32_t st_nlink;  // hard-link count            (offset 12, was st_pad)
    uint32_t st_uid;    // owner uid                  (offset 16)
    uint32_t st_gid;    // owner gid                  (offset 20)
    uint64_t st_atime;  // access time (unix seconds) (offset 24)
    uint64_t st_mtime;  // modify time                (offset 32)
    uint64_t st_ctime;  // change time                (offset 40)
};

// [M26] POSIX mode helpers (S_IFMT mask + type bits; perms in the low 12 bits).
#define S_IFMT   0xF000
#define S_IFLNK  0xA000
#define S_IFSOCK 0xC000
#define S_IFCHR  0x2000
#define S_IFBLK  0x6000
#define S_IFIFO  0x1000

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
int  ksys_lstat(const char* path, struct secos_stat* st);   // [M26] no symlink follow
// [M25] anonymous pipe: allocate a pipe, return two fds (kfds[0]=read, kfds[1]=write)
int  ksys_pipe(int kfds[2]);
// [M26] metadata + symlinks + mount control (kernel-side, args already copied in)
int  ksys_chmod(const char* path, uint32_t mode);
int  ksys_chown(const char* path, uint32_t uid, uint32_t gid);
int  ksys_utimes(const char* path, uint64_t atime, uint64_t mtime);
int  ksys_readlink(const char* path, char* buf, int len);
int  ksys_symlink(const char* target, const char* linkpath);
int  ksys_mount(const char* dev, const char* target, const char* fstype);
int  ksys_umount(const char* target);

// Driver interface forward declaration (struct defined in driver_if.h)
struct driver_call;
int driver_syscall(struct driver_call* req);
