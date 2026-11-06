/*
 * SecOS Kernel - process credentials and discretionary access control (M42)
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 *
 * See cred.h for the model. The rules implemented here (all "deny when in
 * doubt"):
 *
 *  File access   owner bits if euid == file uid, else group bits if egid or a
 *                supplementary group == file gid, else other bits. Root (euid
 *                0) passes every read/write/search check; exec of a non-
 *                directory needs at least one x bit, even for root.
 *  Path walk     search (x) on every directory descended through, including
 *                the ones met while following symlinks (vfs_walk_checked).
 *  Create        w+x on the parent directory. New entries are owned by euid
 *                and egid (or the parent's gid if it is set-gid; a new
 *                directory then inherits S_ISGID).
 *  Delete/rename w+x on the parent; in a sticky directory (/tmp) only the
 *                entry's owner, the directory's owner or root.
 *  chmod         owner or root; a non-root owner's S_ISGID is dropped unless
 *                the file's group is one of the caller's groups.
 *  chown         root changes anything; the owner may only change the group,
 *                to one of its own groups. A chown of an executable or set-id
 *                file clears S_ISUID/S_ISGID (as Linux, root included).
 *  utimes        explicit times: owner or root; "now" (UTIME_NOW): also
 *                anyone with write permission.
 *  signals       root, or the sender's real or effective uid equals the
 *                target's real or saved uid; SIGCONT also within a session.
 *  id changes    POSIX: root may set any id; others only switch among their
 *                own real/effective/saved ids; setgroups is root only.
 *  set-id exec   only for images on the root filesystem whose signed
 *                manifest carries MANIFEST_FLAG_SETID; S_ISGID needs g+x.
 */
#include "cred.h"
#include "process.h"
#include "debugcon.h"
#include "../fs/vfs.h"
#include "../mm/user_copy.h"
#include "../kernel/syscall.h"

#define S_IFMT_   0170000u
#define S_IFDIR_  0040000u
#define S_IFREG_  0100000u
#define S_IFLNK_  0120000u
#define S_ISUID_  04000u
#define S_ISGID_  02000u
#define S_ISVTX_  01000u
#define CRED_UNCHANGED 0xFFFFFFFFu
#define UTIME_NOW_SENTINEL 0xFFFFFFFFFFFFFFFFull

extern uint64_t kclock_real_sec(void);

void cred_init_root(struct proc_cred* cr){
    cr->ruid = cr->euid = cr->suid = 0;
    cr->rgid = cr->egid = cr->sgid = 0;
    cr->ngroups = 0;
    for(int i=0;i<CRED_NGROUPS;i++) cr->groups[i] = 0;
}

int cred_is_kernel(const process_t* c){ return !c || c->pid == 0; }
int cred_is_root(const process_t* c){ return cred_is_kernel(c) || c->cred.euid == 0; }

static int in_group_ids(const struct proc_cred* cr, uint32_t egid, uint32_t gid){
    if(egid == gid) return 1;
    for(uint32_t i=0;i<cr->ngroups && i<CRED_NGROUPS;i++) if(cr->groups[i] == gid) return 1;
    return 0;
}
int cred_in_group(const process_t* c, uint32_t gid){
    if(cred_is_kernel(c)) return 1;
    return in_group_ids(&c->cred, c->cred.egid, gid);
}

uint32_t cred_inode_mode(const vfs_inode_t* ino){
    if(!ino) return 0;
    uint32_t m = ino->mode;
    uint32_t t = ino->type==VFS_NODE_DIR ? S_IFDIR_ : ino->type==VFS_NODE_SYMLINK ? S_IFLNK_ : S_IFREG_;
    if(m == 0) return t | (ino->type==VFS_NODE_DIR ? 0755u : 0644u);   /* FS keeps no modes */
    if(!(m & S_IFMT_)) m |= t;
    return m;
}

/* The DAC decision for an explicit (uid, gid-set) identity. */
static int perm_ids(uint32_t uid, uint32_t gid, const struct proc_cred* cr,
                    const vfs_inode_t* ino, int mask){
    uint32_t m = cred_inode_mode(ino);
    mask &= 7;
    if(uid == 0){
        if((mask & MAY_EXEC) && (m & S_IFMT_) != S_IFDIR_ && !(m & 0111u)) return -CRED_EACCES;
        return 0;
    }
    uint32_t bits;
    if(uid == ino->uid) bits = (m >> 6) & 7;
    else if(in_group_ids(cr, gid, ino->gid)) bits = (m >> 3) & 7;
    else bits = m & 7;
    return ((bits & (uint32_t)mask) == (uint32_t)mask) ? 0 : -CRED_EACCES;
}

