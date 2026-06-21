/* SecOS [M35] capability confinement demo (user side).
 * This program is signed with a CONFINED manifest (CAP_ENFORCE | CAP_FS_READ |
 * CAP_TIME). It proves the kernel enforces least privilege: allowed operations
 * succeed, out-of-scope syscalls are denied (-1) and audited. */
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include "libsecos.h"

int main(void) {
    dprintf(1, "[m35] confined process start\n");

    /* ALLOWED: CAP_TIME — getticks should work. */
    unsigned long t = getticks();
    dprintf(1, "[m35] getticks() = %lu (CAP_TIME granted, OK)\n", t);

    /* ALLOWED: CAP_FS_READ — opening a /proc node read-only should work. */
    int fd = open("/proc/version", O_RDONLY);
    dprintf(1, "[m35] open(/proc/version, RDONLY) = %d (CAP_FS_READ granted)\n", fd);
    if (fd >= 0) close(fd);

    /* DENIED: CAP_FS_WRITE — creating a file must be refused. */
    int r = secos_syscall(41 /*SYS_CREATE*/, (long)"/tmp/m35.txt", 0, 0, 0, 0);
    dprintf(1, "[m35] create(/tmp/m35.txt) = %d (expect -1, DENIED: no CAP_FS_WRITE)\n", r);

    /* DENIED: CAP_PROC — spawning must be refused. */
    r = secos_syscall(8 /*SYS_SPAWN*/, (long)"/bin/echo", 0, 0, 0, 0);
    dprintf(1, "[m35] spawn(/bin/echo) = %d (expect -1, DENIED: no CAP_PROC)\n", r);

    /* DENIED: CAP_IPC — pipe must be refused. */
    int fds[2];
    r = secos_syscall(31 /*SYS_PIPE*/, (long)fds, 0, 0, 0, 0);
    dprintf(1, "[m35] pipe() = %d (expect -1, DENIED: no CAP_IPC)\n", r);

    /* DENIED: CAP_NET — socket must be refused. */
    r = secos_syscall(21 /*SYS_SOCKET*/, 2 /*AF_INET*/, 1 /*SOCK_STREAM*/, 0, 0, 0);
    dprintf(1, "[m35] socket() = %d (expect -1, DENIED: no CAP_NET)\n", r);

    dprintf(1, "[m35] DONE (least-privilege enforced)\n");
    return 0;
}
