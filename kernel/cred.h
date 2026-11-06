/*
 * SecOS Kernel - process credentials and discretionary access control (M42)
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 *
 * Two independent gates decide what a process may do:
 *   - the code signature (mm/elf_sign.c, M9) decides WHICH CODE may run at all;
 *   - the credentials below decide WHO the running code acts for: Unix
 *     uid/gid + file modes (owner/group/other rwx), with root (euid 0) exempt
 *     from the file-permission checks except that exec needs at least one x bit.
 * This revises M26 ("POSIX metadata stored but not enforced"). The rules, and
 * every place they are enforced, are listed in docs/devlog/M42-multiuser.md.
 */
#ifndef SECOS_CRED_H
#define SECOS_CRED_H
#include <stdint.h>
#include <stddef.h>

#define CRED_NGROUPS 32          /* supplementary groups per process (NGROUPS_MAX) */

struct proc_cred {
    uint32_t ruid, euid, suid;   /* real / effective / saved-set user id   */
    uint32_t rgid, egid, sgid;   /* real / effective / saved-set group id  */
    uint32_t ngroups;
    uint32_t groups[CRED_NGROUPS];
};

/* Well-known ids (also in the default /etc/group, kernel/sysroot.c). */
#define GID_ROOT   0
#define GID_TTY    5             /* pty slaves: crw--w---- owner:tty      */
#define GID_DISK   6             /* block devices                          */
#define GID_WHEEL  10            /* sudo                                   */
#define GID_VIDEO  44            /* /dev/fb0 (still needs CAP_DISPLAY)     */
#define GID_USERS  100           /* default primary group of new users     */
#define GID_INPUT  105           /* /dev/input/eventN (still needs CAP_INPUT) */

/* Permission request bits (as in the low mode triplets). */
#define MAY_EXEC  1
#define MAY_WRITE 2
#define MAY_READ  4

/* errno values used here (returned negated). */
#define CRED_EPERM   1
#define CRED_ENOENT  2
#define CRED_ESRCH   3
#define CRED_EFAULT 14
#define CRED_EACCES 13
#define CRED_EEXIST 17
#define CRED_ENOTDIR 20
#define CRED_EINVAL 22

struct process;
struct vfs_inode;

/* Set every id to root, no supplementary groups (kernel-created processes). */
void cred_init_root(struct proc_cred* cr);

/* Kernel context (no process, or an idle task): never checked. */
int  cred_is_kernel(const struct process* c);
/* Effective root (DAC override). Kernel context counts as root. */
int  cred_is_root(const struct process* c);
/* egid or a supplementary group. */
int  cred_in_group(const struct process* c, uint32_t gid);

/* Effective mode of an inode (a filesystem without modes reports 0: 0755 for a
 * directory, 0644 otherwise, as stat() shows). Type bits included. */
uint32_t cred_inode_mode(const struct vfs_inode* ino);

/* May c access `ino` for `mask` (MAY_*)? 0 or -EACCES. Effective ids. */
int  cred_inode_perm(const struct process* c, const struct vfs_inode* ino, int mask);
/* Same with the REAL uid/gid (access(2)). */
int  cred_inode_perm_real(const struct process* c, const struct vfs_inode* ino, int mask);

/* Canonicalize the absolute path `abs` for c, checking search (x) permission on
 * every directory descended through. follow: follow a final symlink. On
 * success `out` holds the canonical path (its last component may not exist).
 * 0 or -errno (-EACCES, -ENOENT, -ENOTDIR, -ELOOP, -ENAMETOOLONG). */
int  cred_resolve(const struct process* c, const char* abs, int follow, char* out, size_t cap);

/* Resolve + lookup + require `mask` on the object. *ino_out may be NULL.
 * 0 or -errno (-ENOENT when it does not exist). */
int  cred_check_path(const struct process* c, const char* abs, int follow, int mask,
                     char* canon, size_t cap, struct vfs_inode** ino_out);

/* May c create an entry named by the canonical path `canon` (W+X on its parent
 * directory, which must exist)? 0 or -errno. */
int  cred_may_create(const struct process* c, const char* canon);
/* May c remove (unlink/rmdir/rename away) the existing entry `canon`? W+X on
 * the parent and, when the parent is sticky (/tmp), c owns the entry or the
 * parent (or is root). 0 or -errno. */
int  cred_may_delete(const struct process* c, const char* canon);
/* Owner of the inode, or root? (chmod, utimes with explicit times). */
int  cred_is_owner(const struct process* c, const struct vfs_inode* ino);

/* Owner + group a new file created by c in directory `parent_canon` gets: euid
 * and egid, or the directory's gid when it is set-group-id (BSD semantics).
 * *inherit_sgid = 1 for a new directory under a set-gid directory. */
void cred_new_owner(const struct process* c, const char* parent_canon,
                    uint32_t* uid, uint32_t* gid, int* inherit_sgid);
/* Give the freshly created `canon` its owner/group and mode (& ~umask, plus the
 * set-gid inheritance). Kernel context: only the mode. */
void cred_apply_new(const struct process* c, const char* canon, uint32_t mode, int is_dir);

/* chmod/chown/utimes policy, then the attribute change. 0 or -errno. */
int  cred_chmod(const struct process* c, const char* canon, uint32_t mode);
int  cred_chown(const struct process* c, const char* canon, uint32_t uid, uint32_t gid);
int  cred_utimes(const struct process* c, const char* canon, uint64_t atime, uint64_t mtime);

/* May `sender` signal `target` (kill(2) rules)? sig is used for the SIGCONT
 * same-session exception. 1 = allowed. */
int  cred_may_signal(const struct process* sender, const struct process* target, int sig);
/* May c inspect `target`'s private state (/proc/<pid>/{environ,fd,cwd,exe,
 * root,maps})? Root, or every id of the target equals c's effective ids
 * (a set-id program is private to root). 1 = allowed. */
int  cred_may_inspect(const struct process* c, const struct process* target);

/* Set-id exec: apply the file's S_ISUID/S_ISGID to p (called after the new
 * image was built). `allowed` = the image may honour set-id bits (root mount +
 * signed manifest flag MANIFEST_FLAG_SETID, see the devlog). Then saved ids
 * = effective ids (POSIX). Returns 1 if the effective ids changed. */
int  cred_exec_setid(struct process* p, uint32_t fmode, uint32_t fuid, uint32_t fgid, int allowed);

/* The id syscalls 100-109 (kernel/syscall.c dispatches them here). */
long cred_syscall(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2);

/* "Is this mount the root filesystem?" -- set-id bits are honoured only there
 * (every other mount behaves nosuid). */
int  cred_on_root_mount(const char* canon);

#endif
