/* SecOS - BusyBox applet stub. One tiny signed program installed under every
 * BusyBox applet name in /bin (/bin/ls, /bin/grep, ...): it re-executes the
 * signed multi-call binary /bin/busybox with the same argv, so BusyBox
 * dispatches on argv[0]. Used because the root FS (ramfs) has no symlinks and
 * SYS_SPAWN does not follow them, while a full copy of busybox per applet
 * would cost ~1 MB of RAM each. With libc's spawn+wait execve emulation the
 * stub waits for busybox and exits with its status (a signal death is
 * re-raised), so callers see busybox's exit status.
 * SPDX-License-Identifier: MIT */
#include "libsecos.h"

int main(int argc, char** argv) {
    (void)argc;
    execve("/bin/busybox", argv, environ);
    static const char msg[] = "bbstub: cannot run /bin/busybox\n";
    write(2, msg, sizeof(msg) - 1);
    return 127;
}
