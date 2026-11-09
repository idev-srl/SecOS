/* sudo - run one command as root (or another user), SecOS minimal edition.
 *
 *   sudo [-u USER] [-i | -s] COMMAND [ARG]...
 *   sudo [-k] [-u USER] [-i | -s] COMMAND [ARG]...
 *   sudo -l | -v | -k | -K
 *
 * Policy, fixed (there is no sudoers file): the members of group wheel (gid
 * 10, per /etc/group) may run any command as any user after typing THEIR OWN
 * password; root needs no password; everybody else is refused without being
 * asked. Every attempt, allowed or not, is logged to /var/log/auth.log with
 * the command. Up to MAX_TRIES attempts, FAIL_DELAY seconds after each wrong
 * one.
 *
 * Credential cache: after a successful password check sudo does not ask again
 * for TS_TIMEOUT seconds IN THE SAME TERMINAL SESSION. The record is kept by
 * the kernel (prctl PR_SECOS_SUDO_TS, kernel/sudo_ts.c), keyed by the real uid,
 * the controlling terminal, the session id and the session leader's start
 * time, measured on the monotonic clock: another terminal, another user, a new
 * login on the same pty or a process without a controlling terminal always
 * gets the prompt. Every successful sudo refreshes the record; a wrong password
 * never creates one. -k drops this session's record (-k COMMAND asks for the
 * password and leaves the record alone), -K drops all of the user's records,
 * -v authenticates (cache allowed) and refreshes it.
 *
 * The command runs with the target's groups and ids (initgroups, setresgid,
 * setresuid), a fresh environment (TERM kept; HOME USER LOGNAME SHELL of the
 * target; a fixed PATH; SUDO_USER/SUDO_UID/SUDO_GID/SUDO_COMMAND) and is
 * looked up in that PATH when it has no '/'. -i runs the target's login shell
 * in its home; -s its shell (with the command, if any, as -c).
 *
 * Carries MANIFEST_FLAG_SETID and is installed root 04755.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include "acct.h"

long secos_syscall(long num, long a0, long a1, long a2, long a3, long a4);   /* user/libsecos.c */
int  getsid(int pid);

#define MAX_TRIES  3
#define FAIL_DELAY 2
#define TS_TIMEOUT (15 * 60)     /* seconds a successful authentication lasts */

/* kernel/sudo_ts.h */
#define PR_SECOS_SUDO_TS   0x53430002
#define TS_CHECK      1
#define TS_UPDATE     2
#define TS_INVALIDATE 3
#define TS_REMOVE_ALL 4
static long ts_op(long op, long arg) { return secos_syscall(90 /* SYS_PRCTL */, PR_SECOS_SUDO_TS, op, arg, 0, 0); }

static void usage(void) {
    printf("usage: sudo [-u USER] [-i | -s] COMMAND [ARG]...\n"
           "       sudo -l | -v | -k | -K\n"
           "  -k  forget this terminal's cached authentication (with COMMAND: ignore it)\n"
           "  -K  forget all of your cached authentications\n"
           "  -v  authenticate and extend the cache (15 minutes, this terminal only)\n"
           "Members of group wheel may run any command after typing their own password.\n");
    _exit(1);
}

