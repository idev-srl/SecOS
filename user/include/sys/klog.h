/* <sys/klog.h> — SecOS libc: the kernel log (SYS_SYSLOG 68, klogctl actions).
 * SPDX-License-Identifier: MIT */
#ifndef _SYS_KLOG_H
#define _SYS_KLOG_H
#define SYSLOG_ACTION_CLOSE         0
#define SYSLOG_ACTION_OPEN          1
#define SYSLOG_ACTION_READ          2
#define SYSLOG_ACTION_READ_ALL      3
#define SYSLOG_ACTION_READ_CLEAR    4
#define SYSLOG_ACTION_CLEAR         5
#define SYSLOG_ACTION_CONSOLE_OFF   6
#define SYSLOG_ACTION_CONSOLE_ON    7
#define SYSLOG_ACTION_CONSOLE_LEVEL 8
#define SYSLOG_ACTION_SIZE_UNREAD   9
#define SYSLOG_ACTION_SIZE_BUFFER   10
int klogctl(int type, char* buf, int len);
#endif