/* Linux inode_permission(): asking to write a file, directory or symlink of a
 * read-only filesystem fails with EROFS before any DAC check, root included
 * (so a write to an NTFS volume reports EROFS, not EACCES). */
static int ro_fs(const vfs_inode_t* ino){
    return ino && ino->ops && (ino->ops->flags & VFS_FS_RDONLY);
}

int cred_inode_perm(const process_t* c, const vfs_inode_t* ino, int mask){
    if(cred_is_kernel(c) || !ino) return 0;
    if((mask & MAY_WRITE) && ro_fs(ino)) return -CRED_EROFS;
    return perm_ids(c->cred.euid, c->cred.egid, &c->cred, ino, mask);
}
int cred_inode_perm_real(const process_t* c, const vfs_inode_t* ino, int mask){
    if(cred_is_kernel(c) || !ino) return 0;
    if((mask & MAY_WRITE) && ro_fs(ino)) return -CRED_EROFS;
    return perm_ids(c->cred.ruid, c->cred.rgid, &c->cred, ino, mask);
}

static int search_cb(void* ctx, const vfs_inode_t* dir){
    return cred_inode_perm((const process_t*)ctx, dir, MAY_EXEC);
}

int cred_resolve(const process_t* c, const char* abs, int follow, char* out, size_t cap){
    if(cred_is_root(c)) return vfs_walk(abs, out, cap, follow);    /* DAC override: no search checks */
    return vfs_walk_checked(abs, out, cap, follow, search_cb, (void*)c);
}

int cred_check_path(const process_t* c, const char* abs, int follow, int mask,
                    char* canon, size_t cap, vfs_inode_t** ino_out){
    char tmp[256];
    if(!canon){ canon = tmp; cap = sizeof(tmp); }
    if(ino_out) *ino_out = NULL;
    int e = cred_resolve(c, abs, follow, canon, cap);
    if(e) return e;
    vfs_inode_t* ino = vfs_lookup(canon);
    if(!ino) return -CRED_ENOENT;
    if(mask){ e = cred_inode_perm(c, ino, mask); if(e) return e; }
    if(ino_out) *ino_out = ino;
    return 0;
}

static void parent_of(const char* p, char* out, size_t cap){
    size_t ls = 0; for(size_t i=0;p[i];i++) if(p[i]=='/') ls = i;
    if(ls == 0){ out[0]='/'; out[1]=0; return; }
    if(ls > cap-1) ls = cap-1;
    for(size_t i=0;i<ls;i++) out[i]=p[i];
    out[ls]=0;
}

int cred_may_create(const process_t* c, const char* canon){
    char par[256]; parent_of(canon, par, sizeof(par));
    vfs_inode_t* d = vfs_lookup(par);
    if(!d) return -CRED_ENOENT;
    if(d->type != VFS_NODE_DIR) return -CRED_ENOTDIR;
    return cred_inode_perm(c, d, MAY_WRITE | MAY_EXEC);
}

int cred_may_delete(const process_t* c, const char* canon){
    char par[256]; parent_of(canon, par, sizeof(par));
    vfs_inode_t* d = vfs_lookup(par);
    if(!d) return -CRED_ENOENT;
    if(d->type != VFS_NODE_DIR) return -CRED_ENOTDIR;
    int e = cred_inode_perm(c, d, MAY_WRITE | MAY_EXEC);
    if(e) return e;
    if(cred_is_root(c)) return 0;
    uint32_t dmode = cred_inode_mode(d), duid = d->uid;   /* d may be recycled by the next lookup */
    vfs_inode_t* v = vfs_lookup(canon);
    if(!v) return -CRED_ENOENT;
    if((dmode & S_ISVTX_) && v->uid != c->cred.euid && duid != c->cred.euid) return -CRED_EPERM;
    return 0;
}

int cred_is_owner(const process_t* c, const vfs_inode_t* ino){
    if(cred_is_root(c)) return 1;
    return ino && ino->uid == c->cred.euid;
}

