/* SecOS - BusyBox applet stub. One tiny signed program installed under every
 * BusyBox applet name in /bin (/bin/ls, /bin/grep, ...): it runs the signed
 * multi-call binary /bin/busybox with the same argv, so BusyBox dispatches on
 * argv[0]. Used because the root FS (ramfs) has no symlinks and SYS_SPAWN does
 * not follow them, while a full copy of busybox per applet would cost ~550 KB
 * of RAM each.
 *
 * There is no in-place execve yet, so the stub spawns busybox, waits and exits
 * with its status (a death by signal is re-raised, so the caller's wait status
 * is the same as busybox's). Termination signals sent to the stub's pid (e.g.
 * `kill <pid>`, timeout(1)) are forwarded to busybox so it does not outlive
 * the stub. SPDX-License-Identifier: MIT */
#include "libsecos.h"

#define SIGHUP  1
#define SIGUSR1 10
#define SIGUSR2 12
#define SIGALRM 14
#define SIGTERM 15

static volatile int g_child;
static void forward(int sig) { if (g_child > 0) kill(g_child, sig); }

int main(int argc, char** argv) {
    (void)argc;
    static const int fwd[] = { SIGHUP, SIGTERM, SIGUSR1, SIGUSR2, SIGALRM, 0 };
    int pid = spawn("/bin/busybox", argv);
    if (pid < 0) {
        static const char msg[] = "bbstub: cannot run /bin/busybox\n";
        write(2, msg, sizeof(msg) - 1);
        return 127;
    }
    g_child = pid;
    for (int i = 0; fwd[i]; i++) signal(fwd[i], forward);
    int st;
    while ((st = waitpid(pid)) == -4 /* -EINTR: a forwarded signal */) {}
    if (st < 0) return 127;
    if (WIFSIGNALED(st)) {
        signal(WTERMSIG(st), (sighandler_t)0 /* SIG_DFL */);
        kill(getpid(), WTERMSIG(st));
        return 128 + WTERMSIG(st);
    }
    return WEXITSTATUS(st);
}
