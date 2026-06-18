/* SecOS M16 parent: spawns a signed child with argv, blocks in waitpid() until
 * it exits, and prints the child's exit status. SPDX-License-Identifier: MIT */
#include "libsecos.h"

int main(void) {
    puts("[parent] spawning child with argv {alpha, beta}");
    char* av[] = { "/m16_child.elf", "alpha", "beta", 0 };
    int pid = spawn("/m16_child.elf", av);
    if (pid < 0) { puts("[parent] spawn FAILED"); return 1; }
    int st = waitpid(pid); /* blocks until the child exits */
    char m[40]; int i = 0;
    const char* p = "[parent] child status=";
    while (*p) m[i++] = *p++;
    if (st >= 100) m[i++] = (char)('0' + (st / 100) % 10);
    if (st >= 10)  m[i++] = (char)('0' + (st / 10) % 10);
    m[i++] = (char)('0' + st % 10);
    m[i++] = '\n';
    write(1, m, i);
    return 0;
}