void cred_new_owner(const process_t* c, const char* parent_canon,
                    uint32_t* uid, uint32_t* gid, int* inherit_sgid){
    *uid = cred_is_kernel(c) ? 0 : c->cred.euid;
    *gid = cred_is_kernel(c) ? 0 : c->cred.egid;
    if(inherit_sgid) *inherit_sgid = 0;
    vfs_inode_t* d = vfs_lookup(parent_canon);
    if(d && (cred_inode_mode(d) & S_ISGID_)){
        *gid = d->gid;
        if(inherit_sgid) *inherit_sgid = 1;
    }
}

void cred_apply_new(const process_t* c, const char* canon, uint32_t mode, int is_dir){
    uint32_t um = c ? c->ki.umask : 022;
    uint32_t want = ((mode ? mode : (is_dir ? 0777u : 0666u)) & 07777u) & ~um;
    char par[256]; parent_of(canon, par, sizeof(par));
    uint32_t uid, gid; int sg;
    cred_new_owner(c, par, &uid, &gid, &sg);
    if(sg && is_dir) want |= S_ISGID_;
    /* S_ISGID on a new file only if the caller is in its group (Linux). */
    if(!is_dir && (want & S_ISGID_) && !cred_is_root(c) && !cred_in_group(c, gid)) want &= ~S_ISGID_;
    vfs_inode_t* ino = vfs_lookup(canon);
    if(!ino) return;
    uint32_t have = cred_inode_mode(ino) & 07777u;
    unsigned valid = 0;
    vfs_attr_t a; a.mode = want; a.uid = uid; a.gid = gid; a.atime = a.mtime = 0;
    if(have != want || ino->mode == 0) valid |= VFS_ATTR_MODE;
    if(ino->uid != uid) valid |= VFS_ATTR_UID;
    if(ino->gid != gid) valid |= VFS_ATTR_GID;
    if(valid) vfs_lsetattr(canon, &a, valid);
}

int cred_chmod(const process_t* c, const char* canon, uint32_t mode){
    vfs_inode_t* ino = vfs_lookup(canon);
    if(!ino) return -CRED_ENOENT;
    if(!cred_is_kernel(c) && ro_fs(ino)) return -CRED_EROFS;
    if(!cred_is_owner(c, ino)) return -CRED_EPERM;
    uint32_t m = mode & 07777u;
    if(!cred_is_root(c) && (m & S_ISGID_) && ino->type != VFS_NODE_DIR && !cred_in_group(c, ino->gid))
        m &= ~S_ISGID_;
    vfs_attr_t a; a.mode = m; a.uid = a.gid = 0; a.atime = a.mtime = 0;
    int r = vfs_lsetattr(canon, &a, VFS_ATTR_MODE);
    return r < 0 ? (r == -1 ? -5 /*EIO*/ : r) : 0;
}

int cred_chown(const process_t* c, const char* canon, uint32_t uid, uint32_t gid){
    vfs_inode_t* ino = vfs_lookup(canon);
    if(!ino) return -CRED_ENOENT;
    if(!cred_is_kernel(c) && ro_fs(ino)) return -CRED_EROFS;
    uint32_t ouid = ino->uid, ogid = ino->gid, omode = cred_inode_mode(ino);
    int is_dir = ino->type == VFS_NODE_DIR;
    int chu = uid != CRED_UNCHANGED && uid != ouid;
    int chg = gid != CRED_UNCHANGED && gid != ogid;
    if(!cred_is_root(c)){
        if(chu) return -CRED_EPERM;                       /* giving files away: root only */
        if(chg && (c->cred.euid != ouid || !cred_in_group(c, gid))) return -CRED_EPERM;
        if(!chg && c->cred.euid != ouid) return -CRED_EPERM;
    }
    vfs_attr_t a; a.uid = uid; a.gid = gid; a.mode = omode & 07777u; a.atime = a.mtime = 0;
    unsigned valid = 0;
    if(uid != CRED_UNCHANGED) valid |= VFS_ATTR_UID;
    if(gid != CRED_UNCHANGED) valid |= VFS_ATTR_GID;
    if(!valid) return 0;
    /* A changed owner/group of an executable or set-id file drops set-id. */
    if(!is_dir && (chu || chg) && (omode & (S_ISUID_ | S_ISGID_))) {
        uint32_t nm = omode & 07777u & ~S_ISUID_;
        if(omode & 0010u) nm &= ~S_ISGID_;                /* S_ISGID without g+x = lock marker: kept */
        if(nm != (omode & 07777u)){ a.mode = nm; valid |= VFS_ATTR_MODE; }
    }
    int r = vfs_lsetattr(canon, &a, valid);
    return r < 0 ? (r == -1 ? -5 : r) : 0;
}

