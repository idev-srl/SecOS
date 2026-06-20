/* SecOS coreutils — filesystem applets (ls, mkdir, rm, touch, cp, basename,
 * dirname, stat). Part of the busybox-style multi-call coreutils binary; each
 * applet is `int applet_NAME(int argc, char** argv)` with argv[0] = applet name
 * and a 0 exit code on success. Freestanding ring-3: no nested functions, no
 * floating point. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "libsecos.h"
#include "coreutils.h"

/* --- small file-scope helpers (no statement-expressions / nested fns) --- */

/* Join dir + "/" + name into out (size cap). Avoids a double slash when dir
 * already ends in '/'. */
static void path_join(char* out, int cap, const char* dir, const char* name) {
    int n = (int)strlen(dir);
    if (n > 0 && dir[n - 1] == '/')
        snprintf(out, cap, "%s%s", dir, name);
    else
        snprintf(out, cap, "%s/%s", dir, name);
}

/* One-word type label for a POSIX st_mode. */
static const char* type_word(unsigned mode) {
    if (S_ISDIR(mode)) return "directory";
    if (S_ISLNK(mode)) return "symbolic link";
    if (S_ISCHR(mode)) return "character device";
    if (S_ISBLK(mode)) return "block device";
    if (S_ISREG(mode)) return "regular file";
    return "unknown";
}

/* Short type tag for `ls -l` first column. */
static const char* type_tag(unsigned mode) {
    if (S_ISDIR(mode)) return "dir ";
    if (S_ISLNK(mode)) return "link";
    if (S_ISCHR(mode)) return "char";
    if (S_ISBLK(mode)) return "blk ";
    if (S_ISREG(mode)) return "file";
    return "?   ";
}

/* --- ls --- */

/* Long listing of a single name inside dir: "<type> <size> <name>". */
static void ls_long_one(const char* dir, const char* name) {
    char full[512];
    struct stat st;
    path_join(full, sizeof full, dir, name);
    if (lstat(full, &st) == 0)
        printf("%s %8lu %s\n", type_tag(st.st_mode), st.st_size, name);
    else
        printf("%s %8s %s\n", "????", "?", name);
}

/* List one path argument. Returns 0 on success, 1 on error. */
static int ls_one(const char* path, int longfmt) {
    DIR* d = opendir(path);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != 0) {
            if (longfmt) {
                ls_long_one(path, e->d_name);
            } else if (e->d_type == DT_DIR) {
                printf("%s/\n", e->d_name);
            } else if (e->d_type == DT_LNK) {
                printf("%s@\n", e->d_name);
            } else {
                printf("%s\n", e->d_name);
            }
        }
        closedir(d);
        return 0;
    }
    /* Not a directory (or unreadable): maybe it's a plain file. */
    struct stat st;
    if (stat(path, &st) == 0) {
        if (longfmt)
            printf("%s %8lu %s\n", type_tag(st.st_mode), st.st_size, path);
        else
            printf("%s\n", path);
        return 0;
    }
    fprintf(stderr, "ls: cannot access '%s'\n", path);
    return 1;
}

int applet_ls(int argc, char** argv) {
    int longfmt = 0;
    int npaths = 0;
    int rc = 0;
    int i;

    /* First pass: count non-flag args and detect -l. */
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) longfmt = 1;
        else npaths++;
    }

    if (npaths == 0) {
        return ls_one("/", longfmt);
    }

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') continue; /* skip flags */
        if (ls_one(argv[i], longfmt) != 0) rc = 1;
    }
    return rc;
}

/* --- mkdir --- */

