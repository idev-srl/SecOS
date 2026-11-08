/* spkg.c — SecOS package installer v2 (U6). SPDX-License-Identifier: MIT
 *
 *   spkg install <file.spkg>   verify, stage, commit (upgrade in place)
 *   spkg remove <name>         remove an installed package
 *   spkg list [<name>]         installed packages / the files of one
 *   spkg verify <file.spkg>    check the signature and the structure only
 *   spkg check <name>          re-hash the installed files against the database
 *   spkg recover               finish or roll back an interrupted operation
 *
 * Trust: a package (format v2, tools/secos-pkg) is signed like an ELF: Ed25519
 * over SHA-256 of the whole file with the 64 signature bytes zeroed. spkg
 * hashes the file as a stream (it can be many GiB) and asks the kernel to
 * check the signature (SYS_SIGVERIFY): the trusted key and the revocation list
 * stay in the kernel, the only trust root. Nothing is written until the
 * signature verified (pass 1). Pass 2 reads the file again into a private
 * staging directory and must produce the same digest (the file cannot change
 * between the check and the use).
 *
 * Atomicity: the staged files live in /var/lib/spkg/staging/<name>.<pid>/ on
 * the same filesystem as the targets. The commit point is the rename of a
 * complete transaction file to /var/lib/spkg/txn: before it, a crash leaves
 * the old version (the staging directory is deleted by the next spkg run);
 * after it, every action of the transaction is idempotent and the next spkg
 * run (or init at boot) replays it to the end: the new version. Each rename
 * is atomic on its own (one journalled ext4 transaction in SecOS).
 *
 * Paths: only under /usr, /opt and /etc (so /usr/share too). A package never
 * overwrites a file it does not own; a configuration file (/etc) that exists
 * and is not the unmodified copy of this package goes to <path>.spkg-new.
 * No setuid/setgid bits, every file owned by root. A path whose parent is a
 * symlink is refused (no write through a link).
 *
 * Owned files: /var/lib/spkg/db/<name>.list (see db_write()).
 * Docs: docs/devlog/U6-pkg2.md.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include "libsecos.h"
#include "../crypto/sha256.h"

#define SYS_SIGVERIFY 96

#define SPKG_DIR     "/var/lib/spkg"
#define SPKG_DB      SPKG_DIR "/db"
#define SPKG_STAGE   SPKG_DIR "/staging"
#define SPKG_TXN     SPKG_DIR "/txn"
#define SPKG_TXN_NEW SPKG_DIR "/txn.new"
#define SPKG_LOCK    SPKG_DIR "/lock"

#define HDR_SIZE  256
#define ENT_SIZE  320
#define MAX_ENTRIES 65536
#define PATH_MAX_ 256
#define T_FILE 0
#define T_DIR  1
#define T_LINK 2
#define F_CONF 1
#define IOBUF  (256 * 1024)

/* ---------------------------------------------------------------- helpers -- */
static int g_quiet;
static void msg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void msg(const char* fmt, ...) {
    char b[600];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    fputs("spkg: ", stderr); fputs(b, stderr); fputc('\n', stderr);
}
static void info(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void info(const char* fmt, ...) {
    if (g_quiet) return;
    char b[600];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    fputs(b, stdout); fputc('\n', stdout); fflush(stdout);
}

static uint8_t g_io[IOBUF];

static long rd_full(int fd, void* buf, long n) {
    long got = 0;
    while (got < n) {
        long r = read(fd, (char*)buf + got, (size_t)(n - got));
        if (r <= 0) return r < 0 ? -1 : got;
        got += r;
    }
    return got;
}
static int wr_full(int fd, const void* buf, long n) {
    long put = 0;
    while (put < n) {
        long r = write(fd, (const char*)buf + put, (size_t)(n - put));
        if (r <= 0) return -1;
        put += r;
    }
    return 0;
}
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t* p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

static void hex(const uint8_t* d, int n, char* out) {
    static const char x[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[2 * i] = x[d[i] >> 4]; out[2 * i + 1] = x[d[i] & 15]; }
    out[2 * n] = 0;
}
static int is_lnk(const char* p) { struct stat st; return lstat(p, &st) == 0 && S_ISLNK(st.st_mode); }
static int exists(const char* p) { struct stat st; return lstat(p, &st) == 0; }
static int is_dir(const char* p) { struct stat st; return lstat(p, &st) == 0 && S_ISDIR(st.st_mode); }

/* SHA-256 of a file (or a symlink's target string); -1 if unreadable. */
static int hash_path(const char* p, char out[65], unsigned long* size) {
    uint8_t d[32];
    sha256_ctx c; sha256_init(&c);
    unsigned long n = 0;
    if (is_lnk(p)) {
        char t[PATH_MAX_];
        long l = readlink(p, t, sizeof(t));
        if (l < 0) return -1;
        sha256_update(&c, t, (size_t)l); n = (unsigned long)l;
    } else {
        int fd = open(p, O_RDONLY);
        if (fd < 0) return -1;
        for (;;) {
            long r = read(fd, g_io, IOBUF);
            if (r < 0) { close(fd); return -1; }
            if (r == 0) break;
            sha256_update(&c, g_io, (size_t)r); n += (unsigned long)r;
        }
        close(fd);
    }
    sha256_final(&c, d);
    hex(d, 32, out);
    if (size) *size = n;
    return 0;
}

/* ------------------------------------------------------------ path rules -- */
/* Absolute, below /usr, /opt or /etc, no empty / . / .. component, no space
 * or control byte, < 256 bytes. top=1: the path is one of the three roots. */
static int path_ok(const char* p, int* top) {
    size_t n = strlen(p);
    *top = 0;
    if (n == 0 || n >= PATH_MAX_ || p[0] != '/' || p[n - 1] == '/') return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)p[i] <= 0x20 || p[i] == 0x7F) return 0;
    if (!strcmp(p, "/usr") || !strcmp(p, "/opt") || !strcmp(p, "/etc")) { *top = 1; return 1; }
    if (strncmp(p, "/usr/", 5) && strncmp(p, "/opt/", 5) && strncmp(p, "/etc/", 5)) return 0;
    const char* s = p + 1;
    while (*s) {
        const char* e = strchr(s, '/');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l == 0 || (l == 1 && s[0] == '.') || (l == 2 && s[0] == '.' && s[1] == '.')) return 0;
        s += l; if (*s == '/') s++;
    }
    return 1;
}
/* Every existing parent of p is a real directory (no symlink on the way). */
static int parents_real(const char* p) {
    char b[PATH_MAX_];
    size_t n = strlen(p);
    for (size_t i = 1; i < n; i++) {
        if (p[i] != '/') continue;
        memcpy(b, p, i); b[i] = 0;
        struct stat st;
        if (lstat(b, &st) != 0) return 1;             /* the rest does not exist yet */
        if (!S_ISDIR(st.st_mode)) return 0;
    }
    return 1;
}
static int mkdir_p(const char* p, unsigned mode) {
    char b[PATH_MAX_];
    size_t n = strlen(p);
    if (n >= sizeof(b)) return -1;
    for (size_t i = 1; i <= n; i++) {
        if (p[i] != '/' && p[i] != 0) continue;
        memcpy(b, p, i); b[i] = 0;
        if (is_dir(b)) continue;
        if (exists(b)) return -1;
        if (mkdir(b, mode) != 0 && !is_dir(b)) return -1;
        chmod(b, mode);
    }
    return 0;
}

