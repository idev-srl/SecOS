/* SecOS BusyBox overlay: <netinet/udp.h>. */
#ifndef _BB_NETINET_UDP_H
#define _BB_NETINET_UDP_H
#include <stdint.h>
struct udphdr { uint16_t source, dest, len, check; };
#endif