int applet_mkdir(int argc, char** argv) {
    int rc = 0;
    int i;
    if (argc < 2) {
        fprintf(stderr, "mkdir: missing operand\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        if (mkdir(argv[i], 0755) != 0) {
            fprintf(stderr, "mkdir: cannot create directory '%s'\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}

/* --- rm --- */

int applet_rm(int argc, char** argv) {
    int force = 0;
    int got = 0;
    int rc = 0;
    int i;

    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-f")) force = 1;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) continue;
        got = 1;
        if (unlink(argv[i]) != 0 && !force) {
            fprintf(stderr, "rm: cannot remove '%s'\n", argv[i]);
            rc = 1;
        }
    }
    if (!got && !force) {
        fprintf(stderr, "rm: missing operand\n");
        return 1;
    }
    return rc;
}

/* --- touch --- */

int applet_touch(int argc, char** argv) {
    int rc = 0;
    int i;
    if (argc < 2) {
        fprintf(stderr, "touch: missing operand\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) == 0) continue; /* already exists */
        if (creat_file(argv[i]) != 0) {
            fprintf(stderr, "touch: cannot touch '%s'\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}

/* --- cp --- */

int applet_cp(int argc, char** argv) {
    const char* src;
    const char* dst;
    int in, out;
    char buf[1024];
    ssize_t n;

    if (argc < 3) {
        fprintf(stderr, "cp: missing operand (usage: cp SRC DST)\n");
        return 1;
    }
    src = argv[1];
    dst = argv[2];

    in = open(src, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "cp: cannot open '%s'\n", src);
        return 1;
    }
    /* open() does not create: make the destination first, then open for write. */
    if (creat_file(dst) != 0) {
        fprintf(stderr, "cp: cannot create '%s'\n", dst);
        close(in);
        return 1;
    }
    out = open(dst, O_WRONLY);
    if (out < 0) {
        fprintf(stderr, "cp: cannot open '%s' for writing\n", dst);
        close(in);
        return 1;
    }

    for (;;) {
        n = read(in, buf, sizeof buf);
        if (n < 0) {
            fprintf(stderr, "cp: read error on '%s'\n", src);
            close(in);
            close(out);
            return 1;
        }
        if (n == 0) break;
        if (write(out, buf, (size_t)n) != n) {
            fprintf(stderr, "cp: write error on '%s'\n", dst);
            close(in);
            close(out);
            return 1;
        }
    }

    close(in);
    close(out);
    return 0;
}

/* --- basename --- */

int applet_basename(int argc, char** argv) {
    static char tmp[512];
    char* b;
    int n;

    if (argc < 2) {
        fprintf(stderr, "basename: missing operand\n");
        return 1;
    }

    /* Work on a mutable copy so we can strip trailing slashes. */
    snprintf(tmp, sizeof tmp, "%s", argv[1]);
    n = (int)strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/') { tmp[n - 1] = '\0'; n--; }

    /* "/" (all slashes collapsed) -> "/". */
    if (!strcmp(tmp, "/")) {
        printf("/\n");
        return 0;
    }

    b = strrchr(tmp, '/');
    b = b ? b + 1 : tmp;

    /* Optional suffix removal. */
    if (argc >= 3) {
        const char* suf = argv[2];
        int bl = (int)strlen(b);
        int sl = (int)strlen(suf);
        if (sl > 0 && sl < bl && !strcmp(b + bl - sl, suf))
            b[bl - sl] = '\0';
    }

    printf("%s\n", b);
    return 0;
}

/* --- dirname --- */

int applet_dirname(int argc, char** argv) {
    static char tmp[512];
    char* slash;
    int n;

    if (argc < 2) {
        fprintf(stderr, "dirname: missing operand\n");
        return 1;
    }

    snprintf(tmp, sizeof tmp, "%s", argv[1]);
    n = (int)strlen(tmp);
    /* Strip trailing slashes (but keep a lone "/"). */
    while (n > 1 && tmp[n - 1] == '/') { tmp[n - 1] = '\0'; n--; }

    slash = strrchr(tmp, '/');
    if (!slash) {
        printf(".\n");          /* no slash at all -> "." per POSIX */
        return 0;
    }
    if (slash == tmp) {
        printf("/\n");          /* leading slash only -> "/" */
        return 0;
    }
    *slash = '\0';
    /* Collapse any trailing slashes left in the directory part. */
    n = (int)strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/') { tmp[n - 1] = '\0'; n--; }
    printf("%s\n", tmp);
    return 0;
}

/* --- stat --- */

int applet_stat(int argc, char** argv) {
    struct stat st;

    if (argc < 2) {
        fprintf(stderr, "stat: missing operand\n");
        return 1;
    }
    if (lstat(argv[1], &st) != 0) {
        fprintf(stderr, "stat: cannot stat '%s'\n", argv[1]);
        return 1;
    }

    printf("  File: %s\n", argv[1]);
    printf("  Size: %lu\n", st.st_size);
    printf("  Type: %s\n", type_word(st.st_mode));
    printf("  Mode: 0%o\n", st.st_mode & 0xFFFF);
    printf("   Uid: %u   Gid: %u\n", st.st_uid, st.st_gid);
    printf(" Links: %u\n", st.st_nlink);
    return 0;
}
