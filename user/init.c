/* init — SecOS minimal service manager (Phase K / M32).
 *
 * Spawns a table of signed services and supervises them: when a service exits it
 * is restarted, up to a per-service limit, then given up on. A real PID-1 init
 * would run forever; this bounded version proves the supervision loop and exits
 * cleanly (this OS has no Ctrl-C yet). Each service is a signed ELF, so the trust
 * boundary still holds — init can only launch what the signing key blessed.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include "libsecos.h"

struct svc { const char* path; int max_restarts; int restarts; int pid; };

static int start(struct svc* s) {
    char* av[2]; av[0] = (char*)s->path; av[1] = 0;
    s->pid = spawn(s->path, av);
    return s->pid;
}

int main(void) {
    struct svc svcs[] = {
        { "/bin/svc", 2, 0, -1 },
    };
    int n = (int)(sizeof(svcs) / sizeof(svcs[0]));
    printf("[init] SecOS service manager: starting %d service(s)\n", n);

    int alive = 0;
    for (int i = 0; i < n; i++) {
        if (start(&svcs[i]) > 0) { alive++; printf("[init] started %s pid=%d\n", svcs[i].path, svcs[i].pid); }
        else printf("[init] FAILED to start %s\n", svcs[i].path);
    }

    while (alive > 0) {
        for (int i = 0; i < n; i++) {
            if (svcs[i].pid < 0) continue;
            int status = waitpid(svcs[i].pid);          /* block until it exits */
            printf("[init] %s (pid=%d) exited status=%d\n", svcs[i].path, svcs[i].pid, status);
            if (svcs[i].restarts < svcs[i].max_restarts) {
                svcs[i].restarts++;
                if (start(&svcs[i]) > 0)
                    printf("[init] restart %d of %s -> pid=%d\n", svcs[i].restarts, svcs[i].path, svcs[i].pid);
                else { svcs[i].pid = -1; alive--; }
            } else {
                printf("[init] %s reached max restarts (%d); giving up\n", svcs[i].path, svcs[i].max_restarts);
                svcs[i].pid = -1; alive--;
            }
        }
    }
    printf("[init] all services stopped\n");
    return 0;
}
