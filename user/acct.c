/* acct.c - shared helpers of the SecOS account tools (see acct.h).
 * SPDX-License-Identifier: MIT */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <dirent.h>
#include <grp.h>
#include <crypt.h>
#include <sys/stat.h>
#include "pwhash.h"
#include "acct.h"

const char* acct_prog = "acct";

/* chmod/chown in libsecos return -errno instead of setting errno. */
static int xchmod(const char* p, unsigned m) {
    long r = secos_syscall(32 /*SYS_CHMOD*/, (long)p, (long)m, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}
static int xchown(const char* p, unsigned u, unsigned g) {
    long r = secos_syscall(33 /*SYS_CHOWN*/, (long)p, (long)u, (long)g, 0, 1 /* no follow */);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

void acct_err(const char* fmt, ...) {
    char buf[384];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    fflush(stdout);
    dprintf(2, "%s: ", acct_prog);
    write(2, buf, (size_t)n);
    write(2, "\n", 1);
}

void acct_wipe(void* p, size_t n) {
    volatile unsigned char* v = (volatile unsigned char*)p;
    while (n--) *v++ = 0;
}

void acct_log(const char* fmt, ...) {
    char msg[320], line[448], ts[32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    time_t now = time(0);
    struct tm tm;
    if (gmtime_r(&now, &tm)) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    else snprintf(ts, sizeof(ts), "%lu", (unsigned long)now);
    int n = snprintf(line, sizeof(line), "%s %s[%d]: %s\n", ts, acct_prog, getpid(), msg);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
    if (geteuid() != 0) return;                      /* only root writes the auth log */
    mkdir("/var/log", 0755);
    int fd = open(ACCT_AUTHLOG, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    fchown(fd, 0, 0);
    fchmod(fd, 0600);
    write(fd, line, (size_t)n);
    close(fd);
}

/* ---- whole-file editing ---------------------------------------------------- */
int acct_text_load(acct_text_t* t, const char* path) {
    t->buf = 0; t->len = 0; t->cap = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            t->buf = malloc(64);
            if (!t->buf) return -1;
            t->cap = 64; t->buf[0] = 0;
            return 0;
        }
        return -1;
    }
    t->cap = 4096;
    t->buf = malloc(t->cap);
    if (!t->buf) { close(fd); errno = ENOMEM; return -1; }
    for (;;) {
        if (t->len + 1024 + 1 > t->cap) {
            size_t nc = t->cap * 2;
            char* nb = malloc(nc);
            if (!nb) { acct_text_free(t); close(fd); errno = ENOMEM; return -1; }
            memcpy(nb, t->buf, t->len);
            acct_wipe(t->buf, t->cap);
            free(t->buf);
            t->buf = nb; t->cap = nc;
        }
        long r = read(fd, t->buf + t->len, t->cap - t->len - 1);
        if (r < 0) { int e = errno; acct_text_free(t); close(fd); errno = e; return -1; }
        if (r == 0) break;
        t->len += (size_t)r;
    }
    close(fd);
    t->buf[t->len] = 0;
    return 0;
}

void acct_text_free(acct_text_t* t) {
    if (t->buf) { acct_wipe(t->buf, t->cap); free(t->buf); }
    t->buf = 0; t->len = t->cap = 0;
}

int acct_text_find(const acct_text_t* t, const char* key, size_t* off, size_t* len) {
    size_t kl = strlen(key);
    size_t p = 0;
    while (p < t->len) {
        size_t e = p;
        while (e < t->len && t->buf[e] != '\n') e++;
        if (e - p > kl && t->buf[p + kl] == ':' && memcmp(t->buf + p, key, kl) == 0) {
            *off = p; *len = e - p;
            return 1;
        }
        p = e + 1;
    }
    return 0;
}

int acct_text_set(acct_text_t* t, const char* key, const char* line) {
    size_t off, len;
    int found = acct_text_find(t, key, &off, &len);
    size_t nl = line ? strlen(line) : 0;
    size_t need = t->len + nl + 2;
    if (need + 1 > t->cap) {
        size_t nc = need + 256;
        char* nb = malloc(nc);
        if (!nb) { errno = ENOMEM; return -1; }
        memcpy(nb, t->buf, t->len + 1);
        acct_wipe(t->buf, t->cap);
        free(t->buf);
        t->buf = nb; t->cap = nc;
    }
    if (found) {
        size_t end = off + len;                       /* the '\n' (or the end) */
        if (end < t->len) end++;
        size_t tail = t->len - end;
        if (line) {
            /* new text = line + '\n' */
            memmove(t->buf + off + nl + 1, t->buf + end, tail);
            memcpy(t->buf + off, line, nl);
            t->buf[off + nl] = '\n';
            t->len = off + nl + 1 + tail;
        } else {
            memmove(t->buf + off, t->buf + end, tail);
            t->len = off + tail;
        }
    } else if (line) {
        if (t->len > 0 && t->buf[t->len - 1] != '\n') t->buf[t->len++] = '\n';
        memcpy(t->buf + t->len, line, nl);
        t->len += nl;
        t->buf[t->len++] = '\n';
    }
    t->buf[t->len] = 0;
    return 0;
}

int acct_text_commit(const acct_text_t* t, const char* path, unsigned mode, unsigned gid) {
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s+", path);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) { acct_err("cannot create %s: %s", tmp, strerror(errno)); return -1; }
    if (fchown(fd, 0, gid) != 0 || fchmod(fd, mode) != 0) {
        acct_err("cannot set the owner of %s: %s", tmp, strerror(errno));
        close(fd); unlink(tmp); return -1;
    }
    size_t done = 0;
    while (done < t->len) {
        long w = write(fd, t->buf + done, t->len - done);
        if (w <= 0) { acct_err("cannot write %s: %s", tmp, strerror(errno)); close(fd); unlink(tmp); return -1; }
        done += (size_t)w;
    }
    if (fsync(fd) != 0) { acct_err("cannot sync %s: %s", tmp, strerror(errno)); close(fd); unlink(tmp); return -1; }
    close(fd);
    if (rename(tmp, path) != 0) { acct_err("cannot replace %s: %s", path, strerror(errno)); unlink(tmp); return -1; }
    return 0;
}

int acct_field(const char* line, size_t len, int idx, char* out, size_t cap) {
    size_t p = 0;
    for (int f = 0; f < idx; f++) {
        while (p < len && line[p] != ':') p++;
        if (p >= len) return -1;
        p++;
    }
    size_t e = p;
    while (e < len && line[e] != ':') e++;
    if (e - p + 1 > cap) return -1;
    memcpy(out, line + p, e - p);
    out[e - p] = 0;
    return 0;
}

/* ---- the lock --------------------------------------------------------------- */
static int g_locked;

int acct_lock(void) {
    for (int tries = 0; tries < 50; tries++) {
        int fd = open(ACCT_LOCK, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            dprintf(fd, "%d\n", getpid());
            close(fd);
            g_locked = 1;
            return 0;
        }
        if (errno != EEXIST) { acct_err("cannot lock the account database (%s): %s", ACCT_LOCK, strerror(errno)); return -1; }
        /* Held by a process that no longer exists? Break it. */
        char b[24] = { 0 };
        int lf = open(ACCT_LOCK, O_RDONLY | O_CLOEXEC);
        if (lf >= 0) { read(lf, b, sizeof(b) - 1); close(lf); }
        int pid = atoi(b);
        if (pid > 0 && kill(pid, 0) != 0 && errno == ESRCH) { unlink(ACCT_LOCK); continue; }
        if (pid <= 0 && b[0] == 0 && tries > 10) { unlink(ACCT_LOCK); continue; }   /* empty: crashed mid-write */
        usleep(100000);
    }
    acct_err("the account database is locked (%s); try again later", ACCT_LOCK);
    return -1;
}

void acct_unlock(void) {
    if (g_locked) unlink(ACCT_LOCK);
    g_locked = 0;
}

/* ---- lookups ---------------------------------------------------------------- */
static int parse_user(const char* l, size_t len, struct acct_user* u) {
    char uid[16], gid[16];
    if (acct_field(l, len, 0, u->name, sizeof(u->name)) || acct_field(l, len, 2, uid, sizeof(uid)) ||
        acct_field(l, len, 3, gid, sizeof(gid))) return -1;
    if (acct_field(l, len, 4, u->gecos, sizeof(u->gecos))) u->gecos[0] = 0;
    if (acct_field(l, len, 5, u->home, sizeof(u->home))) u->home[0] = 0;
    if (acct_field(l, len, 6, u->shell, sizeof(u->shell))) u->shell[0] = 0;
    if (!uid[0] || !gid[0]) return -1;
    u->uid = (unsigned)strtoul(uid, 0, 10);
    u->gid = (unsigned)strtoul(gid, 0, 10);
    return 0;
}

/* Walk the lines of a loaded file. cb returns non-zero to stop. */
typedef int (*line_cb)(const char* l, size_t len, void* ctx);
static int walk(const acct_text_t* t, line_cb cb, void* ctx) {
    size_t p = 0;
    while (p < t->len) {
        size_t e = p;
        while (e < t->len && t->buf[e] != '\n') e++;
        if (e > p && t->buf[p] != '#') { int r = cb(t->buf + p, e - p, ctx); if (r) return r; }
        p = e + 1;
    }
    return 0;
}

struct ufind { const char* name; unsigned uid; int by_uid; struct acct_user* out; };
static int ufind_cb(const char* l, size_t len, void* c) {
    struct ufind* f = c;
    struct acct_user u;
    if (parse_user(l, len, &u)) return 0;
    if (f->by_uid ? u.uid == f->uid : strcmp(u.name, f->name) == 0) { *f->out = u; return 1; }
    return 0;
}
static int user_find(const char* name, unsigned uid, int by_uid, struct acct_user* u) {
    acct_text_t t;
    if (acct_text_load(&t, ACCT_PASSWD)) return -1;
    struct ufind f = { name, uid, by_uid, u };
    int r = walk(&t, ufind_cb, &f);
    acct_text_free(&t);
    return r ? 0 : -1;
}
int acct_user_by_name(const char* name, struct acct_user* u) { return name ? user_find(name, 0, 0, u) : -1; }
int acct_user_by_uid(unsigned uid, struct acct_user* u) { return user_find(0, uid, 1, u); }

struct gfind { const char* name; unsigned gid; int by_gid; char* out; size_t cap; unsigned* gout; };
static int gfind_cb(const char* l, size_t len, void* c) {
    struct gfind* f = c;
    char name[ACCT_NAME_MAX + 1], gid[16];
    if (acct_field(l, len, 0, name, sizeof(name)) || acct_field(l, len, 2, gid, sizeof(gid)) || !gid[0]) return 0;
    unsigned g = (unsigned)strtoul(gid, 0, 10);
    if (f->by_gid ? g != f->gid : strcmp(name, f->name) != 0) return 0;
    if (f->out) snprintf(f->out, f->cap, "%s", name);
    if (f->gout) *f->gout = g;
    return 1;
}
int acct_group_by_name(const char* name, unsigned* gid) {
    acct_text_t t;
    if (!name || acct_text_load(&t, ACCT_GROUP)) return -1;
    struct gfind f = { name, 0, 0, 0, 0, gid };
    int r = walk(&t, gfind_cb, &f);
    acct_text_free(&t);
    return r ? 0 : -1;
}
int acct_group_name(unsigned gid, char* out, size_t cap) {
    acct_text_t t;
    if (acct_text_load(&t, ACCT_GROUP)) return -1;
    struct gfind f = { 0, gid, 1, out, cap, 0 };
    int r = walk(&t, gfind_cb, &f);
    acct_text_free(&t);
    return r ? 0 : -1;
}
int acct_group_resolve(const char* s, unsigned* gid) {
    if (!s || !*s) return -1;
    if (acct_group_by_name(s, gid) == 0) return 0;
    char* e;
    unsigned long v = strtoul(s, &e, 10);
    if (*e || e == s) return -1;
    char nm[ACCT_NAME_MAX + 1];
    if (acct_group_name((unsigned)v, nm, sizeof(nm)) != 0) return -1;
    *gid = (unsigned)v;
    return 0;
}

/* Is `user` listed in the 4th field of the line? */
static int line_has_member(const char* l, size_t len, const char* user) {
    size_t p = 0;
    for (int f = 0; f < 3; f++) { while (p < len && l[p] != ':') p++; if (p >= len) return 0; p++; }
    size_t ul = strlen(user);
    while (p < len) {
        size_t e = p;
        while (e < len && l[e] != ',') e++;
        if (e - p == ul && memcmp(l + p, user, ul) == 0) return 1;
        p = e + 1;
    }
    return 0;
}

struct gl { const char* user; unsigned* out; int cap, n; };
static int gl_cb(const char* l, size_t len, void* c) {
    struct gl* g = c;
    char gid[16];
    if (acct_field(l, len, 2, gid, sizeof(gid)) || !gid[0]) return 0;
    if (!line_has_member(l, len, g->user)) return 0;
    unsigned v = (unsigned)strtoul(gid, 0, 10);
    for (int i = 0; i < g->n; i++) if (g->out[i] == v) return 0;
    if (g->n < g->cap) g->out[g->n++] = v;
    return 0;
}
int acct_user_groups(const char* user, unsigned pgid, unsigned* out, int cap) {
    if (cap < 1) return 0;
    out[0] = pgid;
    struct gl g = { user, out, cap, 1 };
    acct_text_t t;
    if (acct_text_load(&t, ACCT_GROUP) == 0) { walk(&t, gl_cb, &g); acct_text_free(&t); }
    return g.n;
}
int acct_user_in_group(const char* user, unsigned pgid, unsigned gid) {
    unsigned g[64];
    int n = acct_user_groups(user, pgid, g, 64);
    for (int i = 0; i < n; i++) if (g[i] == gid) return 1;
    return 0;
}

int acct_valid_name(const char* s) {
    size_t n = strlen(s);
    if (n == 0 || n > ACCT_NAME_MAX) return 0;
    if (!((s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) return 0;
    for (size_t i = 1; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

/* ---- passwords -------------------------------------------------------------- */
int acct_get_hash(const char* name, char* out, size_t cap) {
    acct_text_t t;
    if (acct_text_load(&t, ACCT_SHADOW)) return -1;
    size_t off, len;
    int r = -1;
    if (acct_text_find(&t, name, &off, &len) && acct_field(t.buf + off, len, 1, out, cap) == 0) r = 0;
    acct_text_free(&t);
    return r;
}

/* A well-formed SHA-512 crypt setting, used to spend the same time on an
 * unknown user or a locked account as on a real check. */
static const char g_dummy_hash[] =
    "$6$SecOSdummySalt0$RAx2v3cgDCwHvu6fIq8M8dAqTOq7wa0VJ0d3D0T2e5r/Dv9YgUj1sZ0Mx3Qw.6L0LhE8bKzO7nfrK3jD9oQ0x/";

int acct_check_password(const char* name, const char* plain) {
    char h[ACCT_HASH_MAX];
    int have = name && acct_get_hash(name, h, sizeof(h)) == 0;
    int ok = 0;
    if (have && h[0] == '$') ok = secos_verify_password(plain, h);
    else (void)secos_verify_password(plain, g_dummy_hash);
    acct_wipe(h, sizeof(h));
    return ok;
}

int acct_hash_password(const char* plain, char* out, size_t cap) {
    char salt[SECOS_SALT_LEN + 1];
    struct crypt_data cd;
    if (secos_make_salt(salt, sizeof(salt)) != 0) { acct_err("no random salt (/dev/urandom)"); return -1; }
    memset(&cd, 0, sizeof(cd));
    const char* h = crypt_r(plain, salt, &cd);
    int r = -1;
    if (h && h[0] == '$' && strlen(h) < cap) { strcpy(out, h); r = 0; }
    acct_wipe(&cd, sizeof(cd));
    acct_wipe(salt, sizeof(salt));
    if (r) acct_err("password hashing failed");
    return r;
}

int acct_set_hash(const char* name, const char* hash) {
    if (acct_lock()) return -1;
    acct_text_t t;
    int r = -1;
    if (acct_text_load(&t, ACCT_SHADOW) == 0) {
        char line[512], rest[256] = "";
        size_t off, len;
        long day = (long)(time(0) / 86400);
        if (acct_text_find(&t, name, &off, &len)) {
            /* keep fields 3.. (min/max/warn/inactive/expire/flag) */
            const char* l = t.buf + off;
            size_t p = 0;
            for (int f = 0; f < 3 && p <= len; f++) { while (p < len && l[p] != ':') p++; p++; }
            if (p < len && len - p < sizeof(rest)) { memcpy(rest, l + p, len - p); rest[len - p] = 0; }
            else snprintf(rest, sizeof(rest), "0:99999:7:::");
        } else snprintf(rest, sizeof(rest), "0:99999:7:::");
        snprintf(line, sizeof(line), "%s:%s:%ld:%s", name, hash, day, rest);
        if (acct_text_set(&t, name, line) == 0) r = acct_text_commit(&t, ACCT_SHADOW, 0600, 0);
        acct_wipe(line, sizeof(line));
        acct_text_free(&t);
    } else acct_err("cannot read %s: %s", ACCT_SHADOW, strerror(errno));
    acct_unlock();
    return r;
}

/* Terminal state while a password is being read, restored by any signal. */
static int g_sec_fd = -1, g_sec_have;
static struct termios g_sec_old;
static void sec_restore(void) {
    if (g_sec_have) tcsetattr(g_sec_fd, TCSAFLUSH, &g_sec_old);
    g_sec_have = 0;
}
static void sec_signal(int sig) {
    sec_restore();
    write(2, "\n", 1);
    _exit(128 + sig);
}
static const int g_sec_sigs[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTERM, SIGHUP };

static int read_line_fd(int fd, char* buf, size_t cap) {
    size_t n = 0;
    int got = 0;
    for (;;) {
        char c;
        long r = read(fd, &c, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got = 1;
        if (c == '\n' || c == '\r') { got = 2; break; }
        if (n < cap - 1) buf[n++] = c;
    }
    buf[n] = 0;
    if (!got) return -1;
    return (int)n;
}

int acct_read_secret(const char* prompt, char* buf, size_t cap) {
    fflush(stdout);
    int fd = open("/dev/tty", O_RDWR | O_CLOEXEC), own = fd >= 0;
    if (!own) fd = 0;
    for (unsigned i = 0; i < sizeof(g_sec_sigs) / sizeof(g_sec_sigs[0]); i++) signal(g_sec_sigs[i], sec_signal);
    struct termios t;
    if (tcgetattr(fd, &g_sec_old) == 0) {
        g_sec_fd = fd;
        t = g_sec_old;
        t.c_lflag &= ~(tcflag_t)(ECHO | ECHOE | ECHOK | ECHONL);
        g_sec_have = 1;
        tcsetattr(fd, TCSAFLUSH, &t);
    }
    if (prompt) write(own ? fd : 2, prompt, strlen(prompt));
    int n = read_line_fd(fd, buf, cap);
    int was_tty = g_sec_have;
    sec_restore();
    if (was_tty) write(own ? fd : 2, "\n", 1);
    for (unsigned i = 0; i < sizeof(g_sec_sigs) / sizeof(g_sec_sigs[0]); i++) signal(g_sec_sigs[i], SIG_DFL);
    if (own) close(fd);
    if (n < 0) acct_wipe(buf, cap);
    return n;
}

int acct_read_line(const char* prompt, char* buf, size_t cap) {
    fflush(stdout);
    if (prompt) write(1, prompt, strlen(prompt));
    return read_line_fd(0, buf, cap);
}

int acct_new_password(const char* prompt1, int min_len, char* hash, size_t cap) {
    char a[ACCT_PW_MAX], b[ACCT_PW_MAX];
    int r = -1;
    int n = acct_read_secret(prompt1 ? prompt1 : "New password: ", a, sizeof(a));
    if (n <= 0) { r = 1; goto out; }
    if (n < min_len) { acct_err("the password must have at least %d characters", min_len); goto out; }
    if (acct_read_secret("Retype new password: ", b, sizeof(b)) < 0 || strcmp(a, b) != 0) {
        acct_err("the passwords do not match");
        goto out;
    }
    r = acct_hash_password(a, hash, cap);
out:
    acct_wipe(a, sizeof(a));
    acct_wipe(b, sizeof(b));
    return r;
}

/* ---- sessions --------------------------------------------------------------- */
int acct_become(const struct acct_user* u) {
    if (initgroups(u->name, (int)u->gid) != 0) { acct_err("initgroups: %s", strerror(errno)); return -1; }
    if (setresgid((int)u->gid, (int)u->gid, (int)u->gid) != 0) { acct_err("setresgid: %s", strerror(errno)); return -1; }
    if (setresuid((int)u->uid, (int)u->uid, (int)u->uid) != 0) { acct_err("setresuid: %s", strerror(errno)); return -1; }
    int r, e, s;
    if (getresuid(&r, &e, &s) != 0 || (unsigned)r != u->uid || (unsigned)e != u->uid || (unsigned)s != u->uid) return -1;
    if (getresgid(&r, &e, &s) != 0 || (unsigned)r != u->gid || (unsigned)e != u->gid || (unsigned)s != u->gid) return -1;
    if (u->uid != 0 && setuid(0) == 0) return -1;    /* must be impossible now */
    return 0;
}

const char* acct_tty(void) {
    for (int fd = 0; fd < 3; fd++) {
        const char* n = isatty(fd) ? ttyname(fd) : 0;
        if (n) return n;
    }
    return "none";
}

char** acct_session_env(const struct acct_user* u, int keep, char* const* extra) {
    static char* env[64];
    static char home[160], user[48], logname[56], shell[160], path[128], term[64];
    int n = 0;
    const char* t = getenv("TERM");
    snprintf(term, sizeof(term), "TERM=%s", t && *t ? t : "linux");
    snprintf(home, sizeof(home), "HOME=%s", u->home[0] ? u->home : "/");
    snprintf(user, sizeof(user), "USER=%s", u->name);
    snprintf(logname, sizeof(logname), "LOGNAME=%s", u->name);
    snprintf(shell, sizeof(shell), "SHELL=%s", u->shell[0] ? u->shell : "/bin/bash");
    snprintf(path, sizeof(path), "PATH=%s", u->uid == 0 ? ACCT_PATH_ROOT : ACCT_PATH_USER);
    env[n++] = term; env[n++] = home; env[n++] = user; env[n++] = logname; env[n++] = shell; env[n++] = path;
    for (int i = 0; extra && extra[i] && n < 50; i++) env[n++] = extra[i];
    if (keep) {
        static const char* const skip[] = { "TERM=", "HOME=", "USER=", "LOGNAME=", "SHELL=", "PATH=", 0 };
        for (int i = 0; environ && environ[i] && n < 63; i++) {
            int s = 0;
            for (int k = 0; skip[k]; k++) if (!strncmp(environ[i], skip[k], strlen(skip[k]))) s = 1;
            if (!s) env[n++] = environ[i];
        }
    }
    env[n] = 0;
    return env;
}

void acct_exec_shell(const struct acct_user* u, int login, const char* cmd, char** envp) {
    const char* sh = u->shell[0] ? u->shell : "/bin/bash";
    const char* base = strrchr(sh, '/');
    base = base ? base + 1 : sh;
    char a0[64];
    snprintf(a0, sizeof(a0), "%s%s", login ? "-" : "", base);
    char* argv[4] = { a0, 0, 0, 0 };
    fflush(stdout);
    if (cmd) { argv[1] = (char*)"-c"; argv[2] = (char*)cmd; }
    execve(sh, argv, envp);
    acct_err("cannot run the shell %s: %s", sh, strerror(errno));
    _exit(126);
}

/* ---- account creation ------------------------------------------------------- */
static int id_used_cb(const char* l, size_t len, void* c) {
    unsigned* want = c;
    char f[16];
    if (acct_field(l, len, 2, f, sizeof(f)) || !f[0]) return 0;
    return (unsigned)strtoul(f, 0, 10) == *want;
}
static int id_used(const acct_text_t* t, unsigned id) { return walk(t, id_used_cb, &id) != 0; }

int acct_group_add_member(acct_text_t* grp, unsigned gid, const char* user) {
    size_t p = 0;
    while (p < grp->len) {
        size_t e = p;
        while (e < grp->len && grp->buf[e] != '\n') e++;
        const char* l = grp->buf + p;
        size_t len = e - p;
        char g[16], name[ACCT_NAME_MAX + 1];
        if (len && l[0] != '#' && acct_field(l, len, 2, g, sizeof(g)) == 0 && g[0] &&
            (unsigned)strtoul(g, 0, 10) == gid && acct_field(l, len, 0, name, sizeof(name)) == 0) {
            if (line_has_member(l, len, user)) return 0;
            char line[1024];
            int has = len > 0 && l[len - 1] != ':';
            if (len + strlen(user) + 2 >= sizeof(line)) return -1;
            memcpy(line, l, len);
            snprintf(line + len, sizeof(line) - len, "%s%s", has ? "," : "", user);
            return acct_text_set(grp, name, line);
        }
        p = e + 1;
    }
    errno = ENOENT;
    return -1;
}

int acct_group_del_member(acct_text_t* grp, const char* user) {
    int changes = 0;
    size_t p = 0;
    while (p < grp->len) {
        size_t e = p;
        while (e < grp->len && grp->buf[e] != '\n') e++;
        const char* l = grp->buf + p;
        size_t len = e - p;
        char name[ACCT_NAME_MAX + 1];
        if (len && l[0] != '#' && line_has_member(l, len, user) && acct_field(l, len, 0, name, sizeof(name)) == 0) {
            char line[1024];
            size_t q = 0, o = 0;
            for (int f = 0; f < 3; f++) { while (q < len && l[q] != ':') q++; q++; }
            if (q > len || q >= sizeof(line)) { p = e + 1; continue; }
            memcpy(line, l, q); o = q;
            size_t ul = strlen(user);
            int first = 1;
            while (q < len) {
                size_t m = q;
                while (m < len && l[m] != ',') m++;
                if (!(m - q == ul && memcmp(l + q, user, ul) == 0) && m > q && o + (m - q) + 2 < sizeof(line)) {
                    if (!first) line[o++] = ',';
                    memcpy(line + o, l + q, m - q); o += m - q;
                    first = 0;
                }
                q = m + 1;
            }
            line[o] = 0;
            if (acct_text_set(grp, name, line) == 0) changes++;
            p = 0;                                    /* the buffer moved: rescan */
            continue;
        }
        p = e + 1;
    }
    return changes;
}

static int copy_file(const char* from, const char* to, unsigned mode, unsigned uid, unsigned gid) {
    int in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out < 0) { close(in); return -1; }
    char buf[2048];
    long r;
    int ok = 1;
    while ((r = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, (size_t)r) != r) { ok = 0; break; }
    if (r < 0) ok = 0;
    fchown(out, uid, gid);
    fchmod(out, mode & 0777);
    close(in);
    close(out);
    return ok ? 0 : -1;
}

static int copy_tree(const char* from, const char* to, unsigned uid, unsigned gid, int depth) {
    if (depth > 8) return -1;
    DIR* d = opendir(from);
    if (!d) return -1;
    struct dirent* de;
    int rc = 0;
    while ((de = readdir(d)) != 0) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char s[256], t[256];
        if (snprintf(s, sizeof(s), "%s/%s", from, de->d_name) >= (int)sizeof(s) ||
            snprintf(t, sizeof(t), "%s/%s", to, de->d_name) >= (int)sizeof(t)) { rc = -1; continue; }
        struct stat st;
        if (lstat(s, &st) != 0) { rc = -1; continue; }
        if ((st.st_mode & S_IFMT) == S_IFDIR) {
            if (mkdir(t, 0700) != 0) { rc = -1; continue; }
            xchown(t, uid, gid);
            xchmod(t, st.st_mode & 0777);
            if (copy_tree(s, t, uid, gid, depth + 1)) rc = -1;
        } else if ((st.st_mode & S_IFMT) == S_IFLNK) {
            char l[256];
            long n = readlink(s, l, sizeof(l) - 1);
            if (n <= 0) { rc = -1; continue; }
            l[n] = 0;
            if (symlink(l, t) != 0) { rc = -1; continue; }
            xchown(t, uid, gid);
        } else if ((st.st_mode & S_IFMT) == S_IFREG) {
            if (copy_file(s, t, st.st_mode, uid, gid)) rc = -1;
        }
    }
    closedir(d);
    return rc;
}

int acct_make_home(const char* home, unsigned uid, unsigned gid) {
    struct stat st;
    if (stat(home, &st) == 0) {
        acct_err("warning: the home directory %s already exists; not copying %s into it", home, ACCT_SKEL);
        return 0;
    }
    if (mkdir(home, 0700) != 0) { acct_err("cannot create %s: %s", home, strerror(errno)); return -1; }
    if (stat(ACCT_SKEL, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR && copy_tree(ACCT_SKEL, home, uid, gid, 0))
        acct_err("warning: could not copy all of %s into %s", ACCT_SKEL, home);
    if (xchown(home, uid, gid) != 0 || xchmod(home, 0700) != 0) {
        acct_err("cannot set the owner of %s: %s", home, strerror(errno));
        return -1;
    }
    return 0;
}

int acct_useradd(const struct acct_newuser* nu, unsigned* uid_out) {
    if (!acct_valid_name(nu->name)) {
        acct_err("invalid user name '%s' (lower-case letters, digits, '_' and '-', starting with a letter or '_', at most %d)",
                 nu->name, ACCT_NAME_MAX);
        return -1;
    }
    char home[128], shell[128];
    snprintf(home, sizeof(home), "%s", nu->home ? nu->home : "");
    if (!nu->home && snprintf(home, sizeof(home), "/home/%s", nu->name) >= (int)sizeof(home)) return -1;
    snprintf(shell, sizeof(shell), "%s", nu->shell ? nu->shell : "/bin/bash");
    if (home[0] != '/' || shell[0] != '/' || strchr(home, ':') || strchr(shell, ':') || strchr(home, '\n') ||
        strchr(shell, '\n') || (nu->gecos && (strchr(nu->gecos, ':') || strchr(nu->gecos, '\n')))) {
        acct_err("the home, shell and comment must not contain ':' or newlines (and paths must be absolute)");
        return -1;
    }
    if (acct_lock()) return -1;
    acct_text_t pw = { 0 }, gr = { 0 }, sh = { 0 };
    int rc = -1;
    size_t off, len;
    if (acct_text_load(&pw, ACCT_PASSWD) || acct_text_load(&gr, ACCT_GROUP) || acct_text_load(&sh, ACCT_SHADOW)) {
        acct_err("cannot read the account database: %s", strerror(errno));
        goto out;
    }
    if (acct_text_find(&pw, nu->name, &off, &len)) { acct_err("user '%s' already exists", nu->name); goto out; }
    int new_group = nu->group == 0;
    if (new_group && acct_text_find(&gr, nu->name, &off, &len)) {
        acct_err("group '%s' already exists (use -g to make it the primary group)", nu->name);
        goto out;
    }
    unsigned lo = nu->system ? ACCT_SYS_UID_MIN : ACCT_UID_MIN, hi = nu->system ? ACCT_SYS_UID_MAX : ACCT_UID_MAX;
    unsigned uid = 0, gid = 0;
    if (nu->uid >= 0) {
        uid = (unsigned)nu->uid;
        if (id_used(&pw, uid)) { acct_err("uid %u is already in use", uid); goto out; }
    } else {
        unsigned u;
        for (u = lo; u <= hi; u++) if (!id_used(&pw, u) && (!new_group || !id_used(&gr, u))) break;
        if (u > hi) for (u = lo; u <= hi; u++) if (!id_used(&pw, u)) break;
        if (u > hi) { acct_err("no free uid left"); goto out; }
        uid = u;
    }
    if (new_group) {
        if (!id_used(&gr, uid)) gid = uid;
        else {
            unsigned g;
            for (g = lo; g <= hi; g++) if (!id_used(&gr, g)) break;
            if (g > hi) { acct_err("no free gid left"); goto out; }
            gid = g;
        }
        char line[128];
        snprintf(line, sizeof(line), "%s:x:%u:", nu->name, gid);
        if (acct_text_set(&gr, nu->name, line)) goto out;
    } else {
        if (acct_group_resolve(nu->group, &gid)) { acct_err("group '%s' does not exist", nu->group); goto out; }
    }
    if (nu->groups && *nu->groups) {
        char list[256];
        snprintf(list, sizeof(list), "%s", nu->groups);
        for (char* g = strtok(list, ","); g; g = strtok(0, ",")) {
            unsigned sg;
            /* resolve against the in-memory copy (it may hold the new group) */
            if (acct_text_find(&gr, g, &off, &len)) {
                char f[16];
                acct_field(gr.buf + off, len, 2, f, sizeof(f));
                sg = (unsigned)strtoul(f, 0, 10);
            } else if (acct_group_resolve(g, &sg)) { acct_err("group '%s' does not exist", g); goto out; }
            if (acct_group_add_member(&gr, sg, nu->name)) { acct_err("cannot add '%s' to group '%s'", nu->name, g); goto out; }
        }
    }
    char pline[512], sline[384];
    snprintf(pline, sizeof(pline), "%s:x:%u:%u:%s:%s:%s", nu->name, uid, gid, nu->gecos ? nu->gecos : "", home, shell);
    snprintf(sline, sizeof(sline), "%s:%s:%ld:0:99999:7:::", nu->name, nu->hash ? nu->hash : "!", (long)(time(0) / 86400));
    if (acct_text_set(&pw, nu->name, pline) || acct_text_set(&sh, nu->name, sline)) goto out;
    acct_wipe(sline, sizeof(sline));
    /* group and shadow first, passwd last: the account appears only once complete. */
    if (acct_text_commit(&gr, ACCT_GROUP, 0644, 0) || acct_text_commit(&sh, ACCT_SHADOW, 0600, 0) ||
        acct_text_commit(&pw, ACCT_PASSWD, 0644, 0)) goto out;
    rc = 0;
    if (uid_out) *uid_out = uid;
out:
    acct_text_free(&pw);
    acct_text_free(&gr);
    acct_text_free(&sh);
    acct_unlock();
    if (rc == 0 && nu->create_home && acct_make_home(home, uid, gid) != 0) rc = -1;
    return rc;
}
