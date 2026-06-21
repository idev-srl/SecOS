#ifndef _PORT_NETINET_IN_H
#define _PORT_NETINET_IN_H
struct in_addr { unsigned int s_addr; };
struct sockaddr_in { short sin_family; unsigned short sin_port; struct in_addr sin_addr; char pad[8]; };
#endif
