/*
 * klog.h — kernel log ring buffer (printk-like, for dmesg).
 *
 * Every byte the kernel writes to the ISA debugcon (debugcon_putchar) is also
 * captured here, line by line. Each completed line is stored as one syslog
 * record "<L>[sssss.uuuuuu] text\n": L is a syslog level inferred from the text
 * ([PANIC] -> 0, [EXC] -> 2, FAIL/[KILL] -> 3, WARN -> 4, else 6) and the stamp
 * is the monotonic time the line started. User programs' console output does
 * not go through here (ksys_write uses debugcon_putchar_raw), so dmesg shows the
 * kernel's messages only, as on Linux. Exposed via SYS_SYSLOG (klogctl), the
 * /proc/kmsg snapshot and /dev/kmsg (one Linux kmsg record per read()).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SECOS_KLOG_H
#define SECOS_KLOG_H
#include <stdint.h>
#include <stddef.h>

#define KLOG_BUF_SIZE (128u * 1024u)

/* syslog(2) / klogctl actions (Linux values). */
#define KLOG_ACTION_CLOSE         0
#define KLOG_ACTION_OPEN          1
#define KLOG_ACTION_READ          2
#define KLOG_ACTION_READ_ALL      3
#define KLOG_ACTION_READ_CLEAR    4
#define KLOG_ACTION_CLEAR         5
#define KLOG_ACTION_CONSOLE_OFF   6
#define KLOG_ACTION_CONSOLE_ON    7
#define KLOG_ACTION_CONSOLE_LEVEL 8
#define KLOG_ACTION_SIZE_UNREAD   9
#define KLOG_ACTION_SIZE_BUFFER   10

/* Capture one byte (called from debugcon_putchar). Safe in any context. */
void klog_putc(char c);

/* Snapshot window [*start, *end) of absolute stream positions holding whole
 * records. from_clear: start at the CLEAR mark (READ_ALL), else at the oldest
 * byte still in the ring. */
void klog_window(int from_clear, uint64_t* start, uint64_t* end);
/* Copy bytes of absolute positions [pos, pos+len) still in the ring. Returns the
 * number copied (short when part of the range was overwritten / not written). */
size_t klog_copy(uint64_t pos, char* dst, size_t len);
/* Mark everything logged so far as cleared (dmesg -c). */
void klog_clear(void);
/* Destructive-reader cursor (SYSLOG_ACTION_READ / SIZE_UNREAD). */
uint64_t klog_read_pos(void);
void     klog_set_read_pos(uint64_t pos);
/* /dev/kmsg: the Linux kmsg record at stream offset `off` (see klog.c). */
int      klog_kmsg_read(uint64_t off, char* out, size_t len);

#endif
