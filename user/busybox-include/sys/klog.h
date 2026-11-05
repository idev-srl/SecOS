/* SecOS BusyBox overlay: <sys/klog.h>. klogctl() reads /proc/kmsg when the
 * kernel provides it (TODO(integration): kmsg). */
#ifndef _BB_SYS_KLOG_H
#define _BB_SYS_KLOG_H
int klogctl(int type, char* buf, int len);
#endif