/* Which mount holds a path (longest prefix in /proc/mounts). */
static void mount_of(const char* p, char* out, size_t outsz) {
    char line[512];
    size_t best = 0;
    snprintf(out, outsz, "/");
    FILE* f = fopen("/proc/mounts", "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char* a = strchr(line, ' '); if (!a) continue;
        char* mp = a + 1; char* e = strchr(mp, ' '); if (!e) continue;
        *e = 0;
        size_t l = strlen(mp);
        int match = (l == 1 && mp[0] == '/') || (strncmp(p, mp, l) == 0 && (p[l] == '/' || p[l] == 0));
        if (match && l > best) { best = l; snprintf(out, outsz, "%s", mp); }
    }
    fclose(f);
}

/* --------------------------------------------------------------- package -- */
typedef struct {
    char     path[PATH_MAX_];
    uint32_t mode, type, flags;
    uint64_t off, size;
    uint8_t  sha[32];
    char     target[PATH_MAX_];       /* symlink target (from the blob) */
} entry_t;

typedef struct {
    int      fd;
    uint64_t file_size, sig_off, data_off, data_size;
    uint32_t n, key_id;
    char     name[64], version[32];
    uint8_t  hdr[HDR_SIZE];
    uint8_t* table;                    /* n * ENT_SIZE raw bytes */
    entry_t* ents;
    uint8_t  digest[32];
} pkg_t;

static int name_ok(const char* s) {
    if (!*s) return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
              *s == '.' || *s == '_' || *s == '+' || *s == '-')) return 0;
    return 1;
}

/* Copy a fixed NUL-padded field; -1 if it has no NUL. */
static int field_str(const uint8_t* src, size_t n, char* dst) {
    for (size_t i = 0; i < n; i++) { dst[i] = (char)src[i]; if (!src[i]) return 0; }
    return -1;
}

/* Parse + validate header and table (already in p->hdr / p->table). */
static int pkg_parse(pkg_t* p) {
    const uint8_t* h = p->hdr;
    p->n = le32(h + 12);
    p->sig_off = le64(h + 16);
    uint64_t toff = le64(h + 24);
    p->data_off = le64(h + 32);
    p->key_id = le32(h + 40);
    p->data_size = le64(h + 144);
    if (field_str(h + 48, 64, p->name) || field_str(h + 112, 32, p->version)) { msg("bad header strings"); return -1; }
    if (!name_ok(p->name) || !p->version[0]) { msg("bad package name or version"); return -1; }
    for (int i = 152; i < 256; i++) if (h[i]) { msg("bad header (reserved bytes)"); return -1; }
    if (le32(h + 44) != 0) { msg("unknown header flags"); return -1; }
    if (toff != HDR_SIZE || p->data_off != HDR_SIZE + (uint64_t)p->n * ENT_SIZE ||
        p->data_off + p->data_size != p->sig_off || p->sig_off + 64 != p->file_size) {
        msg("bad layout (offsets do not match the file size)"); return -1;
    }
    uint64_t pos = p->data_off;
    for (uint32_t i = 0; i < p->n; i++) {
        const uint8_t* e = p->table + (size_t)i * ENT_SIZE;
        entry_t* x = &p->ents[i];
        if (field_str(e, 256, x->path)) { msg("entry %u: path not terminated", i); return -1; }
        x->mode = le32(e + 256); x->type = le32(e + 260);
        x->off = le64(e + 264); x->size = le64(e + 272);
        memcpy(x->sha, e + 280, 32);
        x->flags = le32(e + 312);
        if (le32(e + 316) || (x->flags & ~F_CONF) || x->type > T_LINK || (x->mode & ~07777u)) {
            msg("entry %u (%s): bad type/flags/mode", i, x->path); return -1;
        }
        int top;
        if (!path_ok(x->path, &top) || (top && x->type != T_DIR)) {
            msg("refused: path %s is outside /usr, /opt, /etc or malformed", x->path); return -1;
        }
        if (x->type == T_DIR) {
            if (x->size || x->off) { msg("entry %s: directory with data", x->path); return -1; }
        } else {
            if (x->size && x->off != pos) { msg("entry %s: data out of order", x->path); return -1; }
            if (x->size > p->sig_off - pos) { msg("entry %s: data past the end", x->path); return -1; }
            if (x->type == T_LINK && (x->size == 0 || x->size >= PATH_MAX_)) { msg("entry %s: bad link", x->path); return -1; }
            pos += x->size;
        }
        if ((x->flags & F_CONF) && (strncmp(x->path, "/etc/", 5) || x->type != T_FILE)) {
            msg("entry %s: config flag on a non-file or outside /etc", x->path); return -1;
        }
        /* setuid/setgid never come from a package; directories keep the sticky bit only. */
        x->mode &= (x->type == T_DIR) ? 01777u : 0777u;
    }
    if (pos != p->sig_off) { msg("bad layout (data size)"); return -1; }
    /* Duplicates, and no entry below a symlink entry of the same package. */
    for (uint32_t i = 0; i < p->n; i++) {
        size_t li = strlen(p->ents[i].path);
        for (uint32_t j = 0; j < p->n; j++) {
            if (i == j) continue;
            if (!strcmp(p->ents[i].path, p->ents[j].path)) { msg("duplicate entry %s", p->ents[i].path); return -1; }
            if (p->ents[i].type == T_LINK && !strncmp(p->ents[j].path, p->ents[i].path, li) && p->ents[j].path[li] == '/') {
                msg("entry %s is below the symlink %s", p->ents[j].path, p->ents[i].path); return -1;
            }
        }
    }
    return 0;
}

