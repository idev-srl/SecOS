/* init - SecOS PID 1: boot-time service manager.
 *
 * The kernel starts /bin/init as the first user process (kernel/kernel.c). It
 * reads /etc/inittab, runs the boot steps in order, then starts and supervises
 * the services, reaps every orphan the kernel re-parents to it, and shuts the
 * system down cleanly on request.
 *
 * /etc/inittab, one entry per line ('#' starts a comment):
 *
 *     <id>:<action>:<command> [args...]
 *
 *   id       up to 15 characters; names the entry in messages and the service
 *            log /var/log/<id>.log
 *   action   sysinit   run at boot, in file order, and wait for it (killed
 *                      after SYSINIT_TIMEOUT s); output on the console
 *            respawn   run in its own session (no controlling tty, stdin
 *                      /dev/null, stdout/stderr appended to /var/log/<id>.log)
 *                      and restart it when it exits
 *            console   respawn on the console: session leader, the console
 *                      as controlling tty and its process group in the
 *                      foreground, so job control works
 *            once      like respawn, but never restarted
 *            shutdown  run on the console at shutdown, before the services
 *                      are stopped, and waited for
 *   command  absolute path and arguments, split on blanks (no shell, no
 *            quoting). PATH/HOME/TERM/SHELL are set in the environment.
 *
 * Restart backoff: a service that ran for at least BACKOFF_RESET s restarts at
 * once; one that died sooner waits 1 s, then 2, 4, ... up to BACKOFF_MAX s.
 *
 * Shutdown signals: SIGTERM = power off, SIGUSR1 = halt, SIGUSR2 = reboot
 * (/bin/poweroff, /bin/halt and /bin/reboot send them). init runs the
 * shutdown entries, sends SIGTERM + SIGHUP to every service's process group,
 * waits up to STOP_GRACE s, SIGKILLs the rest, syncs, and calls SYS_REBOOT.
 * Other job-control and terminal signals are ignored.
 *
 * Every program init starts is a signed ELF: init can only launch what the
 * signing key blessed.
 * SPDX-License-Identifier: MIT */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include "libsecos.h"

#define MAX_ENTRIES      16
#define MAX_ARGS         16
#define LINE_MAX_LEN     192
#define SYSINIT_TIMEOUT  60
#define BACKOFF_RESET    10
#define BACKOFF_MAX      30
#define STOP_GRACE        3

enum { A_SYSINIT, A_RESPAWN, A_CONSOLE, A_ONCE, A_SHUTDOWN };
static const char* const g_action_names[] = { "sysinit", "respawn", "console", "once", "shutdown" };

struct entry {
    char          id[16];
    int           action;
    char          line[LINE_MAX_LEN];
    char*         argv[MAX_ARGS + 1];
    int           pid;            /* > 0 while running */
    unsigned long started;        /* tick of the last start */
    unsigned long restart_at;     /* tick of the next start (respawn/console) */
    unsigned      backoff;        /* seconds */
    int           finished;       /* once/sysinit: ran */
    int           quiet;          /* at maximum backoff: stop repeating the message */
};

static struct entry g_tab[MAX_ENTRIES];
static int g_n;
static volatile int g_shutdown;   /* SECOS_REBOOT_* requested by a signal */

static char* const g_envp[] = {
    "PATH=/bin:/usr/bin", "HOME=/root", "TERM=linux", "SHELL=/bin/bash", "USER=root", 0
};

#define SYS_WAITANY_ 55
#define WNOHANG_     1

static unsigned long now_ms(void) { return getticks(); }   /* 1 kHz tick */

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

static void say(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    write(1, "[init] ", 7);
    write(1, buf, (size_t)n);
}

/* Reap one child: pid, 0 (WNOHANG and none ready) or -errno. */
static int reap(int* status, int nohang) {
    return (int)secos_syscall(SYS_WAITANY_, (long)status, nohang ? WNOHANG_ : 0, 0, 0, 0);
}

static void on_signal(int sig) {
    if (sig == SIGUSR1) g_shutdown = SECOS_REBOOT_HALT;
    else if (sig == SIGUSR2) g_shutdown = SECOS_REBOOT_RESTART;
    else g_shutdown = SECOS_REBOOT_POWEROFF;
}

