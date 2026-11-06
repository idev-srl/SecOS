/* acct.h - shared helpers of the SecOS account tools (login, su, sudo, passwd,
 * useradd & co., see docs/USERS.md).
 *
 * The account database is the classic trio /etc/passwd (0644), /etc/group
 * (0644) and /etc/shadow (0600, root only). Every change follows the same
 * protocol: take the lock /etc/.pwd.lock (O_CREAT|O_EXCL, stale locks of dead
 * processes are broken), read the whole file, edit it in memory, write
 * "<file>+" with the final owner and mode, fsync it and rename it over the
 * original. A crash leaves the old or the new file, never a mix, and the
 * shadow copy is never readable by anyone but root, not even for a moment.
 *
 * Passwords are hashed only with the vendored musl crypt (SHA-512 crypt,
 * "$6$", via secos_make_salt) and verified only with secos_verify_password
 * (constant-time compare). Password buffers are wiped after use and never
 * echoed. SPDX-License-Identifier: MIT */
#ifndef SECOS_ACCT_H
#define SECOS_ACCT_H
#include <stddef.h>

#define ACCT_PASSWD   "/etc/passwd"
#define ACCT_GROUP    "/etc/group"
#define ACCT_SHADOW   "/etc/shadow"
#define ACCT_LOCK     "/etc/.pwd.lock"
#define ACCT_AUTHLOG  "/var/log/auth.log"
#define ACCT_SKEL     "/etc/skel"

#define ACCT_UID_MIN      1000   /* regular users and their groups */
#define ACCT_UID_MAX      59999
#define ACCT_SYS_UID_MIN  100    /* useradd -r / groupadd -r */
#define ACCT_SYS_UID_MAX  999
#define ACCT_NAME_MAX     32
#define ACCT_PW_MAX       256    /* longest password read */
#define ACCT_HASH_MAX     160

/* Well-known groups (kernel/cred.h, kernel/sysroot.c). */
#define ACCT_GID_TTY    5
#define ACCT_GID_WHEEL  10
#define ACCT_GID_VIDEO  44
#define ACCT_GID_INPUT  105

#define ACCT_PATH_USER  "/usr/local/bin:/usr/bin:/bin"
#define ACCT_PATH_ROOT  "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

extern const char* acct_prog;            /* name used in messages and the log */

