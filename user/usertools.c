/* usertools - SecOS account administration and identity commands, one signed
 * multi-call program (dispatch on argv[0], or `usertools APPLET ARGS...`):
 *
 *   useradd [-m|-M] [-r] [-u UID] [-g GROUP] [-G G1,G2] [-d HOME] [-s SHELL] [-c COMMENT] [-p HASH] NAME
 *   userdel [-r] [-f] NAME
 *   usermod [-a] [-G G1,G2] [-g GROUP] [-s SHELL] [-d HOME [-m]] [-c COMMENT] [-L|-U] NAME
 *   groupadd [-r] [-g GID] NAME
 *   groupdel NAME
 *   chpasswd                 (stdin: NAME:PASSWORD lines; root)
 *   id [-u|-g|-G] [-n] [-r] [USER]
 *   whoami
 *   groups [USER]
 *
 * The administration commands need root (the database files are root's; the
 * kernel enforces it, the check here only gives a clear message). They edit
 * /etc/passwd, /etc/group and /etc/shadow under /etc/.pwd.lock with atomic
 * rewrites (acct.c). useradd defaults: home /home/NAME (0700, owned by the
 * user, filled from /etc/skel), shell /bin/bash, a new primary group named
 * like the user, uid/gid from 1000 up, password locked until `passwd NAME`.
 *
 * This program does NOT carry MANIFEST_FLAG_SETID: a set-uid bit on it is
 * ignored by the kernel, so none of it can ever run with privileges its
 * caller does not have. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "acct.h"

static int need_root(void) {
    if (geteuid() == 0) return 0;
    acct_err("Permission denied (you must be root)");
    return 1;
}

/* getopt-style scanner: clustered flags ("-Gn"), "-x VALUE" for the
 * options in `with_arg`, "--" ends the options. */
struct opt { int i; int argc; char** argv; const char* arg; int pos; };
static int next_opt(struct opt* o, const char* flags, const char* with_arg) {
    if (o->pos == 0) {
        if (o->i >= o->argc) return -1;
        char* a = o->argv[o->i];
        if (a[0] != '-' || !a[1]) return -1;
        if (!strcmp(a, "--")) { o->i++; return -1; }
        o->pos = 1;
    }
    char* a = o->argv[o->i];
    char c = a[o->pos++];
    int last = a[o->pos] == 0;
    o->arg = 0;
    if (!strchr(flags, c)) return '?';
    if (strchr(with_arg, c)) {
        const char* rest = a + o->pos;              /* "-uVALUE" */
        o->i++; o->pos = 0;
        if (!last) { o->arg = rest; return c; }
        if (o->i >= o->argc) return '?';
        o->arg = o->argv[o->i++];
        return c;
    }
    if (last) { o->i++; o->pos = 0; }
    return c;
}

/* ---- useradd ---------------------------------------------------------------- */
static int cmd_useradd(int argc, char** argv) {
    struct acct_newuser nu = { 0 };
    nu.uid = -1;
    nu.create_home = 1;
    int sysacct = 0, explicit_m = 0;
    struct opt o = { 1, argc, argv, 0, 0 };
    int c;
    while ((c = next_opt(&o, "mMrugGdscp", "ugGdscp")) != -1) {
        switch (c) {
        case 'm': nu.create_home = 1; explicit_m = 1; break;
        case 'M': nu.create_home = 0; break;
        case 'r': sysacct = 1; break;
        case 'u': { char* e; nu.uid = (long)strtoul(o.arg, &e, 10); if (*e) goto usage; break; }
        case 'g': nu.group = o.arg; break;
        case 'G': nu.groups = o.arg; break;
        case 'd': nu.home = o.arg; break;
        case 's': nu.shell = o.arg; break;
        case 'c': nu.gecos = o.arg; break;
        case 'p': nu.hash = o.arg; break;
        default: goto usage;
        }
    }
    if (o.i != argc - 1) goto usage;
    nu.name = argv[o.i];
    nu.system = sysacct;
    if (sysacct && !explicit_m) nu.create_home = 0;   /* system accounts: no home unless -m */
    if (nu.hash && (strchr(nu.hash, ':') || strchr(nu.hash, '\n') || (nu.hash[0] != '$' && nu.hash[0] != '!'))) {
        acct_err("-p takes a crypt hash ($6$...) or '!'");
        return 1;
    }
    if (need_root()) return 1;
    unsigned uid;
    if (acct_useradd(&nu, &uid) != 0) return 1;
    acct_log("new user: name=%s, uid=%u, by uid %d", nu.name, uid, getuid());
    return 0;
usage:
    printf("usage: useradd [-m|-M] [-r] [-u UID] [-g GROUP] [-G G1,G2] [-d HOME] [-s SHELL] [-c COMMENT] [-p HASH] NAME\n");
    return 2;
}

