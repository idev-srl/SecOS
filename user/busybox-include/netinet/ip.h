/* SecOS BusyBox overlay: <netinet/ip.h>. */
#ifndef _BB_NETINET_IP_H
#define _BB_NETINET_IP_H
#include <stdint.h>
#include <netinet/in.h>
struct iphdr {
    unsigned int ihl:4, version:4;
    uint8_t tos; uint16_t tot_len, id, frag_off; uint8_t ttl, protocol; uint16_t check;
    uint32_t saddr, daddr;
};
struct ip {
    unsigned int ip_hl:4, ip_v:4;
    uint8_t ip_tos; uint16_t ip_len, ip_id, ip_off; uint8_t ip_ttl, ip_p; uint16_t ip_sum;
    struct in_addr ip_src, ip_dst;
};
#define IPVERSION 4
#define MAXTTL 255
#define IPDEFTTL 64
#define IP_MAXPACKET 65535
#define IPTOS_LOWDELAY 0x10
#define IPTOS_THROUGHPUT 0x08
#define IPTOS_RELIABILITY 0x04
#define IPOPT_EOL 0
#define IPOPT_NOP 1
#define IPOPT_RR 7
#define IPOPT_TS 68
#define IPOPT_OPTVAL 0
#define IPOPT_OLEN 1
#define IPOPT_OFFSET 2
#define IPOPT_MINOFF 4
#define MAX_IPOPTLEN 40
#define IP_DF 0x4000
#endif
