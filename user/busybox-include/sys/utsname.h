/* SecOS BusyBox overlay: <sys/utsname.h>. SPDX-License-Identifier: MIT */
#ifndef _BB_SYS_UTSNAME_H
#define _BB_SYS_UTSNAME_H
struct utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };
int uname(struct utsname* u);
#endif