/* ---- userdel ---------------------------------------------------------------- */
/* A process of `uid` is running? (scans /proc/<pid>/status) */
static int uid_running(unsigned uid) {
    DIR* d = opendir("/proc");
    if (!d) return 0;
    struct dirent* de;
    int found = 0;
    while (!found && (de = readdir(d)) != 0) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        char p[64], b[1024];
        snprintf(p, sizeof(p), "/proc/%s/status", de->d_name);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        int n = (int)read(fd, b, sizeof(b) - 1);
        close(fd);
        if (n <= 0) continue;
        b[n] = 0;
        char* u = strstr(b, "\nUid:");
        if (!u) continue;
        u += 5;
        for (int k = 0; k < 2; k++) {                  /* real and effective */
            while (*u == '\t' || *u == ' ') u++;
            if ((unsigned)strtoul(u, &u, 10) == uid) found = 1;
        }
    }
    closedir(d);
    return found;
}

static int remove_tree(const char* path, int depth) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if ((st.st_mode & S_IFMT) != S_IFDIR) return unlink(path);
    if (depth > 32) return -1;
    DIR* d = opendir(path);
    if (!d) return -1;
    struct dirent* de;
    int rc = 0;
    while ((de = readdir(d)) != 0) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char p[256];
        if (snprintf(p, sizeof(p), "%s/%s", path, de->d_name) >= (int)sizeof(p)) { rc = -1; continue; }
        if (remove_tree(p, depth + 1)) rc = -1;
    }
    closedir(d);
    if (rmdir(path)) rc = -1;
    return rc;
}

static int cmd_userdel(int argc, char** argv) {
    int rmhome = 0, force = 0;
    struct opt o = { 1, argc, argv, 0, 0 };
    int c;
    while ((c = next_opt(&o, "rf", "")) != -1) {
        if (c == 'r') rmhome = 1;
        else if (c == 'f') force = 1;
        else goto usage;
    }
    if (o.i != argc - 1) goto usage;
    const char* name = argv[o.i];
    if (need_root()) return 1;
    struct acct_user u;
    if (acct_user_by_name(name, &u) != 0) { acct_err("user '%s' does not exist", name); return 1; }
    if (u.uid == 0) { acct_err("refusing to delete a uid-0 account (%s)", name); return 1; }
    if (uid_running(u.uid) && !force) {
        acct_err("user %s is still running processes (use -f to delete anyway)", name);
        return 1;
    }
    if (acct_lock()) return 1;
    acct_text_t pw, gr, sh;
    int rc = 1;
    if (acct_text_load(&pw, ACCT_PASSWD) || acct_text_load(&gr, ACCT_GROUP) || acct_text_load(&sh, ACCT_SHADOW)) {
        acct_err("cannot read the account database: %s", strerror(errno));
        acct_unlock();
        return 1;
    }
    acct_text_set(&pw, name, 0);
    acct_text_set(&sh, name, 0);
    acct_group_del_member(&gr, name);
    /* The user's personal group goes too, unless another user has it as primary. */
    size_t off, len;
    if (acct_text_find(&gr, name, &off, &len)) {
        char g[16];
        int used = 0;
        if (acct_field(gr.buf + off, len, 2, g, sizeof(g)) == 0 && (unsigned)strtoul(g, 0, 10) == u.gid) {
            for (size_t p = 0; p < pw.len; ) {
                size_t e = p;
                while (e < pw.len && pw.buf[e] != '\n') e++;
                char f[16];
                if (acct_field(pw.buf + p, e - p, 3, f, sizeof(f)) == 0 && (unsigned)strtoul(f, 0, 10) == u.gid) used = 1;
                p = e + 1;
            }
            if (!used) acct_text_set(&gr, name, 0);
        }
    }
    /* passwd first: the account disappears before its pieces do. */
    if (acct_text_commit(&pw, ACCT_PASSWD, 0644, 0) == 0 && acct_text_commit(&sh, ACCT_SHADOW, 0600, 0) == 0 &&
        acct_text_commit(&gr, ACCT_GROUP, 0644, 0) == 0) rc = 0;
    acct_text_free(&pw); acct_text_free(&gr); acct_text_free(&sh);
    acct_unlock();
    if (rc) return 1;
    acct_log("delete user '%s' (uid %u) by uid %d", name, u.uid, getuid());
    if (rmhome && u.home[0]) {
        struct stat st;
        if (!strcmp(u.home, "/") || stat(u.home, &st) != 0) acct_err("warning: home %s not removed", u.home);
        else if (st.st_uid != u.uid) acct_err("warning: %s is not owned by %s; not removed", u.home, name);
        else if (remove_tree(u.home, 0) != 0) acct_err("warning: could not remove all of %s", u.home);
    }
    return 0;
usage:
    printf("usage: userdel [-r] [-f] NAME\n");
    return 2;
}

