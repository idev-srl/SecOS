/*
 * dropbear_config.h - hand-written autoconf config.h for the SecOS Dropbear
 * port (replaces the host-probed one: configure cannot test SecOS). Copied to
 * <dropbear>/config.h by tools/port-dropbear.sh. Only what SecOS libc +
 * user/dropbear_compat.c actually provide is asserted here.
 * SPDX-License-Identifier: MIT
 */
#ifndef SECOS_DROPBEAR_CONFIG_H
#define SECOS_DROPBEAR_CONFIG_H

#define BUNDLED_LIBTOM 1
#define DROPBEAR_FUZZ 0
#define DROPBEAR_PLUGIN 0

/* No login accounting, no PAM, no syslog daemon, no zlib. */
#define DISABLE_LASTLOG 1
#define DISABLE_PAM 1
#define DISABLE_PUTUTLINE 1
#define DISABLE_PUTUTXLINE 1
#define DISABLE_SYSLOG 1
#define DISABLE_UTMP 1
#define DISABLE_UTMPX 1
#define DISABLE_WTMP 1
#define DISABLE_WTMPX 1
#define DISABLE_ZLIB 1

/* PTYs: Dropbear's /dev/ptmx + grantpt/unlockpt/ptsname path. Until the
 * kernel provides /dev/ptmx the open fails and Dropbear refuses the pty-req
 * cleanly (ssh falls back to no tty). See docs/SSH.md for the kernel spec. */
#define USE_DEV_PTMX 1

/* Functions (SecOS libc or user/dropbear_compat.c). */
#define HAVE_CLOCK_GETTIME 1
#define HAVE_FORK 1
#define HAVE_STRLCPY 1
#define HAVE_PUTENV 1
#define HAVE_GETADDRINFO 1
#define HAVE_FREEADDRINFO 1
#define HAVE_GETNAMEINFO 1
#define HAVE_GAI_STRERROR 1
#define HAVE_CONST_GAI_STRERROR_PROTO 1
/* basename/daemon/getusershell (/etc/shells) come from Dropbear's own compat.c. */
#undef HAVE_GETUSERSHELL
#undef HAVE_BASENAME
#undef HAVE_DAEMON
#define HAVE_WRITEV 1          /* compat: a write() loop */
#undef HAVE_GETRANDOM
#undef HAVE_CLEARENV
#undef HAVE_EXPLICIT_BZERO
/* [M42] Real accounts: the SecOS libc's credentials (kernel/cred.c), the
 * /etc/shadow parser and crypt() from user/libcrypt.a (musl SHA-crypt/bcrypt). */
#define HAVE_GETGROUPLIST 1
#define HAVE_SETRESGID 1
#define HAVE_CRYPT 1
#define HAVE_CRYPT_H 1
#define HAVE_SHADOW_H 1
#undef HAVE_FEXECVE
#undef HAVE_GETPASS
#undef HAVE_OPENPTY

/* Types. */
#define HAVE_STRUCT_ADDRINFO 1
#define HAVE_STRUCT_SOCKADDR_STORAGE 1
#define HAVE_STRUCT_SOCKADDR_STORAGE_SS_FAMILY 1
#define HAVE_STRUCT_IN6_ADDR 1
#define HAVE_STRUCT_SOCKADDR_IN6 1
#define HAVE_UINT8_T 1
#define HAVE_UINT16_T 1
#define HAVE_UINT32_T 1
#define HAVE_U_INT8_T 1
#define HAVE_U_INT16_T 1
#define HAVE_U_INT32_T 1
#define HAVE_UNDERSCORE_STATIC_ASSERT 1

/* Headers (SecOS libc + user/dropbear-include + user/port-include). */
#define HAVE_INTTYPES_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_WAIT_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1

#define SELECT_TYPE_ARG1 int
#define SELECT_TYPE_ARG234 (fd_set *)
#define SELECT_TYPE_ARG5 (struct timeval *)

#define PACKAGE_BUGREPORT ""
#define PACKAGE_NAME ""
#define PACKAGE_STRING ""
#define PACKAGE_TARNAME ""
#define PACKAGE_URL ""
#define PACKAGE_VERSION ""

#endif /* SECOS_DROPBEAR_CONFIG_H */
