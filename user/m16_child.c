/* SecOS M16 child: receives argv from its parent, prints it, and exits with a
 * status equal to argc (proving exit-status delivery). SPDX-License-Identifier: MIT */
#include "libsecos.h"

int main(int argc, char** argv) {
    char m[32]; int i = 0;
    const char* p = "[child] argc=";
    while (*p) m[i++] = *p++;
    m[i++] = (char)('0' + (argc % 10));
    m[i++] = '\n';
    write(1, m, i);
    for (int k = 0; k < argc; k++) {
        write(1, "[child] arg: ", 13);
        puts(argv[k] ? argv[k] : "(null)");
    }
    return argc; /* exit status = argc */
}