static void pkg_close(pkg_t* p) {
    if (p->fd >= 0) close(p->fd);
    free(p->table); free(p->ents);
    p->fd = -1; p->table = 0; p->ents = 0;
}

static int sigverify(const uint8_t dg[32], const uint8_t sig[64], uint32_t key_id) {
    return (int)secos_syscall(SYS_SIGVERIFY, (long)dg, (long)sig, (long)key_id, 0, 0);
}

/* Pass 1: read the header + table, stream the rest through SHA-256, have the
 * kernel check the signature. Nothing is written. */
static int pkg_open_verify(const char* file, pkg_t* p) {
    memset(p, 0, sizeof(*p)); p->fd = -1;
    p->fd = open(file, O_RDONLY);
    if (p->fd < 0) { msg("%s: cannot open (%s)", file, strerror(errno)); return -1; }
    long sz = lseek(p->fd, 0, 2);
    if (sz < HDR_SIZE + 64 || lseek(p->fd, 0, 0) != 0) { msg("%s: not a package (too short)", file); return -1; }
    p->file_size = (uint64_t)sz;
    if (rd_full(p->fd, p->hdr, HDR_SIZE) != HDR_SIZE) { msg("%s: read error", file); return -1; }
    if (memcmp(p->hdr, "SECOSPKG", 8)) { msg("%s: not a package (magic)", file); return -1; }
    uint32_t ver = le32(p->hdr + 8);
    if (ver != 2) { msg("%s: package format v%u (spkg installs v2; v1 = kernel shell 'pkg install')", file, ver); return -1; }
    uint32_t n = le32(p->hdr + 12);
    if (n == 0 || n > MAX_ENTRIES) { msg("%s: bad entry count %u", file, n); return -1; }
    if ((uint64_t)HDR_SIZE + (uint64_t)n * ENT_SIZE + 64 > p->file_size) { msg("%s: truncated table", file); return -1; }
    p->table = malloc((size_t)n * ENT_SIZE);
    p->ents = malloc((size_t)n * sizeof(entry_t));
    if (!p->table || !p->ents) { msg("out of memory (%u entries)", n); return -1; }
    sha256_ctx c; sha256_init(&c);
    sha256_update(&c, p->hdr, HDR_SIZE);
    if (rd_full(p->fd, p->table, (long)n * ENT_SIZE) != (long)n * ENT_SIZE) { msg("%s: read error", file); return -1; }
    sha256_update(&c, p->table, (size_t)n * ENT_SIZE);
    uint64_t sig_off = le64(p->hdr + 16);
    if (sig_off + 64 != p->file_size || sig_off < (uint64_t)HDR_SIZE + (uint64_t)n * ENT_SIZE) {
        msg("%s: bad layout (signature not at the end)", file); return -1;
    }
    uint64_t left = sig_off - (HDR_SIZE + (uint64_t)n * ENT_SIZE);
    while (left) {
        long want = left < IOBUF ? (long)left : IOBUF;
        long r = rd_full(p->fd, g_io, want);
        if (r != want) { msg("%s: read error", file); return -1; }
        sha256_update(&c, g_io, (size_t)r); left -= (uint64_t)r;
    }
    uint8_t sig[64], zero[64];
    memset(zero, 0, sizeof(zero));
    if (rd_full(p->fd, sig, 64) != 64) { msg("%s: read error (signature)", file); return -1; }
    sha256_update(&c, zero, 64);
    sha256_final(&c, p->digest);
    if (sigverify(p->digest, sig, le32(p->hdr + 40)) != 0) {
        msg("%s: SIGNATURE INVALID - nothing installed", file); return -1;
    }
    if (pkg_parse(p) != 0) { msg("%s: refused (signed, but malformed or not allowed)", file); return -1; }
    return 0;
}

/* --------------------------------------------------------------- database -- */
/* /var/lib/spkg/db/<name>.list, one line per object, path last:
 *   spkg-db 1 / name N / version V / digest HEX / installed UNIXTIME
 *   d <mode> <created 0|1> <path>        directory
 *   f <mode> <size> <sha256> <path>      file
 *   c <mode> <size> <sha256> <path>      configuration file (kept if modified)
 *   l <sha256-of-target> <path>          symlink                              */
typedef struct {
    char kind;                 /* d f c l */
    unsigned mode, created;
    unsigned long size;
    char sha[65];
    char* path;
} dbent_t;
typedef struct {
    char name[64], version[32], digest[65];
    int n, cap;
    dbent_t* e;
} db_t;

static void db_free(db_t* d) {
    for (int i = 0; i < d->n; i++) free(d->e[i].path);
    free(d->e); memset(d, 0, sizeof(*d));
}
static dbent_t* db_add(db_t* d) {
    if (d->n == d->cap) {
        int nc = d->cap ? d->cap * 2 : 64;
        dbent_t* ne = realloc(d->e, (size_t)nc * sizeof(dbent_t));
        if (!ne) return 0;
        d->e = ne; d->cap = nc;
    }
    dbent_t* x = &d->e[d->n++];
    memset(x, 0, sizeof(*x));
    return x;
}
static char* dupstr(const char* s) { size_t n = strlen(s) + 1; char* r = malloc(n); if (r) memcpy(r, s, n); return r; }

