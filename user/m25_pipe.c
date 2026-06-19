/* SecOS M25: anonymous pipes across fork. The parent creates a pipe, forks, and
 * sends a message down the write end; the child blocks on the read end until the
 * data arrives, prints it, then reads again and observes EOF once the parent
 * closes its write end. Exercises the blocking pipe read + EOF refcount path.
 * SPDX-License-Identifier: MIT */
#include "libsecos.h"

int main(void) {
    puts("[m25] pipe+fork test");

    int fds[2];
    if (pipe(fds) < 0) { puts("[m25] pipe FAILED"); return 1; }

    int pid = fork();
    if (pid < 0) { puts("[m25] fork FAILED"); return 1; }

    if (pid == 0) {
        /* child: reader. Close the unused write end so EOF can be observed. */
        close(fds[1]);
        char b[32];
        long n = read(fds[0], b, sizeof(b) - 1);   /* blocks until the parent writes */
        if (n > 0) { write(1, "[m25] child read: ", 18); write(1, b, n); }
        else       { puts("[m25] child read FAIL"); }
        long e = read(fds[0], b, sizeof(b));        /* parent closed write end -> EOF */
        puts(e == 0 ? "[m25] child got EOF OK" : "[m25] child EOF FAIL");
        close(fds[0]);
        _exit(0);
    }

    /* parent: writer. Close the unused read end, send the message, then close the
     * write end so the child's second read returns EOF. */
    close(fds[0]);
    const char* msg = "M25-PIPE-OK\n";
    write(fds[1], msg, 12);
    close(fds[1]);
    waitpid(pid);

    puts("[m25] DONE-USER");
    return 0;
}
