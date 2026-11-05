/* SecOS Dropbear port: <netdb.h>. Numeric IPv4 + "localhost" only (no
 * resolver). TODO(integration): move to libc.  SPDX-License-Identifier: MIT */
#ifndef _DB_NETDB_H
#define _DB_NETDB_H
#include <sys/socket.h>
#include <netinet/in.h>
struct addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr* ai_addr;
    char* ai_canonname;
    struct addrinfo* ai_next;
};
#define AI_PASSIVE     0x01
#define AI_CANONNAME   0x02
#define AI_NUMERICHOST 0x04
#define AI_ADDRCONFIG  0x20
#define AI_NUMERICSERV 0x0400
#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
#define NI_NOFQDN      4
#define NI_NAMEREQD    8
#define NI_DGRAM       16
#define NI_MAXHOST 1025
#define NI_MAXSERV 32
#define EAI_BADFLAGS -1
#define EAI_NONAME   -2
#define EAI_AGAIN    -3
#define EAI_FAIL     -4
#define EAI_FAMILY   -6
#define EAI_SOCKTYPE -7
#define EAI_SERVICE  -8
#define EAI_MEMORY   -10
#define EAI_SYSTEM   -11
int  getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res);
void freeaddrinfo(struct addrinfo* ai);
int  getnameinfo(const struct sockaddr* sa, socklen_t salen, char* host, socklen_t hostlen,
                 char* serv, socklen_t servlen, int flags);
const char* gai_strerror(int err);
#endif