/* ---- usermod ---------------------------------------------------------------- */
static int cmd_usermod(int argc, char** argv) {
    int append = 0, move = 0, lock = 0, unlock = 0;
    const char *groups = 0, *group = 0, *shell = 0, *home = 0, *gecos = 0;
    struct opt o = { 1, argc, argv, 0, 0 };
    int c;
    while ((c = next_opt(&o, "aGgsdmcLU", "Ggsdc")) != -1) {
        switch (c) {
        case 'a': append = 1; break;
        case 'G': groups = o.arg; break;
        case 'g': group = o.arg; break;
        case 's': shell = o.arg; break;
        case 'd': home = o.arg; break;
        case 'm': move = 1; break;
        case 'c': gecos = o.arg; break;
        case 'L': lock = 1; break;
        case 'U': unlock = 1; break;
        default: goto usage;
        }
    }
    if (o.i != argc - 1 || (append && !groups) || (move && !home) || (lock && unlock)) goto usage;
    const char* name = argv[o.i];
    const char* bad[] = { shell, home, gecos };
    for (int k = 0; k < 3; k++)
        if (bad[k] && (strchr(bad[k], ':') || strchr(bad[k], '\n'))) { acct_err("values must not contain ':' or newlines"); return 1; }
    if ((shell && shell[0] != '/') || (home && home[0] != '/')) { acct_err("the shell and home must be absolute paths"); return 1; }
    if (need_root()) return 1;
    struct acct_user u;
    if (acct_user_by_name(name, &u) != 0) { acct_err("user '%s' does not exist", name); return 1; }
    unsigned ngid = u.gid;
    if (group && acct_group_resolve(group, &ngid) != 0) { acct_err("group '%s' does not exist", group); return 1; }
    if (acct_lock()) return 1;
    acct_text_t pw, gr, sh;
    int rc = 1;
    if (acct_text_load(&pw, ACCT_PASSWD) || acct_text_load(&gr, ACCT_GROUP) || acct_text_load(&sh, ACCT_SHADOW)) {
        acct_err("cannot read the account database: %s", strerror(errno));
        acct_unlock();
        return 1;
    }
    if (groups) {
        if (!append) acct_group_del_member(&gr, name);
        char list[256];
        snprintf(list, sizeof(list), "%s", groups);
        for (char* g = strtok(list, ","); g; g = strtok(0, ",")) {
            unsigned sg;
            if (acct_group_resolve(g, &sg) != 0) { acct_err("group '%s' does not exist", g); goto out; }
            if (acct_group_add_member(&gr, sg, name) != 0) { acct_err("cannot add %s to %s", name, g); goto out; }
        }
    }
    if (lock || unlock) {
        size_t off, len;
        char h[ACCT_HASH_MAX + 2];
        if (!acct_text_find(&sh, name, &off, &len) || acct_field(sh.buf + off, len, 1, h, sizeof(h) - 1) != 0) {
            acct_err("%s has no shadow entry", name);
            goto out;
        }
        char nh[ACCT_HASH_MAX + 2];
        if (lock) snprintf(nh, sizeof(nh), "%s%s", h[0] == '!' ? "" : "!", h);
        else {
            if (h[0] != '!' || h[1] != '$') {
                acct_err("unlocking %s would leave it without a usable password; set one with passwd", name);
                acct_wipe(h, sizeof(h));
                goto out;
            }
            snprintf(nh, sizeof(nh), "%s", h + 1);
        }
        /* rebuild: name:nh:<rest after field 1> */
        const char* l = sh.buf + off;
        size_t p = 0;
        for (int f = 0; f < 2 && p <= len; f++) { while (p < len && l[p] != ':') p++; p++; }
        char line[512];
        snprintf(line, sizeof(line), "%s:%s:%.*s", name, nh, p <= len ? (int)(len - p) : 0, p <= len ? l + p : "");
        int r = acct_text_set(&sh, name, line);
        acct_wipe(line, sizeof(line)); acct_wipe(h, sizeof(h)); acct_wipe(nh, sizeof(nh));
        if (r) goto out;
    }
    if (shell || home || gecos || group) {
        char line[512];
        snprintf(line, sizeof(line), "%s:x:%u:%u:%s:%s:%s", name, u.uid, ngid, gecos ? gecos : u.gecos,
                 home ? home : u.home, shell ? shell : u.shell);
        if (acct_text_set(&pw, name, line)) goto out;
    }
    if (acct_text_commit(&gr, ACCT_GROUP, 0644, 0) || acct_text_commit(&sh, ACCT_SHADOW, 0600, 0) ||
        acct_text_commit(&pw, ACCT_PASSWD, 0644, 0)) goto out;
    rc = 0;
out:
    acct_text_free(&pw); acct_text_free(&gr); acct_text_free(&sh);
    acct_unlock();
    if (rc) return 1;
    if (move && strcmp(home, u.home) != 0) {
        if (rename(u.home, home) != 0) acct_err("warning: could not move %s to %s: %s", u.home, home, strerror(errno));
    }
    acct_log("change user '%s'%s%s%s%s%s%s by uid %d", name, groups ? " groups" : "", group ? " group" : "",
             shell ? " shell" : "", home ? " home" : "", lock ? " locked" : "", unlock ? " unlocked" : "", getuid());
    return 0;
usage:
    printf("usage: usermod [-a] [-G G1,G2] [-g GROUP] [-s SHELL] [-d HOME [-m]] [-c COMMENT] [-L|-U] NAME\n");
    return 2;
}

