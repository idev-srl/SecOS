/* passwd - change a password (SHA-512 crypt in /etc/shadow).
 *
 *   passwd [USER]            change the password (own, or any as root)
 *   passwd -l|-u|-d USER     lock / unlock / delete (root only)
 *   passwd -S [USER]         status line
 *
 * A user changes only their own password (the real uid decides) and must type
 * the current one first; the new one needs MIN_LEN characters and must
 * differ from the old one. Root changes anyone's without the old password.
 * -d leaves the account with no usable password: it is locked ("!"), never
 * an empty, password-less entry. Failures cost FAIL_DELAY seconds and are
 * logged to /var/log/auth.log, as is every change.
 *
 * Carries MANIFEST_FLAG_SETID and is installed root 04755.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "acct.h"

#define MIN_LEN    6
#define FAIL_DELAY 2

static void usage(void) {
    printf("usage: passwd [USER]\n       passwd -l|-u|-d USER\n       passwd -S [USER]\n");
    _exit(2);
}

int main(int argc, char** argv) {
    acct_prog = "passwd";
    char op = 0;
    const char* target = 0;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (a[0] == '-' && a[1] && !a[2] && strchr("ludS", a[1])) { if (op) usage(); op = a[1]; }
        else if (a[0] == '-') usage();
        else if (!target) target = a;
        else usage();
    }
    if (geteuid() != 0) { acct_err("must be set-uid root (installed 04755 with the SETID manifest)"); return 1; }
    unsigned caller = (unsigned)getuid();
    struct acct_user me, u;
    if (acct_user_by_uid(caller, &me) != 0) { acct_err("you (uid %u) are not in /etc/passwd", caller); return 1; }
    if (!target) target = me.name;
    if (acct_user_by_name(target, &u) != 0) { acct_err("user '%s' does not exist", target); return 1; }
    if (caller != 0 && u.uid != caller) {
        acct_log("%s tried to change the password of %s: refused", me.name, u.name);
        acct_err("you may not view or modify password information for %s", u.name);
        return 1;
    }
    char cur[ACCT_HASH_MAX];
    if (acct_get_hash(u.name, cur, sizeof(cur)) != 0) snprintf(cur, sizeof(cur), "!");

    if (op == 'S') {
        const char* st = cur[0] == '$' ? "P" : (cur[0] == '!' || cur[0] == '*') ? "L" : "NP";
        printf("%s %s\n", u.name, st);
        acct_wipe(cur, sizeof(cur));
        return 0;
    }
    if (op) {
        if (caller != 0) { acct_err("-%c: permission denied (root only)", op); return 1; }
        char nh[ACCT_HASH_MAX + 2];
        if (op == 'l') {
            if (cur[0] == '!') { printf("passwd: %s is already locked\n", u.name); return 0; }
            snprintf(nh, sizeof(nh), "!%s", cur);
        } else if (op == 'u') {
            if (cur[0] != '!' || cur[1] != '$') {
                acct_err("unlocking %s would leave it without a usable password; set one with passwd %s", u.name, u.name);
                return 1;
            }
            snprintf(nh, sizeof(nh), "%s", cur + 1);
        } else snprintf(nh, sizeof(nh), "!");
        int r = acct_set_hash(u.name, nh);
        acct_wipe(nh, sizeof(nh));
        acct_wipe(cur, sizeof(cur));
        if (r) return 1;
        acct_log("password of %s %s by root", u.name, op == 'l' ? "locked" : op == 'u' ? "unlocked" : "deleted (locked)");
        printf("passwd: password %s for %s\n", op == 'l' ? "locked" : op == 'u' ? "unlocked" : "deleted", u.name);
        return 0;
    }

    char old[ACCT_PW_MAX] = "";
    if (caller != 0) {
        if (cur[0] != '$') {
            acct_err("the password of %s is locked; ask root to set it", u.name);
            return 1;
        }
        printf("Changing password for %s.\n", u.name);
        int n = acct_read_secret("Current password: ", old, sizeof(old));
        int ok = n >= 0 && acct_check_password(u.name, old);
        if (!ok) {
            acct_wipe(old, sizeof(old));
            acct_log("FAILED password change for %s on %s (wrong current password)", u.name, acct_tty());
            sleep(FAIL_DELAY);
            acct_err("Authentication token manipulation error (wrong password)");
            return 1;
        }
    }
    acct_wipe(cur, sizeof(cur));
    char hash[ACCT_HASH_MAX], a[ACCT_PW_MAX];
    int rc = 1;
    for (int t = 0; t < 3; t++) {
        int n = acct_read_secret("New password: ", a, sizeof(a));
        if (n <= 0) { acct_err("no password given; unchanged"); break; }
        if (caller != 0 && n < MIN_LEN) { printf("The password must have at least %d characters.\n", MIN_LEN); continue; }
        if (caller != 0 && !strcmp(a, old)) { printf("The new password must differ from the old one.\n"); continue; }
        char b[ACCT_PW_MAX];
        int m = acct_read_secret("Retype new password: ", b, sizeof(b));
        int same = m >= 0 && !strcmp(a, b);
        acct_wipe(b, sizeof(b));
        if (!same) { printf("Sorry, the passwords do not match.\n"); continue; }
        if (acct_hash_password(a, hash, sizeof(hash)) == 0 && acct_set_hash(u.name, hash) == 0) rc = 0;
        break;
    }
    acct_wipe(a, sizeof(a));
    acct_wipe(old, sizeof(old));
    acct_wipe(hash, sizeof(hash));
    if (rc == 0) {
        acct_log("password changed for %s by %s", u.name, me.name);
        printf("passwd: password updated successfully\n");
    } else acct_err("password unchanged");
    return rc;
}
