/* SecOS Dropbear port: <arpa/inet.h> (IPv4 only). TODO(integration): move to libc.
 * SPDX-License-Identifier: MIT */
#ifndef _DB_ARPA_INET_H
#define _DB_ARPA_INET_H
#define _PORT_ARPA_INET_H
#include <netinet/in.h>
in_addr_t   db_inet_addr(const char* s);
char*       db_inet_ntoa(struct in_addr in);
int         inet_pton(int af, const char* src, void* dst);
const char* inet_ntop(int af, const void* src, char* dst, socklen_t size);
int         inet_aton(const char* s, struct in_addr* out);
#define inet_addr(s) db_inet_addr(s)
#define inet_ntoa(a) db_inet_ntoa(a)
#endif
