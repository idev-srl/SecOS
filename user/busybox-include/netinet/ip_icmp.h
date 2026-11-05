/* SecOS BusyBox overlay: <netinet/ip_icmp.h>. */
#ifndef _BB_NETINET_IP_ICMP_H
#define _BB_NETINET_IP_ICMP_H
#include <stdint.h>
#include <netinet/ip.h>
struct icmphdr {
    uint8_t type, code; uint16_t checksum;
    union { struct { uint16_t id, sequence; } echo; uint32_t gateway; struct { uint16_t __unused, mtu; } frag; } un;
};
struct icmp {
    uint8_t icmp_type, icmp_code; uint16_t icmp_cksum;
    union { struct { uint16_t icd_id, icd_seq; } ih_idseq; uint32_t ih_void; struct in_addr ih_gwaddr; } icmp_hun;
    union { struct { uint32_t its_otime, its_rtime, its_ttime; } id_ts; struct ip id_ip; uint8_t id_data[1]; } icmp_dun;
};
#define icmp_id    icmp_hun.ih_idseq.icd_id
#define icmp_seq   icmp_hun.ih_idseq.icd_seq
#define icmp_gwaddr icmp_hun.ih_gwaddr
#define icmp_data  icmp_dun.id_data
#define icmp_ip    icmp_dun.id_ip
#define ICMP_MINLEN 8
#define ICMP_ECHOREPLY 0
#define ICMP_DEST_UNREACH 3
#define ICMP_UNREACH 3
#define ICMP_SOURCE_QUENCH 4
#define ICMP_REDIRECT 5
#define ICMP_ECHO 8
#define ICMP_TIME_EXCEEDED 11
#define ICMP_TIMXCEED 11
#define ICMP_PARAMETERPROB 12
#define ICMP_TIMESTAMP 13
#define ICMP_TIMESTAMPREPLY 14
#define ICMP_INFO_REQUEST 15
#define ICMP_INFO_REPLY 16
#define ICMP_ADDRESS 17
#define ICMP_ADDRESSREPLY 18
#define ICMP_UNREACH_NET 0
#define ICMP_UNREACH_HOST 1
#define ICMP_UNREACH_PROTOCOL 2
#define ICMP_UNREACH_PORT 3
#define ICMP_UNREACH_NEEDFRAG 4
#define ICMP_UNREACH_SRCFAIL 5
#endif
