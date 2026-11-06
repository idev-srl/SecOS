/* login - SecOS console login (getty + login in one program).
 *
 *   login [-f USER] [USER]
 *
 * init runs it on the console (the default /etc/inittab, docs/USERS.md):
 * prompt "<host> login:", read the password with echo off, check it against
 * /etc/shadow, then start the user's session: the terminal becomes the user's
 * (owner uid, group tty, mode 0620), supplementary groups (initgroups), then
 * setresgid and setresuid, HOME/USER/LOGNAME/SHELL/PATH, chdir HOME, and the
 * shell from /etc/passwd as a login shell (argv[0] "-bash"). The session keeps
 * init's process: login is already the session leader with the console as
 * controlling terminal, and the shell replaces it in place.
 *
 * A failed attempt costs FAIL_DELAY seconds and is logged to /var/log/auth.log;
 * after MAX_TRIES failures login exits (init starts a new one). Unknown users
 * and locked accounts get the same prompt, delay and message.
 *
 * First boot: when started by root on the console and no account has a
 * password yet (root's shadow entry is the never-set "!"), login asks for a
 * root password and offers to create a regular user (member of wheel, video
 * and input) before the first prompt. Enter skips either step.
 *
 * -f USER (root only) starts USER's session without asking (pre-authenticated).
 * The program carries MANIFEST_FLAG_SETID and is installed root 04755, so a
 * user may also run it on a terminal of theirs to log in as someone else, with
 * that user's password.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include "acct.h"

#define MAX_TRIES   3
#define FAIL_DELAY  3

static int file_has(const char* path, const char* needle) {
    char b[512];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int n = (int)read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    return strstr(b, needle) != 0;
}

/* No account can log in with a password yet: root's entry is the never-set
 * "!" and no entry holds a hash. */
static int first_boot(void) {
    acct_text_t t;
    if (acct_text_load(&t, ACCT_SHADOW)) return 0;
    char h[ACCT_HASH_MAX];
    size_t off, len;
    int fb = acct_text_find(&t, "root", &off, &len) && acct_field(t.buf + off, len, 1, h, sizeof(h)) == 0 &&
             strcmp(h, "!") == 0;
    for (size_t p = 0; fb && p < t.len; ) {
        size_t e = p;
        while (e < t.len && t.buf[e] != '\n') e++;
        if (acct_field(t.buf + p, e - p, 1, h, sizeof(h)) == 0 && h[0] == '$') fb = 0;
        p = e + 1;
    }
    acct_wipe(h, sizeof(h));
    acct_text_free(&t);
    return fb;
}

