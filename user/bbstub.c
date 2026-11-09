/* SecOS - BusyBox applet stub. One tiny signed program installed under every
 * BusyBox applet name in /bin (/bin/ls, /bin/grep, ...): it replaces itself
 * (in-place SYS_EXECVE: same pid, fds, environment) with the signed multi-call
 * binary /bin/busybox and the same argv, so BusyBox dispatches on argv[0].
 * Used because the ramfs root has no symlinks, while a full copy of busybox
 * per applet would cost ~550 KB of RAM each. Because the pid is kept, signals,
 * job control and the parent's wait status reach BusyBox directly.
 * The same stub is /bin/sh on ramfs: invoked as "sh" it re-executes GNU bash
 * (which runs in POSIX mode under that name); on a disk root /bin/sh is a
 * symlink to bash.
 * SPDX-License-Identifier: MIT */
#include "libsecos.h"

extern char** environ;

int main(int argc, char** argv) {
    const char* prog = "/bin/busybox";
    if (argc > 0 && argv[0]) {
        const char* b = argv[0];
        for (const char* q = argv[0]; *q; q++) if (*q == '/') b = q + 1;
        if (b[0] == 's' && b[1] == 'h' && b[2] == 0) prog = "/bin/bash";
    }
    execve(prog, argv, environ);
    static const char msg[] = "bbstub: cannot exec its program\n";
    write(2, msg, sizeof(msg) - 1);
    return 127;
}
