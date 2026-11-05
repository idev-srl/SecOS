/* SecOS BusyBox overlay: <netpacket/packet.h>. */
#ifndef _BB_NETPACKET_PACKET_H
#define _BB_NETPACKET_PACKET_H
struct sockaddr_ll { unsigned short sll_family, sll_protocol; int sll_ifindex; unsigned short sll_hatype;
                     unsigned char sll_pkttype, sll_halen, sll_addr[8]; };
#define PACKET_HOST 0
#define PACKET_BROADCAST 1
#define PACKET_MULTICAST 2
#define PACKET_OTHERHOST 3
#define PACKET_OUTGOING 4
#endif
