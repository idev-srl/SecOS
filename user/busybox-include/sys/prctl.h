/* SecOS BusyBox overlay: <sys/prctl.h>. */
#ifndef _BB_SYS_PRCTL_H
#define _BB_SYS_PRCTL_H
#define PR_SET_PDEATHSIG 1
#define PR_GET_PDEATHSIG 2
#define PR_SET_NAME      15
#define PR_GET_NAME      16
#define PR_SET_NO_NEW_PRIVS 38
int prctl(int op, ...);
#endif