/* COMMAND -> absolute path through the fixed PATH (after the switch). */
static int resolve(const char* cmd, const char* path, char* out, size_t cap) {
    if (strchr(cmd, '/')) { snprintf(out, cap, "%s", cmd); return access(out, X_OK) == 0 ? 0 : -1; }
    char p[160];
    snprintf(p, sizeof(p), "%s", path);
    for (char* d = strtok(p, ":"); d; d = strtok(0, ":")) {
        struct stat st;
        snprintf(out, cap, "%s/%s", d, cmd);
        if (stat(out, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG && access(out, X_OK) == 0) return 0;
    }
    return -1;
}

int main(int argc, char** argv) {
    acct_prog = "sudo";
    const char* target = "root";
    int mode_i = 0, mode_s = 0, list = 0, validate = 0, kill_ts = 0, kill_all = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        else if (!strcmp(a, "-u")) { if (++i >= argc) usage(); target = argv[i]; }
        else if (!strcmp(a, "-i")) mode_i = 1;
        else if (!strcmp(a, "-s")) mode_s = 1;
        else if (!strcmp(a, "-l")) list = 1;
        else if (!strcmp(a, "-v")) validate = 1;
        else if (!strcmp(a, "-k")) kill_ts = 1;
        else if (!strcmp(a, "-K")) kill_all = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) usage();
        else { acct_err("unknown option %s", a); usage(); }
    }
    int no_cmd = (i >= argc);
    if (kill_all && (!no_cmd || list || validate || mode_i || mode_s || kill_ts)) usage();
    if (!list && !validate && !mode_i && !mode_s && !kill_ts && !kill_all && no_cmd) usage();
    if (geteuid() != 0) { acct_err("must be set-uid root (installed 04755 with the SETID manifest)"); return 1; }
    /* -K / a bare -k: no authentication needed, nothing else is done. */
    if (kill_all || (kill_ts && no_cmd && !list && !validate && !mode_i && !mode_s)) {
        long r = ts_op(kill_all ? TS_REMOVE_ALL : TS_INVALIDATE, 0);
        if (r < 0 && r != -25) { acct_err("cannot reset the authentication cache (%ld)", r); return 1; }
        return 0;
    }

    unsigned caller = (unsigned)getuid();
    struct acct_user me, u;
    if (acct_user_by_uid(caller, &me) != 0) { acct_err("you (uid %u) are not in /etc/passwd", caller); return 1; }
    char cmdline[256] = "";
    for (int k = i; k < argc; k++) {
        size_t l = strlen(cmdline);
        snprintf(cmdline + l, sizeof(cmdline) - l, "%s%s", k > i ? " " : "", argv[k]);
    }
    const char* tty = acct_tty();

    if (caller != 0) {
        if (!acct_user_in_group(me.name, me.gid, ACCT_GID_WHEEL)) {
            acct_log("%s : NOT ALLOWED (not in wheel) ; TTY=%s ; USER=%s ; COMMAND=%s", me.name, tty, target, cmdline);
            acct_err("%s is not allowed to run sudo (not a member of group wheel). This incident has been logged.", me.name);
            return 1;
        }
        int ok = 0;
        /* A fresh record of this user in this terminal session (-k COMMAND:
         * ask anyway, and leave the record as it is). */
        if (!kill_ts && ts_op(TS_CHECK, TS_TIMEOUT) == 1) {
            ok = 1;
            acct_log("%s : cached authentication reused ; TTY=%s ; SID=%d", me.name, tty, getsid(0));
        }
        for (int t = 0; t < MAX_TRIES && !ok; t++) {
            char prompt[80], pw[ACCT_PW_MAX];
            snprintf(prompt, sizeof(prompt), "[sudo] password for %s: ", me.name);
            int n = acct_read_secret(prompt, pw, sizeof(pw));
            if (n < 0) { acct_wipe(pw, sizeof(pw)); break; }
            ok = acct_check_password(me.name, pw);
            acct_wipe(pw, sizeof(pw));
            if (!ok) {
                sleep(FAIL_DELAY);
                if (t + 1 < MAX_TRIES) printf("Sorry, try again.\n");
            }
        }
        if (!ok) {
            acct_log("%s : AUTHENTICATION FAILED ; TTY=%s ; USER=%s ; COMMAND=%s", me.name, tty, target, cmdline);
            acct_err("authentication failed");
            return 1;
        }
        if (!kill_ts) ts_op(TS_UPDATE, 0);     /* no controlling terminal: no record */
    }
    if (list) { printf("User %s may run any command as any user (member of group wheel).\n", me.name); return 0; }
    if (validate) return 0;
    if (acct_user_by_name(target, &u) != 0) { acct_err("unknown user %s", target); return 1; }
    acct_log("%s : TTY=%s ; PWD=%s ; USER=%s ; COMMAND=%s", me.name, tty, getenv("PWD") ? getenv("PWD") : "?",
             u.name, cmdline[0] ? cmdline : (mode_i ? "(login shell)" : "(shell)"));

    static char s_user[64], s_uid[32], s_gid[32], s_cmd[300];
    snprintf(s_user, sizeof(s_user), "SUDO_USER=%s", me.name);
    snprintf(s_uid, sizeof(s_uid), "SUDO_UID=%u", me.uid);
    snprintf(s_gid, sizeof(s_gid), "SUDO_GID=%u", me.gid);
    snprintf(s_cmd, sizeof(s_cmd), "SUDO_COMMAND=%s", cmdline);
    char* extra[] = { s_user, s_uid, s_gid, s_cmd, 0 };
    if (acct_become(&u) != 0) { acct_err("cannot switch to user %s", u.name); return 1; }
    char** env = acct_session_env(&u, 0, extra);

    if (mode_i || mode_s) {
        if (mode_i && chdir(u.home[0] ? u.home : "/") != 0) chdir("/");
        acct_exec_shell(&u, mode_i, cmdline[0] ? cmdline : 0, env);
    }
    char path[256];
    if (resolve(argv[i], u.uid == 0 ? ACCT_PATH_ROOT : ACCT_PATH_USER, path, sizeof(path)) != 0) {
        acct_err("%s: command not found", argv[i]);
        return 1;
    }
    fflush(stdout);
    execve(path, argv + i, env);
    acct_err("%s: %s", path, strerror(errno));
    return 1;
}
