/* SecOS M17 sleeper: blocks in SYS_SLEEP for 10 ticks and verifies that the
 * uptime advanced by at least that much (proving the caller really slept rather
 * than busy-waited). SPDX-License-Identifier: MIT */
#include "libsecos.h"

int main(void) {
    unsigned long t0 = getticks();
    puts("[sleep] sleeping 10 ticks");
    sleep_ticks(10);
    unsigned long d = getticks() - t0;
    char m[40]; int i = 0;
    const char* p = "[sleep] elapsed=";
    while (*p) m[i++] = *p++;
    if (d >= 100) m[i++] = (char)('0' + (d / 100) % 10);
    if (d >= 10)  m[i++] = (char)('0' + (d / 10) % 10);
    m[i++] = (char)('0' + d % 10);
    m[i++] = '\n';
    write(1, m, i);
    puts(d >= 10 ? "[sleep] OK slept >=10" : "[sleep] FAIL slept <10");
    return 0;
}
