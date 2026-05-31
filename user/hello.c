/* SecOS user program — proves the signed-userland path end to end.
 * SPDX-License-Identifier: MIT */
#include "libsecos.h"

int main(void) {
    puts("[hello] signed SecOS user program running in ring 3");
    int pid = getpid();
    char msg[16];
    int i = 0;
    msg[i++] = '[';
    msg[i++] = 'h';
    msg[i++] = ']';
    msg[i++] = ' ';
    msg[i++] = 'p';
    msg[i++] = 'i';
    msg[i++] = 'd';
    msg[i++] = '=';
    msg[i++] = (char)('0' + (pid % 10));
    msg[i++] = '\n';
    write(1, msg, i);
    return 0;
}
