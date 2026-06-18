/* SecOS M19: copy-on-write fork. The parent fills a static buffer, forks, and
 * the child mutates the buffer — which must NOT be visible in the parent (COW
 * isolation). Also proves fork()==0 in the child, the child inherits the parent's
 * memory, and the parent reads the child's exit status via waitpid.
 * SPDX-License-Identifier: MIT */
#include "libsecos.h"

static char buf[64];

int main(void) {
    puts("[m19] fork/COW test");
    for (int i = 0; i < 64; i++) buf[i] = 'P';      /* parent writes before fork */

    int pid = fork();
    if (pid < 0) { puts("[m19] fork FAILED"); return 1; }

    if (pid == 0) {
        /* child: inherited the parent's 'P', then privately mutates to 'C'. */
        puts(buf[0] == 'P' ? "[m19] child inherited P OK" : "[m19] child inherited FAIL");
        for (int i = 0; i < 64; i++) buf[i] = 'C';
        puts(buf[0] == 'C' ? "[m19] child wrote C OK" : "[m19] child wrote FAIL");
        _exit(7);
    }

    /* parent: wait for the child, then confirm its own buffer is untouched. */
    int st = waitpid(pid);
    char m[40]; int i = 0;
    const char* p = "[m19] parent: child status=";
    while (*p) m[i++] = *p++;
    m[i++] = (char)('0' + (st % 10));
    m[i++] = '\n';
    write(1, m, i);
    puts(buf[0] == 'P' ? "[m19] parent buf isolated OK" : "[m19] parent buf CHANGED FAIL");

    puts("[m19] DONE-USER");
    return 0;
}