/* Read a whole small text file (DB / txn) into a NUL-terminated buffer. */
static char* slurp(const char* path, long max) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    long sz = lseek(fd, 0, 2);
    if (sz < 0 || sz > max || lseek(fd, 0, 0) != 0) { close(fd); return 0; }
    char* b = malloc((size_t)sz + 1);
    if (!b) { close(fd); return 0; }
    if (rd_full(fd, b, sz) != sz) { close(fd); free(b); return 0; }
    close(fd);
    b[sz] = 0;
    return b;
}
static char* next_tok(char** s) {
    while (**s == ' ') (*s)++;
    char* t = *s;
    while (**s && **s != ' ') (*s)++;
    if (**s) *(*s)++ = 0;
    return t;
}
static int db_load(const char* name, db_t* d) {
    char path[PATH_MAX_];
    memset(d, 0, sizeof(*d));
    snprintf(path, sizeof(path), SPKG_DB "/%s.list", name);
    char* b = slurp(path, 64L << 20);
    if (!b) return -1;
    char* s = b; int ok = 0;
    while (*s) {
        char* line = s; char* nl = strchr(s, '\n');
        if (nl) { *nl = 0; s = nl + 1; } else s += strlen(s);
        char* t = line; char* k = next_tok(&t);
        if (!strcmp(k, "spkg-db")) { ok = 1; continue; }
        if (!strcmp(k, "name")) { snprintf(d->name, sizeof(d->name), "%s", t); continue; }
        if (!strcmp(k, "version")) { snprintf(d->version, sizeof(d->version), "%s", t); continue; }
        if (!strcmp(k, "digest")) { snprintf(d->digest, sizeof(d->digest), "%s", t); continue; }
        if (strlen(k) != 1 || !strchr("dfcl", k[0])) continue;
        dbent_t* x = db_add(d); if (!x) break;
        x->kind = k[0];
        if (x->kind == 'd') { x->mode = (unsigned)strtoul(next_tok(&t), 0, 8); x->created = (unsigned)strtoul(next_tok(&t), 0, 10); }
        else if (x->kind == 'l') { snprintf(x->sha, sizeof(x->sha), "%s", next_tok(&t)); }
        else {
            x->mode = (unsigned)strtoul(next_tok(&t), 0, 8);
            x->size = strtoul(next_tok(&t), 0, 10);
            snprintf(x->sha, sizeof(x->sha), "%s", next_tok(&t));
        }
        x->path = dupstr(t);
        if (!x->path) { d->n--; break; }
    }
    free(b);
    if (!ok || !d->name[0]) { db_free(d); return -1; }
    return 0;
}
static int db_write(const char* file, const db_t* d) {
    int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    FILE* f = fdopen(fd, "w");
    if (!f) { close(fd); return -1; }
    fprintf(f, "spkg-db 1\nname %s\nversion %s\ndigest %s\ninstalled %lu\n", d->name, d->version, d->digest,
            (unsigned long)time(0));
    for (int i = 0; i < d->n; i++) {
        const dbent_t* x = &d->e[i];
        if (x->kind == 'd') fprintf(f, "d %o %u %s\n", x->mode, x->created, x->path);
        else if (x->kind == 'l') fprintf(f, "l %s %s\n", x->sha, x->path);
        else fprintf(f, "%c %o %lu %s %s\n", x->kind, x->mode, x->size, x->sha, x->path);
    }
    if (fflush(f) != 0) { fclose(f); return -1; }
    fsync(fd);
    return fclose(f) == 0 ? 0 : -1;
}

/* Names of the installed packages. */
static int db_names(char names[][64], int max) {
    DIR* dp = opendir(SPKG_DB);
    if (!dp) return 0;
    int n = 0; struct dirent* de;
    while ((de = readdir(dp)) && n < max) {
        size_t l = strlen(de->d_name);
        if (l < 6 || l - 5 >= 64 || strcmp(de->d_name + l - 5, ".list")) continue;
        memcpy(names[n], de->d_name, l - 5); names[n][l - 5] = 0;
        if (name_ok(names[n])) n++;
    }
    closedir(dp);
    return n;
}

/* Path -> owning package, over every database (a small open hash table). */
typedef struct { char* path; int pkg; dbent_t* ent; } own_t;
static own_t* g_own; static unsigned g_own_cap;
static char (*g_pk_names)[64]; static int g_pk_n;
static db_t* g_pk_db;
static unsigned fnv(const char* s) { unsigned h = 2166136261u; while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; } return h; }
static own_t* own_find(const char* p) {
    if (!g_own_cap) return 0;
    for (unsigned i = fnv(p) & (g_own_cap - 1);; i = (i + 1) & (g_own_cap - 1)) {
        if (!g_own[i].path) return 0;
        if (!strcmp(g_own[i].path, p)) return &g_own[i];
    }
}
static int own_build(void) {
    static char names[256][64];
    g_pk_names = names;
    g_pk_n = db_names(names, 256);
    g_pk_db = calloc((size_t)(g_pk_n ? g_pk_n : 1), sizeof(db_t));
    if (!g_pk_db) return -1;
    unsigned total = 0;
    for (int i = 0; i < g_pk_n; i++) { if (db_load(names[i], &g_pk_db[i]) == 0) total += (unsigned)g_pk_db[i].n; }
    g_own_cap = 64; while (g_own_cap < total * 2 + 2) g_own_cap <<= 1;
    g_own = calloc(g_own_cap, sizeof(own_t));
    if (!g_own) return -1;
    for (int i = 0; i < g_pk_n; i++)
        for (int k = 0; k < g_pk_db[i].n; k++) {
            dbent_t* x = &g_pk_db[i].e[k];
            if (x->kind == 'd' || own_find(x->path)) continue;
            unsigned j = fnv(x->path) & (g_own_cap - 1);
            while (g_own[j].path) j = (j + 1) & (g_own_cap - 1);
            g_own[j].path = x->path; g_own[j].pkg = i; g_own[j].ent = x;
        }
    return 0;
}
static int pk_index(const char* name) {
    for (int i = 0; i < g_pk_n; i++) if (!strcmp(g_pk_names[i], name) && g_pk_db[i].name[0]) return i;
    return -1;
}

/* ------------------------------------------------------------ transaction -- */
/* /var/lib/spkg/txn (text, written to txn.new, fsync'd, renamed: the commit):
 *   spkg-txn 1 / op install|remove / name N / staging DIR
 *   D <mode> <path>       mkdir if missing, chmod
 *   F <index> <path>      rename DIR/f<index> over <path> (skip if gone)
 *   R <path>              unlink if present
 *   C <sha256> <path>     unlink if present and unmodified (a config file)
 *   E <path>              rmdir if empty (created directories, deepest first)
 *   B <name>              rename DIR/db over db/<name>.list
 *   X <name>              unlink db/<name>.list
 *   end
 * Every action is idempotent, so replaying a half-applied txn finishes it. */
typedef struct { FILE* f; int fd; } txnw_t;

static long g_kill_at = -1;     /* test hook SPKG_TEST_KILL_AT=<n>: SIGKILL self after n actions */
static long g_actions;
static void action_done(void) {
    g_actions++;
    if (g_kill_at >= 0 && g_actions >= g_kill_at) { fflush(stdout); kill(getpid(), 9); }
}

