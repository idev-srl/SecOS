/* SecOS - force-included shim for the BusyBox port (tools/port-busybox.sh).
 *
 * BusyBox is compiled against the SecOS libc (user/libc.c, user/libsecos.c)
 * plus the header overlay in user/busybox-include/ and the shim objects in
 * user/busybox_compat.c. This header runs before any BusyBox or libc header:
 *
 *  1. a few libc functions are re-labelled (asm labels, same C prototype) to
 *     sbb_* shims: read/write/close dispatch socket descriptors, and the
 *     path/fd calls set errno (several libsecos wrappers return the raw -1);
 *  2. the SecOS headers are then parsed, so the native prototypes that differ
 *     from POSIX (socket API, mmap, utimes, mount, ioctl, ...) are seen under
 *     their native names; after that, function-like macros route the POSIX
 *     spellings BusyBox uses to POSIX-shaped sbb_* shims;
 *  3. stdio is BusyBox-private (user/busybox-include/stdio.h, link names
 *     bbio_*): a buffered FILE with read buffering, getline, %m/%j/%z/%ll.
 *
 * SPDX-License-Identifier: MIT */
#ifndef SECOS_BUSYBOX_PORT_H
#define SECOS_BUSYBOX_PORT_H
#ifndef __ASSEMBLER__
#define SECOS_BUSYBOX_PORT 1
#define __secos__ 1
/* libsecos.h: hide its 2-arg open() and 1-arg waitpid() (POSIX ones below). */
#define SECOS_BASH_PORT 1

/* The overlay (net/if.h, netinet/in.h, ...) defines these structures itself;
 * keep the Linux UAPI headers used by `ip` (linux/rtnetlink.h, ...) from
 * redefining them (see linux/libc-compat.h). */
#define __UAPI_DEF_IF_IFCONF 0
#define __UAPI_DEF_IF_IFMAP 0
#define __UAPI_DEF_IF_IFREQ 0
#define __UAPI_DEF_IF_NET_DEVICE_FLAGS 0
#define __UAPI_DEF_IF_NET_DEVICE_FLAGS_LOWER_UP_DORMANT_ECHO 0
#define __UAPI_DEF_IN6_ADDR 0
#define __UAPI_DEF_IN6_ADDR_ALT 0
#define __UAPI_DEF_IN6_PKTINFO 0
#define __UAPI_DEF_IN_ADDR 0
#define __UAPI_DEF_IN_CLASS 0
#define __UAPI_DEF_IN_IPPROTO 0
#define __UAPI_DEF_IN_PKTINFO 0
#define __UAPI_DEF_IP6_MTUINFO 0
#define __UAPI_DEF_IPPROTO_V6 0
#define __UAPI_DEF_IPV6_MREQ 0
#define __UAPI_DEF_IPV6_OPTIONS 0
#define __UAPI_DEF_IP_MREQ 0
#define __UAPI_DEF_SOCKADDR_IN 0
#define __UAPI_DEF_SOCKADDR_IN6 0
#define __UAPI_DEF_XATTR 0
#define __UAPI_DEF_ETHHDR 0
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* ---- 1. asm-labelled overrides (declared before libsecos.h) ---- */
struct stat;
typedef long ssize_t;
ssize_t read(int fd, void* buf, size_t len)        __asm__("sbb_read");
ssize_t write(int fd, const void* buf, size_t len) __asm__("sbb_write");
int     close(int fd)                              __asm__("sbb_close");
int     dup(int oldfd)                             __asm__("sbb_dup");
int     dup2(int oldfd, int newfd)                 __asm__("sbb_dup2");
int     stat(const char* path, struct stat* st)    __asm__("sbb_stat");
int     lstat(const char* path, struct stat* st)   __asm__("sbb_lstat");
int     chmod(const char* path, unsigned mode)     __asm__("sbb_chmod");
int     chown(const char* path, unsigned u, unsigned g) __asm__("sbb_chown");
long    readlink(const char* path, char* buf, long len) __asm__("sbb_readlink");
int     symlink(const char* target, const char* linkpath) __asm__("sbb_symlink");
int     mkdir(const char* path, unsigned mode)     __asm__("sbb_mkdir");
int     unlink(const char* path)                   __asm__("sbb_unlink");
int     rmdir(const char* path)                    __asm__("sbb_rmdir");
int     chdir(const char* path)                    __asm__("sbb_chdir");
int     isatty(int fd)                             __asm__("sbb_isatty");
int     rename(const char* from, const char* to)   __asm__("sbb_rename");
int     remove(const char* path)                   __asm__("sbb_remove");
int     execvp(const char* file, char* const argv[]) __asm__("sbb_execvp");
int     execve(const char* path, char* const argv[], char* const envp[]) __asm__("sbb_execve");
int     execv(const char* path, char* const argv[]) __asm__("sbb_execv");
void    exit(int code) __asm__("bbio_exit") __attribute__((noreturn));
int     puts(const char* s)                        __asm__("bbio_puts");
char*   strerror(int errnum)                       __asm__("sbb_strerror");
/* libc opendir reads at most 64 entries and does not resolve the cwd */
#define opendir  sbb_opendir
#define readdir  sbb_readdir
#define closedir sbb_closedir
/* BusyBox's main() becomes sbb_main; busybox_compat.c owns the real entry so
 * the private stdio is flushed when an applet returns from main. */
#ifndef BB_COMPAT_IMPL
int main(int argc, char** argv) __asm__("sbb_main");
#endif

/* ---- 2. native SecOS declarations, then the POSIX-shaped routes ---- */
#include "libsecos.h"
#include <termios.h>

#define socket(d, t, p)             sbb_socket(d, t, p)
#define connect(f, a, l)            sbb_connect(f, a, l)
#define bind(f, a, l)               sbb_bind(f, a, l)
#define listen(f, b)                sbb_listen(f, b)
#define accept(f, a, l)             sbb_accept(f, a, l)
#define send(f, b, n, fl)           sbb_send(f, b, n, fl)
#define recv(f, b, n, fl)           sbb_recv(f, b, n, fl)
#define sendto(f, b, n, fl, a, l)   sbb_sendto(f, b, n, fl, a, l)
#define recvfrom(f, b, n, fl, a, l) sbb_recvfrom(f, b, n, fl, a, l)
#define mmap(a, l, p, f, fd, o)     sbb_mmap(a, l, p, f, fd, o)
#define utimes(p, tv)               sbb_utimes(p, tv)
#define mount(s, t, ty, f, d)       sbb_mount(s, t, ty, f, d)
#define umount(t)                   sbb_umount2(t, 0)
#define waitpid(p, s, o)            sbb_waitpid(p, s, o)
#define open(...)                   sbb_open(__VA_ARGS__)
#define ioctl(...)                  sbb_ioctl(__VA_ARGS__)
#define signal(s, h)                sbb_signal(s, h)
/* BusyBox has its own spawn(char**) (libbb); SecOS libc's spawn(path, argv)
 * keeps its link name, BusyBox's is renamed. */
#define spawn                       sbb_bb_spawn

#endif /* !__ASSEMBLER__ */
#endif /* SECOS_BUSYBOX_PORT_H */
