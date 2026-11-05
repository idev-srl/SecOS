/* SecOS BusyBox overlay: <arpa/inet.h>. */
#ifndef _BB_ARPA_INET_H
#define _BB_ARPA_INET_H
#include <netinet/in.h>
in_addr_t   inet_addr(const char* s);
int         inet_aton(const char* s, struct in_addr* a);
char*       inet_ntoa(struct in_addr a);
int         inet_pton(int af, const char* s, void* dst);
const char* inet_ntop(int af, const void* src, char* dst, socklen_t len);
in_addr_t   inet_network(const char* s);
struct in_addr inet_makeaddr(in_addr_t net, in_addr_t host);
in_addr_t   inet_lnaof(struct in_addr a);
in_addr_t   inet_netof(struct in_addr a);
#endif
