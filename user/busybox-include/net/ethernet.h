/* SecOS BusyBox overlay: <net/ethernet.h>. */
#ifndef _BB_NET_ETHERNET_H
#define _BB_NET_ETHERNET_H
#include <stdint.h>
#include <sys/types.h>
#include <linux/if_ether.h>  /* ETH_ALEN, ETH_P_* (struct ethhdr is defined below) */
#define ETHERTYPE_IP  0x0800
#define ETHERTYPE_ARP 0x0806
struct ether_addr { uint8_t ether_addr_octet[ETH_ALEN]; };
struct ether_header { uint8_t ether_dhost[ETH_ALEN], ether_shost[ETH_ALEN]; uint16_t ether_type; };
struct ethhdr { unsigned char h_dest[ETH_ALEN], h_source[ETH_ALEN]; uint16_t h_proto; };
#define ETHER_ADDR_LEN ETH_ALEN
char* ether_ntoa(const struct ether_addr* a);
char* ether_ntoa_r(const struct ether_addr* a, char* buf);
struct ether_addr* ether_aton(const char* s);
struct ether_addr* ether_aton_r(const char* s, struct ether_addr* a);
int ether_hostton(const char* h, struct ether_addr* a);
#endif
