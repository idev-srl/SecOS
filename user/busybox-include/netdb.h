/* SecOS BusyBox overlay: <netdb.h> (numeric hosts + a small UDP DNS client in
 * the shim; /etc/hosts and /etc/resolv.conf are honoured if present).
 * SPDX-License-Identifier: MIT */
#ifndef _BB_NETDB_H
#define _BB_NETDB_H
#include <netinet/in.h>
struct hostent { char* h_name; char** h_aliases; int h_addrtype; int h_length; char** h_addr_list; };
#define h_addr h_addr_list[0]
struct servent { char* s_name; char** s_aliases; int s_port; char* s_proto; };
struct protoent { char* p_name; char** p_aliases; int p_proto; };
struct netent { char* n_name; char** n_aliases; int n_addrtype; uint32_t n_net; };
struct addrinfo { int ai_flags, ai_family, ai_socktype, ai_protocol; socklen_t ai_addrlen;
                  struct sockaddr* ai_addr; char* ai_canonname; struct addrinfo* ai_next; };
#define AI_PASSIVE     0x01
#define AI_CANONNAME   0x02
#define AI_NUMERICHOST 0x04
#define AI_V4MAPPED    0x08
#define AI_ALL         0x10
#define AI_ADDRCONFIG  0x20
#define AI_NUMERICSERV 0x400
#define NI_NUMERICHOST 0x01
#define NI_NUMERICSERV 0x02
#define NI_NOFQDN      0x04
#define NI_NAMEREQD    0x08
#define NI_DGRAM       0x10
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
#define EAI_OVERFLOW -12
#define HOST_NOT_FOUND 1
#define TRY_AGAIN 2
#define NO_RECOVERY 3
#define NO_DATA 4
extern int h_errno;
struct hostent* gethostbyname(const char* name);
struct hostent* gethostbyname2(const char* name, int af);
struct hostent* gethostbyaddr(const void* a, socklen_t l, int t);
int  getaddrinfo(const char* node, const char* svc, const struct addrinfo* hints, struct addrinfo** res);
void freeaddrinfo(struct addrinfo* ai);
const char* gai_strerror(int e);
int  getnameinfo(const struct sockaddr* sa, socklen_t sl, char* host, socklen_t hl, char* serv, socklen_t svl, int flags);
struct servent* getservbyname(const char* name, const char* proto);
struct servent* getservbyport(int port, const char* proto);
struct protoent* getprotobyname(const char* name);
struct protoent* getprotobynumber(int n);
const char* hstrerror(int e);
void herror(const char* s);
#endif