/* ---- groupadd / groupdel ---------------------------------------------------- */
static int cmd_groupadd(int argc, char** argv) {
    int sysgrp = 0;
    long want = -1;
    struct opt o = { 1, argc, argv, 0, 0 };
    int c;
    while ((c = next_opt(&o, "rg", "g")) != -1) {
        if (c == 'r') sysgrp = 1;
        else if (c == 'g') { char* e; want = (long)strtoul(o.arg, &e, 10); if (*e) goto usage; }
        else goto usage;
    }
    if (o.i != argc - 1) goto usage;
    const char* name = argv[o.i];
    if (!acct_valid_name(name)) { acct_err("invalid group name '%s'", name); return 1; }
    if (need_root()) return 1;
    if (acct_lock()) return 1;
    acct_text_t gr;
    int rc = 1;
    if (acct_text_load(&gr, ACCT_GROUP)) { acct_err("cannot read %s", ACCT_GROUP); acct_unlock(); return 1; }
    size_t off, len;
    char nm[ACCT_NAME_MAX + 1];
    unsigned gid = 0;
    if (acct_text_find(&gr, name, &off, &len)) { acct_err("group '%s' already exists", name); goto out; }
    if (want >= 0) {
        if (acct_group_name((unsigned)want, nm, sizeof(nm)) == 0) { acct_err("gid %ld is already in use", want); goto out; }
        gid = (unsigned)want;
    } else {
        unsigned lo = sysgrp ? ACCT_SYS_UID_MIN : ACCT_UID_MIN, hi = sysgrp ? ACCT_SYS_UID_MAX : ACCT_UID_MAX;
        for (gid = lo; gid <= hi; gid++) if (acct_group_name(gid, nm, sizeof(nm)) != 0) break;
        if (gid > hi) { acct_err("no free gid left"); goto out; }
    }
    char line[96];
    snprintf(line, sizeof(line), "%s:x:%u:", name, gid);
    if (acct_text_set(&gr, name, line) == 0 && acct_text_commit(&gr, ACCT_GROUP, 0644, 0) == 0) rc = 0;
out:
    acct_text_free(&gr);
    acct_unlock();
    if (rc == 0) acct_log("new group: name=%s, gid=%u, by uid %d", name, gid, getuid());
    return rc;
usage:
    printf("usage: groupadd [-r] [-g GID] NAME\n");
    return 2;
}

