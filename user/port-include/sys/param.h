#ifndef _PORT_SYS_PARAM_H
#define _PORT_SYS_PARAM_H
#define MAXPATHLEN 1024
#define NOFILE 32
#ifndef howmany
#define howmany(x,y) (((x)+((y)-1))/(y))
#endif
#endif
