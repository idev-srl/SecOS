/* SecOS poweroff / reboot / halt / shutdown: one multi-call program,
 * dispatching on argv[0], over SYS_REBOOT (orderly: the kernel terminates every
 * other process, syncs and unmounts the filesystems, then powers off / resets /
 * halts). Installed as /bin/{poweroff,reboot,halt,shutdown}.
 *
 *   poweroff | reboot | halt         [-f] (accepted, ignored: always orderly)
 *   shutdown [-h|-P|-H|-r] [now|+0]  (-h/-P poweroff, -H halt, -r reboot)
 *
 * SPDX-License-Identifier: MIT */
#include "libsecos.h"
#include <stdio.h>
#include <string.h>

static const char* base(const char* p) {
    const char* b = p;
    for (; *p; p++) if (*p == '/') b = p + 1;
    return b;
}

static int usage_shutdown(void) {
    dprintf(2, "usage: shutdown [-h|-P|-H|-r] [now]\n"
               "  -h, -P  power off (default)   -H  halt   -r  reboot\n");
    return 1;
}

int main(int argc, char** argv) {
    const char* me = (argc > 0 && argv[0]) ? base(argv[0]) : "poweroff";
    int cmd = SECOS_REBOOT_POWEROFF;

    if (strcmp(me, "shutdown") == 0) {
        int when_ok = 1;
        for (int i = 1; i < argc; i++) {
            const char* a = argv[i];
            if (strcmp(a, "-h") == 0 || strcmp(a, "-P") == 0 || strcmp(a, "--poweroff") == 0) cmd = SECOS_REBOOT_POWEROFF;
            else if (strcmp(a, "-H") == 0 || strcmp(a, "--halt") == 0)   cmd = SECOS_REBOOT_HALT;
            else if (strcmp(a, "-r") == 0 || strcmp(a, "--reboot") == 0) cmd = SECOS_REBOOT_RESTART;
            else if (strcmp(a, "now") == 0 || strcmp(a, "+0") == 0) when_ok = 1;
            else if (strcmp(a, "--help") == 0) return usage_shutdown();
            else if (a[0] == '-') { dprintf(2, "shutdown: unknown option '%s'\n", a); return usage_shutdown(); }
            else {
                dprintf(2, "shutdown: only 'now' is supported (got '%s')\n", a);
                when_ok = 0;
            }
        }
        if (!when_ok) return 1;
    } else if (strcmp(me, "reboot") == 0) {
        cmd = SECOS_REBOOT_RESTART;
    } else if (strcmp(me, "halt") == 0) {
        cmd = SECOS_REBOOT_HALT;
    } else if (strcmp(me, "poweroff") != 0) {
        dprintf(2, "%s: invoke as poweroff, reboot, halt or shutdown\n", me);
        return 1;
    }
    for (int i = 1; i < argc && strcmp(me, "shutdown") != 0; i++) {
        if (strcmp(argv[i], "-f") != 0 && strcmp(argv[i], "--force") != 0) {
            dprintf(2, "%s: unknown option '%s'\n", me, argv[i]);
            return 1;
        }
    }

    int r = secos_reboot(cmd);                  /* returns only on failure */
    dprintf(2, "%s: failed (%d)%s\n", me, r,
            r == -1 ? " - not permitted (CAP_POWER)" : "");
    return 1;
}