static int cmd_groupdel(int argc, char** argv) {
    if (argc != 2 || argv[1][0] == '-') { printf("usage: groupdel NAME\n"); return 2; }
    const char* name = argv[1];
    if (need_root()) return 1;
    unsigned gid;
    if (acct_group_by_name(name, &gid) != 0) { acct_err("group '%s' does not exist", name); return 1; }
    if (gid == 0) { acct_err("refusing to delete group %s (gid 0)", name); return 1; }
    if (acct_lock()) return 1;
    acct_text_t pw, gr;
    int rc = 1;
    if (acct_text_load(&pw, ACCT_PASSWD) || acct_text_load(&gr, ACCT_GROUP)) { acct_unlock(); return 1; }
    for (size_t p = 0; p < pw.len; ) {
        size_t e = p;
        while (e < pw.len && pw.buf[e] != '\n') e++;
        char f[16], un[ACCT_NAME_MAX + 1];
        if (acct_field(pw.buf + p, e - p, 3, f, sizeof(f)) == 0 && (unsigned)strtoul(f, 0, 10) == gid &&
            acct_field(pw.buf + p, e - p, 0, un, sizeof(un)) == 0) {
            acct_err("cannot remove the primary group of user '%s'", un);
            goto out;
        }
        p = e + 1;
    }
    if (acct_text_set(&gr, name, 0) == 0 && acct_text_commit(&gr, ACCT_GROUP, 0644, 0) == 0) rc = 0;
out:
    acct_text_free(&pw); acct_text_free(&gr);
    acct_unlock();
    if (rc == 0) acct_log("removed group '%s' (gid %u) by uid %d", name, gid, getuid());
    return rc;
}

/* ---- chpasswd --------------------------------------------------------------- */
static int cmd_chpasswd(int argc, char** argv) {
    (void)argv;
    if (argc != 1) { printf("usage: chpasswd < NAME:PASSWORD lines\n"); return 2; }
    if (need_root()) return 1;
    char line[ACCT_PW_MAX + ACCT_NAME_MAX + 2];
    int rc = 0, ln = 0;
    for (;;) {
        int n = acct_read_line(0, line, sizeof(line));
        if (n < 0) break;
        ln++;
        if (n == 0) continue;
        char* c = strchr(line, ':');
        if (!c) { acct_err("line %d: missing ':'", ln); rc = 1; continue; }
        *c = 0;
        struct acct_user u;
        if (acct_user_by_name(line, &u) != 0) { acct_err("line %d: unknown user %s", ln, line); rc = 1; acct_wipe(line, sizeof(line)); continue; }
        char hash[ACCT_HASH_MAX];
        if (!c[1] || acct_hash_password(c + 1, hash, sizeof(hash)) != 0 || acct_set_hash(line, hash) != 0) {
            acct_err("line %d: password of %s not changed", ln, line);
            rc = 1;
        } else acct_log("password changed for %s (chpasswd) by uid %d", line, getuid());
        acct_wipe(hash, sizeof(hash));
        acct_wipe(line, sizeof(line));
    }
    return rc;
}

/* ---- id / whoami / groups --------------------------------------------------- */
static void put_id(const char* label, unsigned id, int is_group) {
    char nm[ACCT_NAME_MAX + 1];
    struct acct_user u;
    int have = is_group ? acct_group_name(id, nm, sizeof(nm)) == 0 : acct_user_by_uid(id, &u) == 0;
    if (!is_group && have) snprintf(nm, sizeof(nm), "%s", u.name);
    if (have) printf("%s%u(%s)", label, id, nm);
    else printf("%s%u", label, id);
}

static void put_name(unsigned id, int is_group) {
    char nm[ACCT_NAME_MAX + 1];
    struct acct_user u;
    if (is_group && acct_group_name(id, nm, sizeof(nm)) == 0) printf("%s", nm);
    else if (!is_group && acct_user_by_uid(id, &u) == 0) printf("%s", u.name);
    else printf("%u", id);
}

