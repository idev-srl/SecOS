/* SecOS BusyBox overlay: <net/route.h>. */
#ifndef _BB_NET_ROUTE_H
#define _BB_NET_ROUTE_H
#include <sys/socket.h>
struct rtentry { unsigned long rt_pad1; struct sockaddr rt_dst, rt_gateway, rt_genmask;
                 unsigned short rt_flags; short rt_pad2; unsigned long rt_pad3; void* rt_pad4;
                 short rt_metric; char* rt_dev; unsigned long rt_mtu, rt_window; unsigned short rt_irtt; };
#define RTF_UP 0x0001
#define RTF_GATEWAY 0x0002
#define RTF_HOST 0x0004
#define RTF_REINSTATE 0x0008
#define RTF_DYNAMIC 0x0010
#define RTF_MODIFIED 0x0020
#define RTF_MTU 0x0040
#define RTF_MSS RTF_MTU
#define RTF_WINDOW 0x0080
#define RTF_IRTT 0x0100
#define RTF_REJECT 0x0200
#endif
