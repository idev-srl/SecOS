/* SecOS BusyBox overlay: <netinet/in.h>. SPDX-License-Identifier: MIT */
#ifndef _BB_NETINET_IN_H
#define _BB_NETINET_IN_H
#include <stdint.h>
#include <sys/socket.h>
#include <endian.h>
typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;
struct in_addr { in_addr_t s_addr; };
struct sockaddr_in { sa_family_t sin_family; in_port_t sin_port; struct in_addr sin_addr;
                     unsigned char sin_zero[8]; };
struct in6_addr { union { uint8_t __s6_addr[16]; uint16_t __s6_addr16[8]; uint32_t __s6_addr32[4]; } __in6_union; };
#define s6_addr   __in6_union.__s6_addr
#define s6_addr16 __in6_union.__s6_addr16
#define s6_addr32 __in6_union.__s6_addr32
struct sockaddr_in6 { sa_family_t sin6_family; in_port_t sin6_port; uint32_t sin6_flowinfo;
                      struct in6_addr sin6_addr; uint32_t sin6_scope_id; };
struct ip_mreq { struct in_addr imr_multiaddr, imr_interface; };
struct ip_mreqn { struct in_addr imr_multiaddr, imr_address; int imr_ifindex; };
struct in_pktinfo { int ipi_ifindex; struct in_addr ipi_spec_dst, ipi_addr; };
extern const struct in6_addr in6addr_any, in6addr_loopback;
#define IN6ADDR_ANY_INIT { { { 0 } } }
#define IN6_IS_ADDR_V4MAPPED(a) 0
#define IN6_IS_ADDR_UNSPECIFIED(a) 0
#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_NONE      ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)
#define INADDR_ALLHOSTS_GROUP ((in_addr_t)0xe0000001)
#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46
#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_IGMP    2
#define IPPROTO_IPIP    4
#define IPPROTO_TCP     6
#define IPPROTO_EGP     8
#define IPPROTO_PUP     12
#define IPPROTO_IDP     22
#define IPPROTO_GRE     47
#define IPPROTO_ESP     50
#define IPPROTO_AH      51
#define IPPROTO_SCTP    132
#define IPPROTO_UDP     17
#define IPPROTO_IPV6    41
#define IPPROTO_ICMPV6  58
#define IPPROTO_RAW     255
#define IP_TOS          1
#define IP_TTL          2
#define IP_HDRINCL      3
#define IP_OPTIONS      4
#define IP_RECVOPTS     6
#define IP_RETOPTS      7
#define IP_PKTINFO      8
#define IP_MTU_DISCOVER 10
#define IP_RECVERR      11
#define IP_RECVTTL      12
#define IP_MULTICAST_IF   32
#define IP_MULTICAST_TTL  33
#define IP_MULTICAST_LOOP 34
#define IP_ADD_MEMBERSHIP 35
#define IP_PMTUDISC_DONT 0
#define IP_PMTUDISC_DO   2
#define IPV6_V6ONLY 26
#define IPV6_UNICAST_HOPS 16
#define IPV6_PKTINFO 50
#define IPV6_RECVPKTINFO 49
#define IPV6_HOPLIMIT 52
#define IPV6_RECVHOPLIMIT 51
#define IN_CLASSA_NET 0xff000000
#define IN_CLASSD(a) ((((in_addr_t)(a)) & 0xf0000000) == 0xe0000000)
#define IN_MULTICAST(a) IN_CLASSD(a)
#define IN_BADCLASS(a) ((((in_addr_t)(a)) & 0xf0000000) == 0xf0000000)
#define IN_LOOPBACKNET 127
#define htons(x) __builtin_bswap16((uint16_t)(x))
#define ntohs(x) __builtin_bswap16((uint16_t)(x))
#define htonl(x) __builtin_bswap32((uint32_t)(x))
#define ntohl(x) __builtin_bswap32((uint32_t)(x))
int bindresvport(int fd, struct sockaddr_in* a);
#endif