/* [M43] Built with CONSOLE=root (secos.console=root on /proc/cmdline)? */
static int console_root(void) {
    char b[512];
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) return 0;
    int n = (int)read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    return strstr(b, "secos.console=root") != 0;
}

/* ---- /etc/inittab ---------------------------------------------------------- */
static int parse_action(const char* s) {
    for (int i = 0; i < (int)(sizeof(g_action_names) / sizeof(g_action_names[0])); i++)
        if (strcmp(s, g_action_names[i]) == 0) return i;
    return -1;
}

/* Parse one "<id>:<action>:<command>" line into e. 0 ok, -1 malformed. */
static int parse_line(char* s, struct entry* e) {
    char* c1 = strchr(s, ':');
    if (!c1) return -1;
    *c1 = 0;
    char* c2 = strchr(c1 + 1, ':');
    if (!c2) return -1;
    *c2 = 0;
    if (!s[0] || strlen(s) >= sizeof(e->id)) return -1;
    int act = parse_action(c1 + 1);
    if (act < 0) return -1;
    char* cmd = c2 + 1;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (*cmd != '/' || strlen(cmd) >= sizeof(e->line)) return -1;
    memset(e, 0, sizeof(*e));
    strcpy(e->id, s);
    e->action = act;
    strcpy(e->line, cmd);
    int ac = 0;
    char* p = e->line;
    while (*p && ac < MAX_ARGS) {
        while (*p == ' ' || *p == '\t') *p++ = 0;
        if (!*p) break;
        e->argv[ac++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    e->argv[ac] = 0;
    e->backoff = 1;
    return ac > 0 ? 0 : -1;
}

static void load_inittab(const char* path) {
    static char buf[4096];
    int fd = open(path, O_RDONLY);
    int len = 0;
    if (fd >= 0) {
        int r;
        while (len < (int)sizeof(buf) - 1 && (r = (int)read(fd, buf + len, sizeof(buf) - 1 - len)) > 0) len += r;
        close(fd);
    }
    buf[len] = 0;
    int lineno = 0;
    for (char* s = buf; s && *s; ) {
        char* nl = strchr(s, '\n');
        if (nl) *nl = 0;
        lineno++;
        char* hash = strchr(s, '#');
        if (hash) *hash = 0;
        while (*s == ' ' || *s == '\t') s++;
        int l = (int)strlen(s);
        while (l > 0 && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r')) s[--l] = 0;
        if (*s) {
            if (g_n >= MAX_ENTRIES) say("%s:%d: too many entries, ignored\n", path, lineno);
            else if (parse_line(s, &g_tab[g_n]) == 0) g_n++;
            else say("%s:%d: malformed entry, ignored\n", path, lineno);
        }
        s = nl ? nl + 1 : 0;
    }
    if (g_n == 0) {   /* no usable inittab: keep a console login (or shell) at least */
        char def_login[] = "console:console:/bin/login";
        char def_root[] = "console:console:/bin/bash -i";
        char* def = console_root() || access("/bin/login", X_OK) != 0 ? def_root : def_login;
        say("%s missing or empty; using \"%s\"\n", path, def);
        parse_line(def, &g_tab[g_n++]);
    }
}

/* ---- starting and stopping ------------------------------------------------ */
static void child_setup(const struct entry* e) {
    static const int sigs[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGHUP,
                                SIGPIPE, SIGTERM, SIGUSR1, SIGUSR2, SIGCHLD };
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) signal(sigs[i], SIG_DFL);
    unsigned long none = 0;
    sigprocmask(SIG_SETMASK, &none, 0);
    setsid();                                  /* own session and process group */
    if (e->action == A_CONSOLE) {
        /* [M43] A new console session: the console is root's again (login
         * gives it to whoever logs in; the previous user loses it here). */
        fchown(0, 0, 0);
        fchmod(0, 0600);
        ioctl(0, TIOCSCTTY, 0);                /* the console is our controlling tty */
        tcsetpgrp(0, getpid());                /* ... and we are its foreground */
    } else if (e->action == A_RESPAWN || e->action == A_ONCE) {
        char log[48];
        snprintf(log, sizeof(log), "/var/log/%s.log", e->id);
        int in = open("/dev/null", O_RDWR);
        int out = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (in >= 0) { dup2(in, 0); if (in > 2) close(in); }
        if (out >= 0) { dup2(out, 1); dup2(out, 2); if (out > 2) close(out); }
    }
    chdir(e->action == A_CONSOLE ? "/root" : "/");
}

static int start(struct entry* e) {
    int pid = fork();
    if (pid < 0) {
        say("%s: fork failed (%d)\n", e->id, pid);
        return -1;
    }
    if (pid == 0) {
        child_setup(e);
        execve(e->argv[0], e->argv, g_envp);
        dprintf(2, "[init] %s: cannot run %s (missing or unsigned)\n", e->id, e->argv[0]);
        _exit(127);
    }
    e->pid = pid;
    e->started = now_ms();
    if (e->action != A_SYSINIT && e->action != A_SHUTDOWN)
        say("started %s (%s) pid %d\n", e->id, e->argv[0], pid);
    return pid;
}

static struct entry* by_pid(int pid) {
    for (int i = 0; i < g_n; i++) if (g_tab[i].pid == pid) return &g_tab[i];
    return 0;
}

static void describe_exit(const struct entry* e, int pid, int st, unsigned long ran_s) {
    if (WIFSIGNALED(st))
        say("%s (pid %d) killed by signal %d after %lu s\n", e->id, pid, WTERMSIG(st), ran_s);
    else
        say("%s (pid %d) exited with status %d after %lu s\n", e->id, pid, WEXITSTATUS(st), ran_s);
}

/* A child was reaped: update its entry and schedule a restart if needed. */
static void child_exited(int pid, int st) {
    struct entry* e = by_pid(pid);
    if (!e) {
        say("reaped orphan pid %d\n", pid);
        return;
    }
    unsigned long ran = (now_ms() - e->started) / 1000;
    e->pid = 0;
    e->finished = 1;
    if (e->action != A_RESPAWN && e->action != A_CONSOLE) {
        if (e->action == A_ONCE) describe_exit(e, pid, st, ran);
        return;
    }
    if (g_shutdown) return;
    unsigned delay = 0;
    if (ran >= BACKOFF_RESET) {
        e->backoff = 1;
    } else {
        delay = e->backoff;
        e->backoff = e->backoff * 2 > BACKOFF_MAX ? BACKOFF_MAX : e->backoff * 2;
    }
    e->restart_at = now_ms() + (unsigned long)delay * 1000;
    if (e->action == A_CONSOLE && delay == 0) return;    /* `exit` on the console: quiet */
    if (e->quiet && delay > 0) return;                   /* already said it keeps failing */
    describe_exit(e, pid, st, ran);
    if (delay >= BACKOFF_MAX) {
        say("%s keeps failing; retrying every %u s (see /var/log/%s.log)\n", e->id, delay, e->id);
        e->quiet = 1;
    } else {
        say("restarting %s in %u s\n", e->id, delay);
        e->quiet = 0;
    }
}

/* Run e to completion (sysinit/shutdown), bounded; reaps orphans meanwhile. */
static void run_wait(struct entry* e, unsigned timeout_s) {
    if (start(e) < 0) return;
    unsigned long deadline = now_ms() + (unsigned long)timeout_s * 1000;
    while (e->pid > 0) {
        int st = 0;
        int pid = reap(&st, 1);
        if (pid > 0) {
            if (pid == e->pid) {
                e->pid = 0;
                e->finished = 1;
                if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) describe_exit(e, pid, st, (now_ms() - e->started) / 1000);
            } else {
                child_exited(pid, st);
            }
            continue;
        }
        if (now_ms() >= deadline) {
            say("%s did not finish in %u s; killing it\n", e->id, timeout_s);
            kill(-e->pid, SIGKILL);
            kill(e->pid, SIGKILL);
            deadline = now_ms() + 2000;
            timeout_s = 2;
        }
        usleep(20000);
    }
}

static int running_services(void) {
    int n = 0;
    for (int i = 0; i < g_n; i++) if (g_tab[i].pid > 0) n++;
    return n;
}

static void signal_services(int sig) {
    for (int i = 0; i < g_n; i++) {
        struct entry* e = &g_tab[i];
        if (e->pid <= 0) continue;
        kill(-e->pid, sig);                    /* its session/group (setsid) */
        kill(e->pid, sig);
        kill(e->pid, SIGCONT);
    }
}

static void reap_until(unsigned long deadline) {
    while (running_services() > 0 && now_ms() < deadline) {
        int st = 0;
        int pid = reap(&st, 1);
        if (pid > 0) child_exited(pid, st);
        else usleep(20000);
    }
}

static void do_shutdown(int cmd) {
    const char* what = cmd == SECOS_REBOOT_RESTART ? "reboot" : cmd == SECOS_REBOOT_HALT ? "halt" : "power off";
    say("shutdown requested (%s)\n", what);
    for (int i = 0; i < g_n; i++)
        if (g_tab[i].action == A_SHUTDOWN) run_wait(&g_tab[i], 10);
    say("stopping %d service(s)\n", running_services());
    signal_services(SIGTERM);
    signal_services(SIGHUP);                   /* an interactive bash ignores SIGTERM */
    reap_until(now_ms() + STOP_GRACE * 1000);
    if (running_services() > 0) {
        say("%d service(s) still running; sending SIGKILL\n", running_services());
        signal_services(SIGKILL);
        reap_until(now_ms() + 1000);
    }
    say("syncing filesystems\n");
    sync();
    int r = secos_reboot(cmd);                 /* does not return on success */
    say("SYS_REBOOT failed (%d); resuming service supervision\n", r);
    g_shutdown = 0;
    for (int i = 0; i < g_n; i++)
        if ((g_tab[i].action == A_RESPAWN || g_tab[i].action == A_CONSOLE) && g_tab[i].pid <= 0)
            g_tab[i].restart_at = now_ms();
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    signal(SIGTERM, on_signal);
    signal(SIGUSR1, on_signal);
    signal(SIGUSR2, on_signal);
    static const int ign[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGHUP, SIGPIPE };
    for (unsigned i = 0; i < sizeof(ign) / sizeof(ign[0]); i++) signal(ign[i], SIG_IGN);

    mkdir("/var/log", 0755);
    mkdir("/var/run", 0755);
    mkdir("/root/.ssh", 0700);
    mkdir("/etc/dropbear", 0700);

    const char* tab = "/etc/inittab";
    load_inittab(tab);
    say("SecOS init (pid %d): %d entr%s from %s\n", getpid(), g_n, g_n == 1 ? "y" : "ies", tab);
    if (getpid() != 1) say("warning: not running as PID 1\n");

    for (int i = 0; i < g_n && !g_shutdown; i++)
        if (g_tab[i].action == A_SYSINIT) run_wait(&g_tab[i], SYSINIT_TIMEOUT);
    for (int i = 0; i < g_n; i++)
        if (g_tab[i].action == A_RESPAWN || g_tab[i].action == A_CONSOLE || g_tab[i].action == A_ONCE)
            g_tab[i].restart_at = now_ms();

    for (;;) {
        if (g_shutdown) do_shutdown(g_shutdown);
        unsigned long now = now_ms();
        long wait_ms = -1;                     /* -1: nothing scheduled, block */
        for (int i = 0; i < g_n; i++) {
            struct entry* e = &g_tab[i];
            int wants = (e->action == A_RESPAWN || e->action == A_CONSOLE ||
                         (e->action == A_ONCE && !e->finished));
            if (!wants || e->pid > 0) continue;
            if (now >= e->restart_at) {
                if (start(e) < 0) e->restart_at = now + 1000;
            } else {
                long left = (long)(e->restart_at - now);
                if (wait_ms < 0 || left < wait_ms) wait_ms = left;
            }
        }
        int st = 0;
        int pid = reap(&st, wait_ms >= 0);
        if (pid > 0) { child_exited(pid, st); continue; }
        if (pid == -4) continue;               /* EINTR: a signal, re-check */
        if (pid == 0 || pid == -10) {          /* nothing reaped / no children */
            long ms = wait_ms >= 0 ? (wait_ms < 200 ? wait_ms : 200) : 1000;
            usleep((unsigned long)(ms > 0 ? ms : 1) * 1000);
            continue;
        }
        usleep(100000);                        /* unexpected error: do not spin */
    }
}