int cred_utimes(const process_t* c, const char* canon, uint64_t at, uint64_t mt){
    vfs_inode_t* ino = vfs_lookup(canon);
    if(!ino) return -CRED_ENOENT;
    if(!cred_is_kernel(c) && ro_fs(ino)) return -CRED_EROFS;
    int now = (at == UTIME_NOW_SENTINEL && mt == UTIME_NOW_SENTINEL);
    if(!cred_is_owner(c, ino)){
        if(!now) return -CRED_EPERM;
        int e = cred_inode_perm(c, ino, MAY_WRITE);
        if(e) return e;
    }
    if(at == UTIME_NOW_SENTINEL) at = kclock_real_sec();
    if(mt == UTIME_NOW_SENTINEL) mt = kclock_real_sec();
    vfs_attr_t a; a.atime = at; a.mtime = mt; a.mode = 0; a.uid = a.gid = 0;
    int r = vfs_lsetattr(canon, &a, VFS_ATTR_ATIME | VFS_ATTR_MTIME);
    return r < 0 ? (r == -1 ? -5 : r) : 0;
}

int cred_may_signal(const process_t* s, const process_t* t, int sig){
    if(cred_is_kernel(s) || !t) return 1;
    if(s->cred.euid == 0) return 1;
    if(s->cred.euid == t->cred.ruid || s->cred.euid == t->cred.suid ||
       s->cred.ruid == t->cred.ruid || s->cred.ruid == t->cred.suid) return 1;
    if(sig == 18 /*SIGCONT*/ && s->sid == t->sid) return 1;
    return 0;
}

int cred_may_inspect(const process_t* c, const process_t* t){
    if(cred_is_root(c) || !t) return 1;
    uint32_t u = c->cred.euid, g = c->cred.egid;
    return t->cred.ruid == u && t->cred.euid == u && t->cred.suid == u &&
           t->cred.rgid == g && t->cred.egid == g && t->cred.sgid == g;
}

int cred_on_root_mount(const char* canon){
    const char* mp = vfs_mount_point_for(canon);
    return mp && mp[0] == '/' && mp[1] == 0;
}

static void cred_log(const process_t* p, const char* what){
    debugcon_writestring("[CRED] pid="); debugcon_print_hex(p->pid);
    debugcon_writestring(" "); debugcon_writestring(what);
    debugcon_writestring(" uid="); debugcon_print_hex(p->cred.ruid);
    debugcon_writestring("/"); debugcon_print_hex(p->cred.euid);
    debugcon_writestring("/"); debugcon_print_hex(p->cred.suid);
    debugcon_writestring(" gid="); debugcon_print_hex(p->cred.rgid);
    debugcon_writestring("/"); debugcon_print_hex(p->cred.egid);
    debugcon_writestring("/"); debugcon_print_hex(p->cred.sgid);
    debugcon_writestring("\n");
}

int cred_exec_setid(process_t* p, uint32_t fmode, uint32_t fuid, uint32_t fgid, int allowed){
    uint32_t oe = p->cred.euid, og = p->cred.egid;
    int want = (fmode & S_ISUID_) || ((fmode & S_ISGID_) && (fmode & 0010u));
    if(want && allowed){
        if(fmode & S_ISUID_) p->cred.euid = fuid;
        if((fmode & S_ISGID_) && (fmode & 0010u)) p->cred.egid = fgid;
    } else if(want){
        debugcon_writestring("[CRED] set-id bit ignored (not on the root fs, or the manifest lacks SETID)\n");
    }
    p->cred.suid = p->cred.euid;                          /* POSIX: saved = effective after exec */
    p->cred.sgid = p->cred.egid;
    int changed = (p->cred.euid != oe || p->cred.egid != og);
    if(changed) cred_log(p, "set-id exec");
    return changed;
}

/* ---- the id syscalls (100-109) -------------------------------------------- */
static int is_one_of(uint32_t v, const uint32_t* set){ return v==set[0] || v==set[1] || v==set[2]; }

