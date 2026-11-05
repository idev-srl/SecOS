/* SecOS Dropbear port: syslog constants (no syslog daemon; Dropbear is built
 * with DISABLE_SYSLOG and logs to stderr with -E). SPDX-License-Identifier: MIT */
#ifndef _DB_SYSLOG_H
#define _DB_SYSLOG_H
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7
#define LOG_PID     0x01
#define LOG_CONS    0x02
#define LOG_NDELAY  0x08
#define LOG_DAEMON   (3<<3)
#define LOG_AUTH     (4<<3)
#define LOG_AUTHPRIV (10<<3)
void openlog(const char* ident, int opt, int facility);
void syslog(int pri, const char* fmt, ...);
void closelog(void);
#endif
