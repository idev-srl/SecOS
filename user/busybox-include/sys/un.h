/* SecOS BusyBox overlay: <sys/un.h> (no AF_UNIX on SecOS; types only). */
#ifndef _BB_SYS_UN_H
#define _BB_SYS_UN_H
#include <sys/socket.h>
struct sockaddr_un { sa_family_t sun_family; char sun_path[108]; };
#endif
