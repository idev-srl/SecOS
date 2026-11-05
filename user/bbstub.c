/* SecOS - BusyBox applet stub. One tiny signed program installed under every
 * BusyBox applet name in /bin (/bin/ls, /bin/grep, ...): it replaces itself
 * (in-place SYS_EXECVE: same pid, fds, environment) with the signed multi-call
 * binary /bin/busybox and the same argv, so BusyBox dispatches on argv[0].
 * Used because the ramfs root has no symlinks, while a full copy of busybox
 * per applet would cost ~550 KB of RAM each. Because the pid is kept, signals,
 * job control and the parent's wait status reach BusyBox directly.
 * SPDX-License-Identifier: MIT */
#include "libsecos.h"

extern char** environ;

int main(int argc, char** argv) {
    (void)argc;
    execve("/bin/busybox", argv, environ);
    static const char msg[] = "bbstub: cannot exec /bin/busybox\n";
    write(2, msg, sizeof(msg) - 1);
    return 127;
}
