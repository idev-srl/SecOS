/* SecOS BusyBox overlay: <resolv.h> (resolver state used by the small
 * nslookup; the shim's DNS client queries _res.nsaddr_list[0]). */
#ifndef _BB_RESOLV_H
#define _BB_RESOLV_H
#include <netinet/in.h>
#define MAXNS 3
#define RES_INIT 1
#define RES_USE_INET6 0x2000
struct __res_state {
    int retrans, retry;
    unsigned long options;
    int nscount;
    struct sockaddr_in nsaddr_list[MAXNS];
    union { struct { unsigned short nscount; struct sockaddr_in6* nsaddrs[MAXNS]; } _ext; } _u;
};
extern struct __res_state _res;
int res_init(void);
#endif
