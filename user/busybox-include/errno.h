/* SecOS BusyBox overlay: <errno.h> = SecOS libc + the full Linux errno set
 * (same numbering; the kernel returns -errno). SPDX-License-Identifier: MIT */
#ifndef _BB_ERRNO_H
#define _BB_ERRNO_H
#include_next <errno.h>
#ifndef ENXIO
#define ENXIO 6
#endif
#ifndef E2BIG
#define E2BIG 7
#endif
#ifndef ENOEXEC
#define ENOEXEC 8
#endif
#ifndef ENOTBLK
#define ENOTBLK 15
#endif
#ifndef EBUSY
#define EBUSY 16
#endif
#ifndef EXDEV
#define EXDEV 18
#endif
#ifndef ENODEV
#define ENODEV 19
#endif
#ifndef ENFILE
#define ENFILE 23
#endif
#ifndef ETXTBSY
#define ETXTBSY 26
#endif
#ifndef EFBIG
#define EFBIG 27
#endif
#ifndef ESPIPE
#define ESPIPE 29
#endif
#ifndef EROFS
#define EROFS 30
#endif
#ifndef EMLINK
#define EMLINK 31
#endif
#ifndef EDOM
#define EDOM 33
#endif
#ifndef EDEADLK
#define EDEADLK 35
#endif
#ifndef ENAMETOOLONG
#define ENAMETOOLONG 36
#endif
#ifndef ENOLCK
#define ENOLCK 37
#endif
#ifndef ENOTEMPTY
#define ENOTEMPTY 39
#endif
#ifndef ELOOP
#define ELOOP 40
#endif
#define EWOULDBLOCK EAGAIN
#ifndef ENOMSG
#define ENOMSG 42
#endif
#ifndef ENOSTR
#define ENOSTR 60
#endif
#ifndef ENODATA
#define ENODATA 61
#endif
#ifndef ETIME
#define ETIME 62
#endif
#ifndef ENOLINK
#define ENOLINK 67
#endif
#ifndef EPROTO
#define EPROTO 71
#endif
#ifndef EBADMSG
#define EBADMSG 74
#endif
#ifndef EOVERFLOW
#define EOVERFLOW 75
#endif
#ifndef EILSEQ
#define EILSEQ 84
#endif
#ifndef ENOTSOCK
#define ENOTSOCK 88
#endif
#ifndef EDESTADDRREQ
#define EDESTADDRREQ 89
#endif
#ifndef EMSGSIZE
#define EMSGSIZE 90
#endif
#ifndef EPROTOTYPE
#define EPROTOTYPE 91
#endif
#ifndef ENOPROTOOPT
#define ENOPROTOOPT 92
#endif
#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT 93
#endif
#ifndef ESOCKTNOSUPPORT
#define ESOCKTNOSUPPORT 94
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP 95
#endif
#define ENOTSUP EOPNOTSUPP
#ifndef EPFNOSUPPORT
#define EPFNOSUPPORT 96
#endif
#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT 97
#endif
#ifndef EADDRINUSE
#define EADDRINUSE 98
#endif
#ifndef EADDRNOTAVAIL
#define EADDRNOTAVAIL 99
#endif
#ifndef ENETDOWN
#define ENETDOWN 100
#endif
#ifndef ENETUNREACH
#define ENETUNREACH 101
#endif
#ifndef ENETRESET
#define ENETRESET 102
#endif
#ifndef ECONNABORTED
#define ECONNABORTED 103
#endif
#ifndef ECONNRESET
#define ECONNRESET 104
#endif
#ifndef ENOBUFS
#define ENOBUFS 105
#endif
#ifndef EISCONN
#define EISCONN 106
#endif
#ifndef ENOTCONN
#define ENOTCONN 107
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED 111
#endif
#ifndef EHOSTDOWN
#define EHOSTDOWN 112
#endif
#ifndef EHOSTUNREACH
#define EHOSTUNREACH 113
#endif
#ifndef EALREADY
#define EALREADY 114
#endif
#ifndef EINPROGRESS
#define EINPROGRESS 115
#endif
#ifndef ESTALE
#define ESTALE 116
#endif
#ifndef EDQUOT
#define EDQUOT 122
#endif
#ifndef ECANCELED
#define ECANCELED 125
#endif
#endif