void acct_err(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void acct_wipe(void* p, size_t n);
/* Append one line to /var/log/auth.log (root:root 0600). Needs euid 0. */
void acct_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- whole-file editing ---------------------------------------------------- */
typedef struct { char* buf; size_t len, cap; } acct_text_t;
/* Load `path` (a missing file loads as empty). 0, or -1 with errno set. */
int  acct_text_load(acct_text_t* t, const char* path);
void acct_text_free(acct_text_t* t);     /* wipes the buffer (shadow!) */
/* The line whose first ':' field is `key`: 1 found (off/len set, len without the
 * newline), 0 not found. */
int  acct_text_find(const acct_text_t* t, const char* key, size_t* off, size_t* len);
/* Replace the line of `key` by `line` (no newline), append it when absent, or
 * delete it when line is NULL. 0, or -1 (out of memory). */
int  acct_text_set(acct_text_t* t, const char* key, const char* line);
/* Atomic rewrite: <path>+ with root:<gid> <mode>, fsync, rename. 0 or -1. */
int  acct_text_commit(const acct_text_t* t, const char* path, unsigned mode, unsigned gid);
/* Copy field `idx` (0-based) of a line of `len` bytes. 0, or -1 when the field
 * does not exist or does not fit. */
int  acct_field(const char* line, size_t len, int idx, char* out, size_t cap);

int  acct_lock(void);                    /* 0, or -1 (message printed) */
void acct_unlock(void);

/* ---- lookups (straight from the files, copies) ----------------------------- */
struct acct_user {
    char     name[ACCT_NAME_MAX + 1];
    unsigned uid, gid;
    char     gecos[128], home[128], shell[128];
};
int  acct_user_by_name(const char* name, struct acct_user* u);   /* 0 found, -1 not */
int  acct_user_by_uid(unsigned uid, struct acct_user* u);
int  acct_group_by_name(const char* name, unsigned* gid);        /* 0 found, -1 not */
int  acct_group_name(unsigned gid, char* out, size_t cap);       /* 0 found, -1 not */
/* "wheel" or "10": a group name or number that exists. 0 / -1. */
int  acct_group_resolve(const char* s, unsigned* gid);
/* Is `user` (primary group `pgid`) a member of `gid` per /etc/group? */
int  acct_user_in_group(const char* user, unsigned pgid, unsigned gid);
/* Every group of `user`: primary first, then the /etc/group memberships. */
int  acct_user_groups(const char* user, unsigned pgid, unsigned* out, int cap);
int  acct_valid_name(const char* s);

/* ---- passwords ------------------------------------------------------------- */
/* The shadow hash of `name` (needs euid 0). 0, or -1 when there is no entry. */
int  acct_get_hash(const char* name, char* out, size_t cap);
/* 1 when `plain` is the password of `name`; 0 otherwise (unknown users and
 * locked accounts included; the cost of one crypt() is paid either way). */
int  acct_check_password(const char* name, const char* plain);
/* SHA-512 crypt of `plain` with a fresh random salt. 0 or -1. */
int  acct_hash_password(const char* plain, char* out, size_t cap);
/* Under the lock: set the shadow hash of `name` (adding the entry if missing)
 * and the last-change day. 0 or -1. */
int  acct_set_hash(const char* name, const char* hash);
/* Read a password from the controlling terminal (echo off, signals restore the
 * terminal). Without a terminal: from stdin. Length, or -1 on EOF. */
int  acct_read_secret(const char* prompt, char* buf, size_t cap);
/* Read a line with echo (user names). Length, or -1 on EOF. */
int  acct_read_line(const char* prompt, char* buf, size_t cap);
/* Ask for a new password twice and hash it. min_len: shortest accepted (0 =
 * any non-empty). 0 ok, 1 empty answer, -1 mismatch / too short / error. */
int  acct_new_password(const char* prompt1, int min_len, char* hash, size_t cap);

/* ---- sessions -------------------------------------------------------------- */
/* Switch to the user for good: supplementary groups (initgroups), then
 * setresgid, then setresuid; verified afterwards. 0, or -1. */
int  acct_become(const struct acct_user* u);
/* Environment of a new session: HOME USER LOGNAME SHELL PATH (+ TERM kept or
 * "linux"); keep != 0 also keeps the other inherited variables. Static. */
char** acct_session_env(const struct acct_user* u, int keep, char* const* extra);
/* exec the user's shell (login shell: argv[0] "-name"), with `cmd` as -c. */
void acct_exec_shell(const struct acct_user* u, int login, const char* cmd, char** envp)
    __attribute__((noreturn));
/* Controlling-terminal name for messages and the log ("/dev/console", ...). */
const char* acct_tty(void);

/* ---- account creation (useradd, first boot) -------------------------------- */
struct acct_newuser {
    const char* name;
    long        uid;          /* -1: first free id */
    const char* group;        /* primary group (name or gid); NULL: new group named like the user */
    const char* groups;       /* comma-separated supplementary groups, or NULL */
    const char* home;         /* NULL: /home/<name> */
    const char* shell;        /* NULL: /bin/bash */
    const char* gecos;        /* NULL: "" */
    const char* hash;         /* NULL: "!" (locked until passwd) */
    int         create_home;  /* make the home (0700 + /etc/skel) */
    int         system;       /* ids from the system range */
};
/* 0, or -1 (message printed). *uid_out: the new uid. */
int  acct_useradd(const struct acct_newuser* nu, unsigned* uid_out);
/* Add `user` to the member list of group `gid` (text already loaded). */
int  acct_group_add_member(acct_text_t* grp, unsigned gid, const char* user);
/* Remove `user` from every member list. Returns the number of changes. */
int  acct_group_del_member(acct_text_t* grp, const char* user);
/* mkdir 0700 + copy /etc/skel, everything owned by uid:gid. 0 or -1. */
int  acct_make_home(const char* home, unsigned uid, unsigned gid);

#endif
