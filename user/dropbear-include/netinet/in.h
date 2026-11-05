/* SecOS Dropbear port: <netinet/in.h>. SPDX-License-Identifier: MIT */
#ifndef _DB_NETINET_IN_H
#define _DB_NETINET_IN_H
#define _PORT_NETINET_IN_H   /* shadow the bash-port stub */
#include <stdint.h>
#include <sys/socket.h>
typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;
struct in_addr { in_addr_t s_addr; };
struct sockaddr_in { sa_family_t sin_family; in_port_t sin_port; struct in_addr sin_addr; unsigned char sin_zero[8]; };
struct in6_addr { union { uint8_t u6_addr8[16]; uint32_t u6_addr32[4]; } in6_u; };
#define s6_addr in6_u.u6_addr8
struct sockaddr_in6 { sa_family_t sin6_family; in_port_t sin6_port; uint32_t sin6_flowinfo; struct in6_addr sin6_addr; uint32_t sin6_scope_id; };
#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define IPPROTO_IPV6 41
#define IP_TOS       1
#define IPV6_V6ONLY  26
#define IPV6_TCLASS  67
#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)
#define INADDR_NONE      ((in_addr_t)0xffffffff)
#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46
#define IN6_IS_ADDR_V4MAPPED(a) 0
static inline uint16_t db_bswap16(uint16_t x){ return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint32_t db_bswap32(uint32_t x){ return __builtin_bswap32(x); }
#define htons(x) db_bswap16((uint16_t)(x))
#define ntohs(x) db_bswap16((uint16_t)(x))
#define htonl(x) db_bswap32((uint32_t)(x))
#define ntohl(x) db_bswap32((uint32_t)(x))
#endif