static int cmd_id(int argc, char** argv) {
    int only = 0, names = 0, real = 0;
    struct opt o = { 1, argc, argv, 0, 0 };
    int c;
    while ((c = next_opt(&o, "ugGnr", "")) != -1) {
        if (c == 'u' || c == 'g' || c == 'G') { if (only && only != c) goto usage; only = c; }
        else if (c == 'n') names = 1;
        else if (c == 'r') real = 1;
        else goto usage;
    }
    if (o.i < argc - 1 || ((names || real) && !only)) goto usage;
    unsigned uid, gid, euid, egid, grp[64];
    int ng;
    if (o.i == argc - 1) {
        struct acct_user u;
        if (acct_user_by_name(argv[o.i], &u) != 0) { acct_err("'%s': no such user", argv[o.i]); return 1; }
        uid = euid = u.uid; gid = egid = u.gid;
        ng = acct_user_groups(u.name, u.gid, grp, 64);
    } else {
        uid = (unsigned)getuid(); euid = (unsigned)geteuid(); gid = (unsigned)getgid(); egid = (unsigned)getegid();
        int g[64];
        int n = getgroups(64, g);
        if (n < 0) n = 0;
        ng = 0;
        grp[ng++] = egid;
        for (int k = 0; k < n; k++) {
            int dup = 0;
            for (int j = 0; j < ng; j++) if (grp[j] == (unsigned)g[k]) dup = 1;
            if (!dup && ng < 64) grp[ng++] = (unsigned)g[k];
        }
    }
    if (only == 'u') { unsigned v = real ? uid : euid; if (names) put_name(v, 0); else printf("%u", v); printf("\n"); return 0; }
    if (only == 'g') { unsigned v = real ? gid : egid; if (names) put_name(v, 1); else printf("%u", v); printf("\n"); return 0; }
    if (only == 'G') {
        for (int k = 0; k < ng; k++) { if (k) printf(" "); if (names) put_name(grp[k], 1); else printf("%u", grp[k]); }
        printf("\n");
        return 0;
    }
    put_id("uid=", uid, 0);
    printf(" ");
    put_id("gid=", gid, 1);
    if (euid != uid) { printf(" "); put_id("euid=", euid, 0); }
    if (egid != gid) { printf(" "); put_id("egid=", egid, 1); }
    printf(" groups=");
    for (int k = 0; k < ng; k++) { if (k) printf(","); put_id("", grp[k], 1); }
    printf("\n");
    return 0;
usage:
    printf("usage: id [-u|-g|-G] [-n] [-r] [USER]\n");
    return 2;
}

static int cmd_whoami(int argc, char** argv) {
    (void)argv;
    if (argc != 1) { printf("usage: whoami\n"); return 2; }
    put_name((unsigned)geteuid(), 0);
    printf("\n");
    return 0;
}

static int cmd_groups(int argc, char** argv) {
    if (argc > 2) { printf("usage: groups [USER]\n"); return 2; }
    char* av[] = { (char*)"id", (char*)"-Gn", argc == 2 ? argv[1] : 0, 0 };
    return cmd_id(argc == 2 ? 3 : 2, av);
}

static const struct { const char* name; int (*fn)(int, char**); } g_applets[] = {
    { "useradd", cmd_useradd }, { "userdel", cmd_userdel }, { "usermod", cmd_usermod },
    { "groupadd", cmd_groupadd }, { "groupdel", cmd_groupdel }, { "chpasswd", cmd_chpasswd },
    { "id", cmd_id }, { "whoami", cmd_whoami }, { "groups", cmd_groups },
};

int main(int argc, char** argv) {
    const char* base = argc > 0 && argv[0] ? strrchr(argv[0], '/') : 0;
    base = base ? base + 1 : (argc > 0 && argv[0] ? argv[0] : "usertools");
    if (!strcmp(base, "usertools") && argc > 1) { argc--; argv++; base = argv[0]; }
    for (unsigned i = 0; i < sizeof(g_applets) / sizeof(g_applets[0]); i++)
        if (!strcmp(base, g_applets[i].name)) { acct_prog = g_applets[i].name; return g_applets[i].fn(argc, argv); }
    printf("usertools: applets:");
    for (unsigned i = 0; i < sizeof(g_applets) / sizeof(g_applets[0]); i++) printf(" %s", g_applets[i].name);
    printf("\n");
    return 1;
}