static int txn_apply(char* txt, int verbose) {
    char staging[PATH_MAX_] = "";
    char op[16] = "";
    int ended = 0;
    for (char* s = txt; *s;) {                        /* the commit is valid only when complete */
        char* nl = strchr(s, '\n'); if (!nl) break;
        if (!strncmp(s, "end\n", 4) && nl[1] == 0) ended = 1;
        s = nl + 1;
    }
    if (strncmp(txt, "spkg-txn 1\n", 11) || !ended) return -2;
    int errs = 0, nact = 0;
    char* s = txt;
    while (*s) {
        char* line = s; char* nl = strchr(s, '\n');
        if (nl) { *nl = 0; s = nl + 1; } else s += strlen(s);
        char* t = line; char* k = next_tok(&t);
        if (!strcmp(k, "op")) { snprintf(op, sizeof(op), "%s", t); continue; }
        if (!strcmp(k, "staging")) { snprintf(staging, sizeof(staging), "%s", t); continue; }
        if (strlen(k) != 1) continue;
        char path[PATH_MAX_ + 32], src[PATH_MAX_ + 32];
        int r = 0;
        switch (k[0]) {
        case 'D': {
            unsigned mode = (unsigned)strtoul(next_tok(&t), 0, 8);
            if (!parents_real(t)) { msg("refusing %s: a parent is not a directory", t); errs++; break; }
            if (!is_dir(t)) { if (mkdir(t, mode) != 0 && !is_dir(t)) { msg("mkdir %s failed", t); errs++; break; } }
            chmod(t, mode); chown(t, 0, 0);
            break;
        }
        case 'F': {
            char* idx = next_tok(&t);
            snprintf(src, sizeof(src), "%s/f%s", staging, idx);
            if (!exists(src)) break;                   /* already moved */
            if (!parents_real(t)) { msg("refusing %s: a parent is not a directory", t); errs++; break; }
            if (is_dir(t) && !is_lnk(t)) { msg("cannot replace directory %s", t); errs++; break; }
            if (rename(src, t) != 0) { msg("rename -> %s failed (%s)", t, strerror(errno)); errs++; }
            break;
        }
        case 'R':
            if (exists(t) && !is_dir(t)) r = unlink(t);
            if (r) { msg("unlink %s failed", t); errs++; }
            break;
        case 'C': {
            char* want = next_tok(&t);
            char have[65];
            if (!exists(t)) break;
            if (hash_path(t, have, 0) == 0 && !strcmp(have, want)) { if (unlink(t)) { msg("unlink %s failed", t); errs++; } }
            else if (verbose) info("kept modified configuration file %s", t);
            break;
        }
        case 'E':
            if (is_dir(t) && !is_lnk(t)) rmdir(t);   /* fails while not empty: fine */
            break;
        case 'B':
            snprintf(src, sizeof(src), "%s/db", staging);
            snprintf(path, sizeof(path), SPKG_DB "/%s.list", t);
            if (exists(src) && rename(src, path) != 0) { msg("database update failed"); errs++; }
            break;
        case 'X':
            snprintf(path, sizeof(path), SPKG_DB "/%s.list", t);
            if (exists(path) && unlink(path) != 0) { msg("database update failed"); errs++; }
            break;
        default:
            continue;
        }
        nact++;
        action_done();
    }
    (void)op; (void)nact;
    return errs ? -1 : 0;
}

/* Delete one staging directory (flat: f<i> files/links + db). */
static void rm_stage(const char* dir) {
    DIR* dp = opendir(dir);
    if (dp) {
        struct dirent* de; char p[PATH_MAX_ + 64];
        while ((de = readdir(dp))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
            unlink(p);
        }
        closedir(dp);
    }
    rmdir(dir);
}
static void rm_all_staging(void) {
    DIR* dp = opendir(SPKG_STAGE);
    if (!dp) return;
    static char names[512][256]; int n = 0;
    struct dirent* de;
    while ((de = readdir(dp)) && n < 512) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(names[n++], 256, "%s", de->d_name);
    }
    closedir(dp);
    for (int i = 0; i < n; i++) {
        char p[PATH_MAX_ + 64];
        snprintf(p, sizeof(p), SPKG_STAGE "/%s", names[i]);
        rm_stage(p);
    }
}

/* Finish a committed transaction, or discard an uncommitted one. Called with
 * the lock held by every command that changes state (and by `spkg recover`,
 * which init runs at boot when a txn is pending). */
static int recover(int verbose) {
    int rc = 0;
    char* t = slurp(SPKG_TXN, 64L << 20);
    if (t) {
        char staging[PATH_MAX_] = "";
        char* sp = strstr(t, "\nstaging ");
        if (sp) { sp += 9; size_t l = 0; while (sp[l] && sp[l] != '\n' && l < PATH_MAX_ - 1) { staging[l] = sp[l]; l++; } staging[l] = 0; }
        info("spkg: finishing an interrupted operation (journal %s)", SPKG_TXN);
        int r = txn_apply(t, verbose);
        free(t);
        if (r == -2) {
            msg("transaction journal is incomplete or damaged: moved to %s.bad, not applied", SPKG_TXN);
            rename(SPKG_TXN, SPKG_TXN ".bad"); rc = -1;
        } else if (r != 0) {
            msg("some actions of the interrupted operation failed; journal kept, rerun 'spkg recover'");
            return -1;
        } else {
            if (staging[0]) rm_stage(staging);
            unlink(SPKG_TXN);
            info("spkg: interrupted operation completed");
        }
    }
    unlink(SPKG_TXN_NEW);
    rm_all_staging();                                  /* uncommitted work: the old version stays */
    return rc;
}

/* ------------------------------------------------------------------ lock -- */
static int g_locked;
static void unlock(void) { if (g_locked) { unlink(SPKG_LOCK); g_locked = 0; } }
static int lock(void) {
    if (mkdir_p(SPKG_DB, 0755) || mkdir_p(SPKG_STAGE, 0700)) { msg("cannot create " SPKG_DIR); return -1; }
    chmod(SPKG_STAGE, 0700);
    for (int tries = 0; tries < 3; tries++) {
        int fd = open(SPKG_LOCK, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            char b[32]; int l = snprintf(b, sizeof(b), "%d\n", getpid());
            wr_full(fd, b, l); close(fd);
            g_locked = 1;
            return 0;
        }
        char* o = slurp(SPKG_LOCK, 64);
        int pid = o ? atoi(o) : 0;
        free(o);
        if (pid > 0 && pid != getpid() && kill(pid, 0) == 0) { msg("another spkg (pid %d) is running", pid); return -1; }
        unlink(SPKG_LOCK);                             /* stale: its owner is gone */
    }
    msg("cannot take " SPKG_LOCK);
    return -1;
}

static int need_root(void) {
    if (geteuid() != 0) { msg("must be run as root (sudo spkg ...)"); return -1; }
    return 0;
}

/* --------------------------------------------------------------- install -- */
typedef struct { const char* path; unsigned mode; int created; } dirplan_t;

