/* SecOS Dropbear port: AF_UNIX address (SecOS has no unix sockets yet).
 * SPDX-License-Identifier: MIT */
#ifndef _DB_SYS_UN_H
#define _DB_SYS_UN_H
#include <sys/socket.h>
struct sockaddr_un { sa_family_t sun_family; char sun_path[108]; };
#endif
