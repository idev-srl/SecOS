/* SecOS M13 IPC producer: sends a message on kernel channel 0 and reports its
 * uptime (SYS_GETTICKS). SPDX-License-Identifier: MIT */
#include "libsecos.h"

static int put_str(char* b, int i, const char* s) { while (*s) b[i++] = *s++; return i; }
static int put_ulong(char* b, int i, unsigned long v) {
    char t[24]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) b[i++] = t[--n];
    return i;
}

int main(void) {
    puts("[ipc_send] producer running");
    const char* msg = "M13-IPC-OK";
    long n = msg_send(0, msg, 10);

    char line[96]; int i = 0;
    i = put_str(line, i, "[ipc_send] sent="); i = put_ulong(line, i, (unsigned long)n);
    i = put_str(line, i, " ticks="); i = put_ulong(line, i, getticks());
    line[i++] = '\n';
    write(1, line, i);
    return 0;
}