static int txn_line(FILE* f, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static int txn_line(FILE* f, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap);
    return r < 0 ? -1 : 0;
}
static int path_depth(const char* p) { int n = 0; for (; *p; p++) if (*p == '/') n++; return n; }
static int cmp_depth_desc(const void* a, const void* b) {
    return path_depth(*(char* const*)b) - path_depth(*(char* const*)a);
}

static int cmd_install(const char* file) {
    pkg_t P; db_t nd; memset(&nd, 0, sizeof(nd));
    char stage[PATH_MAX_], sp[PATH_MAX_ + 32];
    int rc = 1;
    if (need_root() || lock()) return 1;
    if (recover(1) != 0) goto out_unlock;
    if (pkg_open_verify(file, &P) != 0) { pkg_close(&P); goto out_unlock; }
    char dg[65]; hex(P.digest, 32, dg);
    info("spkg: %s %s: signature OK (%u entries, %llu bytes of data)", P.name, P.version, P.n,
         (unsigned long long)P.data_size);

    /* Room for the new files (the old ones go only after the commit). */
    struct statfs sf;
    if (statfs(SPKG_STAGE, &sf) == 0 && sf.f_bsize > 0) {
        unsigned long long avail = (unsigned long long)sf.f_bavail * (unsigned long long)sf.f_bsize;
        unsigned long long need = P.data_size + (unsigned long long)P.n * 4096ULL + (8ULL << 20);
        if (avail < need) { msg("not enough space: %llu MiB needed, %llu MiB free", need >> 20, avail >> 20); goto out_close; }
    }
    char m1[PATH_MAX_], m2[PATH_MAX_];
    mount_of(SPKG_STAGE, m1, sizeof(m1));

    if (own_build() != 0) { msg("out of memory"); goto out_close; }
    int old = pk_index(P.name);
    db_t* od = old >= 0 ? &g_pk_db[old] : 0;
    if (od) info("spkg: upgrading %s %s -> %s", P.name, od->version, P.version);

    /* ---- plan ---- */
    snprintf(nd.name, sizeof(nd.name), "%s", P.name);
    snprintf(nd.version, sizeof(nd.version), "%s", P.version);
    snprintf(nd.digest, sizeof(nd.digest), "%s", dg);
    char** final = calloc(P.n, sizeof(char*));         /* where each entry lands */
    char** mkd = calloc((size_t)P.n * 2 + 8, sizeof(char*));
    unsigned* mkm = calloc((size_t)P.n * 2 + 8, sizeof(unsigned));
    int* mkw = calloc((size_t)P.n * 2 + 8, sizeof(int));   /* existed before */
    int nmkd = 0;
    if (!final || !mkd || !mkm || !mkw) { msg("out of memory"); goto out_close; }
    for (uint32_t i = 0; i < P.n; i++) {
        entry_t* e = &P.ents[i];
        int top; path_ok(e->path, &top);
        if (top) continue;
        mount_of(e->path, m2, sizeof(m2));
        if (strcmp(m1, m2)) { msg("refused: %s is on %s, not on %s with " SPKG_DIR, e->path, m2, m1); goto out_close; }
        if (!parents_real(e->path)) { msg("refused: a parent of %s is not a directory (symlink?)", e->path); goto out_close; }
        /* missing parents (and the directory entries themselves) */
        char b[PATH_MAX_];
        size_t n = strlen(e->path);
        for (size_t k = 5; k <= n; k++) {
            if (e->path[k] != '/' && e->path[k] != 0) continue;
            if (k == n && e->type != T_DIR) break;
            memcpy(b, e->path, k); b[k] = 0;
            int dup = 0;
            for (int q = 0; q < nmkd; q++) if (!strcmp(mkd[q], b)) { dup = 1; if (k == n) mkm[q] = e->mode; break; }
            if (dup) continue;
            if (exists(b) && !is_dir(b)) { msg("conflict: %s exists and is not a directory", b); goto out_close; }
            mkd[nmkd] = dupstr(b); mkm[nmkd] = (k == n) ? e->mode : 0755; mkw[nmkd] = is_dir(b); nmkd++;
        }
        if (e->type == T_DIR) continue;
        own_t* o = own_find(e->path);
        if (exists(e->path) && is_dir(e->path)) { msg("conflict: %s is a directory", e->path); goto out_close; }
        if (o && o->pkg != old) { msg("conflict: %s belongs to package %s", e->path, g_pk_names[o->pkg]); goto out_close; }
        const char* dst = e->path;
        char alt[PATH_MAX_ + 16];
        if (e->flags & F_CONF) {
            char have[65], want[65]; hex(e->sha, 32, want);
            int there = exists(e->path);
            int ours_unmodified = o && o->ent->kind == 'c' && there && hash_path(e->path, have, 0) == 0 &&
                                  !strcmp(have, o->ent->sha);
            if (there && !ours_unmodified) {
                if (hash_path(e->path, have, 0) == 0 && !strcmp(have, want)) {
                    /* already this content: keep the file (still ours if it was) */
                    if (o && o->pkg == old) {
                        dbent_t* x = db_add(&nd); x->kind = 'c'; x->mode = e->mode; x->size = (unsigned long)e->size;
                        snprintf(x->sha, sizeof(x->sha), "%s", want); x->path = dupstr(e->path);
                    }
                    final[i] = 0;
                    continue;
                }
                snprintf(alt, sizeof(alt), "%s.spkg-new", e->path);
                if (strlen(alt) >= PATH_MAX_) { msg("path too long: %s", alt); goto out_close; }
                own_t* oa = own_find(alt);
                if (oa && oa->pkg != old) { msg("conflict: %s belongs to package %s", alt, g_pk_names[oa->pkg]); goto out_close; }
                info("spkg: %s exists (not the packaged copy): new version saved as %s", e->path, alt);
                dst = alt;
                if (o && o->pkg == old) {                 /* keep tracking the user's modified copy */
                    dbent_t* x = db_add(&nd); *x = *o->ent; x->path = dupstr(e->path);
                }
            }
        } else if (!o && exists(e->path)) {
            msg("conflict: %s exists and belongs to no package (remove it first)", e->path); goto out_close;
        }
        final[i] = dupstr(dst);
        dbent_t* x = db_add(&nd);
        if (!x || !final[i]) { msg("out of memory"); goto out_close; }
        x->kind = e->type == T_LINK ? 'l' : ((e->flags & F_CONF) && dst == e->path ? 'c' : 'f');
        x->mode = e->mode; x->size = (unsigned long)e->size;
        hex(e->sha, 32, x->sha);
        x->path = dupstr(dst);
    }
    for (int q = 0; q < nmkd; q++) {
        dbent_t* x = db_add(&nd);
        x->kind = 'd'; x->mode = mkm[q];
        int was = mkw[q];
        int old_created = 0;
        if (od) for (int k = 0; k < od->n; k++) if (od->e[k].kind == 'd' && !strcmp(od->e[k].path, mkd[q])) old_created = (int)od->e[k].created;
        x->created = (!was || old_created) ? 1 : 0;
        x->path = dupstr(mkd[q]);
    }

    /* ---- stage (pass 2: same bytes as verified, or nothing happens) ---- */
    snprintf(stage, sizeof(stage), SPKG_STAGE "/%s.%d", P.name, getpid());
    if (mkdir(stage, 0700) != 0 && !is_dir(stage)) { msg("cannot create %s", stage); goto out_close; }
    {
        sha256_ctx c; sha256_init(&c);
        uint8_t h2[HDR_SIZE];
        if (lseek(P.fd, 0, 0) != 0 || rd_full(P.fd, h2, HDR_SIZE) != HDR_SIZE || memcmp(h2, P.hdr, HDR_SIZE)) goto changed;
        sha256_update(&c, h2, HDR_SIZE);
        for (uint32_t i = 0; i < P.n; i++) {
            uint8_t eb[ENT_SIZE];
            if (rd_full(P.fd, eb, ENT_SIZE) != ENT_SIZE || memcmp(eb, P.table + (size_t)i * ENT_SIZE, ENT_SIZE)) goto changed;
            sha256_update(&c, eb, ENT_SIZE);
        }
        for (uint32_t i = 0; i < P.n; i++) {
            entry_t* e = &P.ents[i];
            if (e->type == T_DIR) continue;
            snprintf(sp, sizeof(sp), "%s/f%u", stage, i);
            if (e->type == T_LINK) {
                if (rd_full(P.fd, g_io, (long)e->size) != (long)e->size) goto changed;
                sha256_update(&c, g_io, (size_t)e->size);
                memcpy(e->target, g_io, (size_t)e->size); e->target[e->size] = 0;
                if (strlen(e->target) != e->size) { msg("%s: link target has a NUL byte", e->path); goto out_stage; }
                if (final[i] && symlink(e->target, sp) != 0) { msg("staging %s failed", e->path); goto out_stage; }
                continue;
            }
            int fd = -1;
            if (final[i]) {
                fd = open(sp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (fd < 0) { msg("staging %s failed (%s)", e->path, strerror(errno)); goto out_stage; }
            }
            uint64_t left = e->size;
            while (left) {
                long want = left < IOBUF ? (long)left : IOBUF;
                if (rd_full(P.fd, g_io, want) != want) { if (fd >= 0) close(fd); goto changed; }
                sha256_update(&c, g_io, (size_t)want);
                if (fd >= 0 && wr_full(fd, g_io, want) != 0) {
                    msg("writing %s failed (disk full?)", e->path); close(fd); goto out_stage;
                }
                left -= (uint64_t)want;
            }
            if (fd >= 0) {
                fsync(fd); close(fd);
                chmod(sp, e->mode); chown(sp, 0, 0);
            }
        }
        uint8_t sig[64], zero[64], d2[32];
        memset(zero, 0, sizeof(zero));
        if (rd_full(P.fd, sig, 64) != 64) goto changed;
        sha256_update(&c, zero, 64);
        sha256_final(&c, d2);
        if (memcmp(d2, P.digest, 32)) goto changed;
    }
    snprintf(sp, sizeof(sp), "%s/db", stage);
    if (db_write(sp, &nd) != 0) { msg("cannot write the package database"); goto out_stage; }

    /* ---- the transaction ---- */
    {
        int fd = open(SPKG_TXN_NEW, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        FILE* f = fd >= 0 ? fdopen(fd, "w") : 0;
        if (!f) { msg("cannot write " SPKG_TXN_NEW); if (fd >= 0) close(fd); goto out_stage; }
        int e = txn_line(f, "spkg-txn 1\nop install\nname %s\nstaging %s\n", P.name, stage);
        /* directories, parents first */
        for (int depth = 1; depth < 64; depth++)
            for (int q = 0; q < nmkd; q++)          /* only new ones: never chmod a shared directory */
                if (!mkw[q] && path_depth(mkd[q]) == depth) e |= txn_line(f, "D %o %s\n", mkm[q], mkd[q]);
        for (uint32_t i = 0; i < P.n; i++) if (P.ents[i].type != T_DIR && final[i]) e |= txn_line(f, "F %u %s\n", i, final[i]);
        if (od) {                                      /* the old version's leftovers */
            char** gone = calloc((size_t)od->n + 1, sizeof(char*)); int ng = 0;
            for (int k = 0; k < od->n; k++) {
                dbent_t* x = &od->e[k];
                int keep = 0;
                for (int q = 0; q < nd.n && !keep; q++) if (!strcmp(nd.e[q].path, x->path)) keep = 1;
                if (keep) continue;
                if (x->kind == 'd') { if (x->created && gone) gone[ng++] = x->path; }
                else if (x->kind == 'c') e |= txn_line(f, "C %s %s\n", x->sha, x->path);
                else e |= txn_line(f, "R %s\n", x->path);
            }
            if (gone) { qsort(gone, (size_t)ng, sizeof(char*), cmp_depth_desc); for (int q = 0; q < ng; q++) e |= txn_line(f, "E %s\n", gone[q]); free(gone); }
        }
        e |= txn_line(f, "B %s\nend\n", P.name);
        if (e || fflush(f) != 0) { fclose(f); msg("cannot write " SPKG_TXN_NEW); goto out_stage; }
        fsync(fd);
        fclose(f);
    }
    if (rename(SPKG_TXN_NEW, SPKG_TXN) != 0) { msg("cannot commit (%s)", strerror(errno)); goto out_stage; }
    /* ---- committed: from here the new version wins, even after a crash ---- */
    {
        char* t = slurp(SPKG_TXN, 64L << 20);
        int r = t ? txn_apply(t, 1) : -1;
        free(t);
        if (r != 0) { msg("commit incomplete; run 'spkg recover'"); pkg_close(&P); goto out_unlock; }
        rm_stage(stage);
        unlink(SPKG_TXN);
    }
    info("spkg: installed %s %s (%d objects)", P.name, P.version, nd.n);
    rc = 0;
    goto out_close;
changed:
    msg("%s: the package file changed or could not be read while installing - nothing installed", file);
out_stage:
    rm_stage(stage);
out_close:
    pkg_close(&P);
out_unlock:
    unlock();
    return rc;
}

/* ---------------------------------------------------------------- remove -- */
static int cmd_remove(const char* name) {
    int rc = 1;
    if (need_root() || lock()) return 1;
    if (recover(1) != 0) goto out;
    if (!name_ok(name)) { msg("bad package name"); goto out; }
    db_t d;
    if (db_load(name, &d) != 0) { msg("%s is not installed", name); goto out; }
    int fd = open(SPKG_TXN_NEW, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    FILE* f = fd >= 0 ? fdopen(fd, "w") : 0;
    if (!f) { msg("cannot write " SPKG_TXN_NEW); if (fd >= 0) close(fd); db_free(&d); goto out; }
    int e = txn_line(f, "spkg-txn 1\nop remove\nname %s\n", name);
    char** dirs = calloc((size_t)d.n + 1, sizeof(char*)); int nd = 0;
    for (int k = 0; k < d.n; k++) {
        dbent_t* x = &d.e[k];
        if (x->kind == 'd') { if (x->created && dirs) dirs[nd++] = x->path; }
        else if (x->kind == 'c') e |= txn_line(f, "C %s %s\n", x->sha, x->path);
        else e |= txn_line(f, "R %s\n", x->path);
    }
    if (dirs) { qsort(dirs, (size_t)nd, sizeof(char*), cmp_depth_desc); for (int q = 0; q < nd; q++) e |= txn_line(f, "E %s\n", dirs[q]); free(dirs); }
    e |= txn_line(f, "X %s\nend\n", name);
    if (e || fflush(f) != 0) { fclose(f); db_free(&d); msg("cannot write " SPKG_TXN_NEW); goto out; }
    fsync(fd); fclose(f);
    if (rename(SPKG_TXN_NEW, SPKG_TXN) != 0) { msg("cannot commit"); db_free(&d); goto out; }
    char* t = slurp(SPKG_TXN, 64L << 20);
    int r = t ? txn_apply(t, 1) : -1;
    free(t);
    if (r != 0) { msg("removal incomplete; run 'spkg recover'"); db_free(&d); goto out; }
    unlink(SPKG_TXN);
    info("spkg: removed %s %s", d.name, d.version);
    db_free(&d);
    rc = 0;
out:
    unlock();
    return rc;
}

/* ------------------------------------------------------------ list/check -- */
static int cmd_list(const char* name) {
    if (name) {
        db_t d;
        if (!name_ok(name) || db_load(name, &d) != 0) { msg("%s is not installed", name); return 1; }
        for (int k = 0; k < d.n; k++) {
            dbent_t* x = &d.e[k];
            if (x->kind == 'd') printf("d %04o            %s%s\n", x->mode, x->path, x->created ? "" : " (shared)");
            else if (x->kind == 'l') printf("l                  %s\n", x->path);
            else printf("%c %04o %10lu %s\n", x->kind, x->mode, x->size, x->path);
        }
        db_free(&d);
        return 0;
    }
    static char names[256][64];
    int n = db_names(names, 256);
    if (exists(SPKG_TXN)) printf("(an interrupted operation is pending: run 'spkg recover' as root)\n");
    for (int i = 0; i < n; i++) {
        db_t d;
        if (db_load(names[i], &d) != 0) { printf("%-24s (damaged database)\n", names[i]); continue; }
        unsigned long long bytes = 0; int files = 0;
        for (int k = 0; k < d.n; k++) if (d.e[k].kind == 'f' || d.e[k].kind == 'c') { files++; bytes += d.e[k].size; }
        printf("%-24s %-12s %6d files %10llu bytes\n", d.name, d.version, files, bytes);
        db_free(&d);
    }
    return 0;
}
static int cmd_check(const char* name) {
    db_t d;
    if (!name_ok(name) || db_load(name, &d) != 0) { msg("%s is not installed", name); return 1; }
    int bad = 0, ok = 0;
    for (int k = 0; k < d.n; k++) {
        dbent_t* x = &d.e[k];
        if (x->kind == 'd') { if (!is_dir(x->path)) { printf("MISSING %s\n", x->path); bad++; } else ok++; continue; }
        char have[65]; unsigned long sz = 0;
        if (hash_path(x->path, have, &sz) != 0) { printf("MISSING %s\n", x->path); bad++; continue; }
        if (strcmp(have, x->sha) || (x->kind != 'l' && sz != x->size)) {
            if (x->kind == 'c') { printf("modified (config) %s\n", x->path); ok++; }
            else { printf("CHANGED %s\n", x->path); bad++; }
            continue;
        }
        ok++;
    }
    printf("spkg: %s %s: %d ok, %d bad\n", d.name, d.version, ok, bad);
    db_free(&d);
    return bad ? 1 : 0;
}
static int cmd_verify(const char* file) {
    pkg_t P;
    int r = pkg_open_verify(file, &P);
    if (r == 0) {
        char dg[65]; hex(P.digest, 32, dg);
        printf("spkg: %s: OK %s %s, %u entries, %llu data bytes, digest %s\n", file, P.name, P.version, P.n,
               (unsigned long long)P.data_size, dg);
    }
    pkg_close(&P);
    return r == 0 ? 0 : 1;
}

static void usage(void) {
    fputs("usage: spkg install <file.spkg> | remove <name> | list [<name>] | verify <file.spkg>\n"
          "            | check <name> | recover\n", stderr);
}

int main(int argc, char** argv) {
    const char* k = getenv("SPKG_TEST_KILL_AT");
    if (k && *k) g_kill_at = atol(k);
    if (argc >= 2 && !strcmp(argv[1], "-q")) { g_quiet = 1; argv++; argc--; }
    if (argc == 3 && !strcmp(argv[1], "install")) return cmd_install(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "remove")) return cmd_remove(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "verify")) return cmd_verify(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "check")) return cmd_check(argv[2]);
    if (argc <= 3 && argc >= 2 && !strcmp(argv[1], "list")) return cmd_list(argc == 3 ? argv[2] : 0);
    if (argc == 2 && !strcmp(argv[1], "recover")) {
        if (need_root()) return 1;
        if (!exists(SPKG_TXN) && !is_dir(SPKG_STAGE)) return 0;
        if (lock()) return 1;
        int r = recover(1);
        unlock();
        return r == 0 ? 0 : 1;
    }
    usage();
    return 2;
}
