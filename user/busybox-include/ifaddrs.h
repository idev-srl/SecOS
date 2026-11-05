/* SecOS BusyBox overlay: <ifaddrs.h>. */
#ifndef _BB_IFADDRS_H
#define _BB_IFADDRS_H
#include <sys/socket.h>
struct ifaddrs { struct ifaddrs* ifa_next; char* ifa_name; unsigned ifa_flags;
                 struct sockaddr* ifa_addr, *ifa_netmask, *ifa_broadaddr; void* ifa_data; };
#define ifa_dstaddr ifa_broadaddr
int  getifaddrs(struct ifaddrs** ifap);
void freeifaddrs(struct ifaddrs* ifa);
#endif
