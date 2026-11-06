/* su - run a shell as another user (default root).
 *
 *   su [-|-l|--login] [-c COMMAND] [-s SHELL] [USER]
 *
 * Asks for the TARGET user's password unless the caller's real uid is 0.
 * A failure costs FAIL_DELAY seconds, prints "Authentication failure" and is
 * logged to /var/log/auth.log; so is every successful switch. Then: groups
 * (initgroups), setresgid, setresuid, and the target's shell (or -s SHELL,
 * root only), with HOME/USER/LOGNAME/SHELL/PATH set for the target. With "-"
 * the environment is rebuilt from scratch (TERM kept), the shell is a login
 * shell and starts in the target's home; without it the rest of the
 * environment and the working directory are kept.
 *
 * Carries MANIFEST_FLAG_SETID and is installed root 04755.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "acct.h"

#define FAIL_DELAY 2

static void usage(void) {
    printf("usage: su [-|-l|--login] [-c COMMAND] [-s SHELL] [USER]\n");
    _exit(2);
}

int main(int argc, char** argv) {
    acct_prog = "su";
    int login = 0;
    const char* cmd = 0;
    const char* shell = 0;
    const char* target = "root";
    int have_user = 0;
    for (int i = 1; i < argc; i++) {           /* options may come before or after USER */
        const char* a = argv[i];
        if (!strcmp(a, "-") || !strcmp(a, "-l") || !strcmp(a, "--login")) login = 1;
        else if (!strcmp(a, "-c") || !strcmp(a, "--command")) { if (++i >= argc) usage(); cmd = argv[i]; }
        else if (!strcmp(a, "-s") || !strcmp(a, "--shell")) { if (++i >= argc) usage(); shell = argv[i]; }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) usage();
        else if (a[0] == '-') { acct_err("unknown option %s", a); usage(); }
        else if (!have_user) { target = a; have_user = 1; }
        else { acct_err("extra argument %s (use -c COMMAND)", a); usage(); }
    }
    if (geteuid() != 0) { acct_err("must be set-uid root (installed 04755 with the SETID manifest)"); return 1; }

    unsigned caller = (unsigned)getuid();
    struct acct_user me, u;
    char who[ACCT_NAME_MAX + 8];
    if (acct_user_by_uid(caller, &me) == 0) snprintf(who, sizeof(who), "%s", me.name);
    else snprintf(who, sizeof(who), "uid %u", caller);
    if (acct_user_by_name(target, &u) != 0) { acct_err("user %s does not exist", target); return 1; }
    if (shell && caller != 0) { acct_err("-s: only root may choose the shell"); return 1; }
    if (shell) snprintf(u.shell, sizeof(u.shell), "%s", shell);

    if (caller != 0) {
        char pw[ACCT_PW_MAX];
        int n = acct_read_secret("Password: ", pw, sizeof(pw));
        int ok = n >= 0 && acct_check_password(u.name, pw);
        acct_wipe(pw, sizeof(pw));
        if (!ok) {
            acct_log("FAILED su for %s by %s on %s", u.name, who, acct_tty());
            sleep(FAIL_DELAY);
            acct_err("Authentication failure");
            return 1;
        }
    }
    acct_log("(to %s) %s on %s", u.name, who, acct_tty());
    if (acct_become(&u) != 0) { acct_err("cannot switch to user %s", u.name); return 1; }
    char** env = acct_session_env(&u, !login, 0);
    if (login && chdir(u.home[0] ? u.home : "/") != 0) {
        acct_err("warning: cannot change to %s", u.home);
        chdir("/");
    }
    acct_exec_shell(&u, login, cmd, env);
}