static long set_res(process_t* c, uint32_t* ids, uint32_t r, uint32_t e, uint32_t s, int priv, const char* what){
    if(!priv){
        if(r != CRED_UNCHANGED && !is_one_of(r, ids)) return -CRED_EPERM;
        if(e != CRED_UNCHANGED && !is_one_of(e, ids)) return -CRED_EPERM;
        if(s != CRED_UNCHANGED && !is_one_of(s, ids)) return -CRED_EPERM;
    }
    if(r != CRED_UNCHANGED) ids[0] = r;
    if(e != CRED_UNCHANGED) ids[1] = e;
    if(s != CRED_UNCHANGED) ids[2] = s;
    (void)c; (void)what;
    return 0;
}

long cred_syscall(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2){
    extern process_t* sched_get_current(void);
    process_t* c = sched_get_current();
    if(!c) return -CRED_EINVAL;
    struct proc_cred* cr = &c->cred;
    int priv = cr->euid == 0;                             /* CAP_SETUID/CAP_SETGID == root */
    switch(num){
    case SYS_GETUID: case SYS_GETGID: {
        uint32_t v[3];
        if(num == SYS_GETUID){ v[0]=cr->ruid; v[1]=cr->euid; v[2]=cr->suid; }
        else                 { v[0]=cr->rgid; v[1]=cr->egid; v[2]=cr->sgid; }
        if(a0 && copy_to_user((void*)a0, v, sizeof(v)) != 0) return -CRED_EFAULT;  /* getres[ug]id */
        return (long)v[0];
    }
    case SYS_GETEUID: return (long)cr->euid;
    case SYS_GETEGID: return (long)cr->egid;
    case SYS_SETUID: {
        uint32_t u = (uint32_t)a0;
        if(u == CRED_UNCHANGED) return -CRED_EINVAL;
        if(priv){ cr->ruid = cr->euid = cr->suid = u; cred_log(c, "setuid"); return 0; }
        if(u == cr->ruid || u == cr->suid){ cr->euid = u; cred_log(c, "setuid"); return 0; }
        return -CRED_EPERM;
    }
    case SYS_SETGID: {
        uint32_t g = (uint32_t)a0;
        if(g == CRED_UNCHANGED) return -CRED_EINVAL;
        if(priv){ cr->rgid = cr->egid = cr->sgid = g; cred_log(c, "setgid"); return 0; }
        if(g == cr->rgid || g == cr->sgid){ cr->egid = g; cred_log(c, "setgid"); return 0; }
        return -CRED_EPERM;
    }
    case SYS_SETRESUID: {
        uint32_t ids[3] = { cr->ruid, cr->euid, cr->suid };
        long r = set_res(c, ids, (uint32_t)a0, (uint32_t)a1, (uint32_t)a2, priv, "setresuid");
        if(r == 0){ cr->ruid = ids[0]; cr->euid = ids[1]; cr->suid = ids[2]; cred_log(c, "setresuid"); }
        return r;
    }
    case SYS_SETRESGID: {
        uint32_t ids[3] = { cr->rgid, cr->egid, cr->sgid };
        long r = set_res(c, ids, (uint32_t)a0, (uint32_t)a1, (uint32_t)a2, priv, "setresgid");
        if(r == 0){ cr->rgid = ids[0]; cr->egid = ids[1]; cr->sgid = ids[2]; cred_log(c, "setresgid"); }
        return r;
    }
    case SYS_GETGROUPS: {
        int64_t n = (int64_t)a0;
        if(n < 0) return -CRED_EINVAL;
        if(n == 0) return (long)cr->ngroups;
        if((uint64_t)n < cr->ngroups) return -CRED_EINVAL;
        if(cr->ngroups && copy_to_user((void*)a1, cr->groups, cr->ngroups * sizeof(uint32_t)) != 0) return -CRED_EFAULT;
        return (long)cr->ngroups;
    }
    case SYS_SETGROUPS: {
        int64_t n = (int64_t)a0;
        if(!priv) return -CRED_EPERM;
        if(n < 0 || n > CRED_NGROUPS) return -CRED_EINVAL;
        uint32_t g[CRED_NGROUPS];
        if(n && copy_from_user(g, (const void*)a1, (size_t)n * sizeof(uint32_t)) != 0) return -CRED_EFAULT;
        for(int64_t i=0;i<n;i++) cr->groups[i] = g[i];
        cr->ngroups = (uint32_t)n;
        cred_log(c, "setgroups");
        return 0;
    }
    }
    (void)a2;
    return -38; /* ENOSYS */
}