static void first_boot_setup(void) {
    char hash[ACCT_HASH_MAX];
    printf("\nWelcome to SecOS.\n"
           "No password has been set on this system yet. Choose the root password now\n"
           "(it is stored only as a SHA-512 crypt hash in /etc/shadow). Enter skips.\n\n");
    for (int i = 0; i < 3; i++) {
        int r = acct_new_password("New root password: ", 0, hash, sizeof(hash));
        if (r == 1) { printf("Skipped: root stays locked (log in with an SSH key, or set it later with passwd).\n"); break; }
        if (r == 0) {
            if (acct_set_hash("root", hash) == 0) {
                printf("Root password set.\n");
                acct_log("root password set at first boot on %s", acct_tty());
            }
            break;
        }
    }
    acct_wipe(hash, sizeof(hash));

    printf("\nCreate a regular user account now? It will be a member of wheel (sudo),\n"
           "video and input (the graphical desktop).\n");
    for (int i = 0; i < 3; i++) {
        char name[64];
        int n = acct_read_line("User name (Enter skips): ", name, sizeof(name));
        if (n <= 0) return;
        if (!acct_valid_name(name)) { printf("Invalid name: lower-case letters, digits, '_' and '-'.\n"); continue; }
        struct acct_user u;
        if (acct_user_by_name(name, &u) == 0) { printf("%s already exists.\n", name); continue; }
        int r;
        do r = acct_new_password("Password for the new user: ", 0, hash, sizeof(hash)); while (r == -1 && ++i < 3);
        if (r != 0) { printf("No password: user not created.\n"); return; }
        struct acct_newuser nu = { 0 };
        nu.name = name; nu.uid = -1; nu.groups = "wheel,video,input"; nu.hash = hash; nu.create_home = 1;
        unsigned uid;
        if (acct_useradd(&nu, &uid) == 0) {
            printf("User %s created (uid %u, home /home/%s).\n", name, uid, name);
            acct_log("user %s (uid %u) created at first boot", name, uid);
            /* GUI at boot (make GUI_AUTOSTART=1): start it as this user. */
            struct stat st;
            if (file_has("/proc/cmdline", "secos.gui_autostart=1") && stat("/etc/gui-autostart", &st) != 0) {
                int fd = open("/etc/gui-autostart", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
                if (fd >= 0) { dprintf(fd, "1\nuser=%s\n", name); close(fd); }
                printf("The desktop will start as %s at the next boot (/etc/gui-autostart).\n", name);
            }
        }
        acct_wipe(hash, sizeof(hash));
        return;
    }
}

static void motd(void) {
    char b[1024];
    int fd = open("/etc/motd", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    int n;
    while ((n = (int)read(fd, b, sizeof(b))) > 0) write(1, b, (size_t)n);
    close(fd);
}

static void start_session(const struct acct_user* u, const char* tty) {
    if (strncmp(tty, "/dev/", 5) == 0) {
        /* The terminal belongs to the user now: only they (and group tty,
         * for write(1)-style messages) can open it. */
        if (chown(tty, u->uid, ACCT_GID_TTY) < 0 || chmod(tty, 0620) < 0)
            acct_err("warning: could not give %s to %s", tty, u->name);
    }
    if (acct_become(u) != 0) {
        acct_err("cannot switch to user %s", u->name);
        acct_log("session setup FAILED for %s on %s", u->name, tty);
        sleep(FAIL_DELAY);
        _exit(1);
    }
    char** env = acct_session_env(u, 0, 0);
    const char* home = u->home[0] ? u->home : "/";
    if (chdir(home) != 0) {
        printf("No directory %s; logging in with HOME=/\n", home);
        chdir("/");
        for (int i = 0; env[i]; i++) if (!strncmp(env[i], "HOME=", 5)) env[i] = (char*)"HOME=/";
    }
    motd();
    acct_exec_shell(u, 1, 0, env);
}

int main(int argc, char** argv) {
    acct_prog = "login";
    const char* user = 0;
    int preauth = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) { preauth = 1; user = argv[++i]; }
        else if (!strcmp(argv[i], "-p")) { /* environment is always rebuilt */ }
        else if (argv[i][0] == '-') { printf("usage: login [-f USER] [USER]\n"); return 2; }
        else user = argv[i];
    }
    if (geteuid() != 0) { acct_err("must be set-uid root (installed 04755 with the SETID manifest)"); return 1; }
    int privileged = getuid() == 0;
    if (preauth && !privileged) { acct_err("-f: permission denied"); return 1; }
    if (!isatty(0)) { acct_err("not a terminal"); return 1; }
    const char* tty = acct_tty();
    char ttybuf[64];
    snprintf(ttybuf, sizeof(ttybuf), "%s", tty);
    tty = ttybuf;

    if (preauth) {
        struct acct_user u;
        if (acct_user_by_name(user, &u) != 0) { acct_err("unknown user %s", user); return 1; }
        acct_log("LOGIN ON %s BY %s (pre-authenticated)", tty, user);
        start_session(&u, tty);
    }

    if (privileged && !strcmp(tty, "/dev/console") && first_boot()) first_boot_setup();

    struct utsname un;
    const char* host = (uname(&un) == 0 && un.nodename[0]) ? un.nodename : "secos";
    char name[64], pw[ACCT_PW_MAX];
    for (int tries = 0; tries < MAX_TRIES; ) {
        if (user) { snprintf(name, sizeof(name), "%s", user); user = 0; }
        else {
            char prompt[96];
            snprintf(prompt, sizeof(prompt), "\n%s login: ", host);
            int n = acct_read_line(prompt, name, sizeof(name));
            if (n < 0) return 0;                       /* EOF / hangup */
            if (n == 0) continue;
        }
        int n = acct_read_secret("Password: ", pw, sizeof(pw));
        if (n < 0) { acct_wipe(pw, sizeof(pw)); return 0; }
        struct acct_user u;
        int known = acct_valid_name(name) && acct_user_by_name(name, &u) == 0;
        int ok = acct_check_password(known ? name : 0, pw) && known;
        acct_wipe(pw, sizeof(pw));
        if (ok) {
            acct_log("LOGIN ON %s BY %s", tty, name);
            start_session(&u, tty);
        }
        tries++;
        acct_log("FAILED LOGIN (%d) ON %s FOR %s", tries, tty, known ? name : "UNKNOWN");
        sleep(FAIL_DELAY);
        printf("Login incorrect\n");
    }
    acct_log("TOO MANY LOGIN TRIES (%d) ON %s", MAX_TRIES, tty);
    return 1;
}
