/*
 * SecOS Kernel - system root provisioning
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * The kernel image carries the signed system programs. At every boot this file
 * puts them on the root filesystem, whichever it is:
 *
 *   ramfs root (no system disk): everything is created fresh, as before. The
 *     ramfs has no symlinks, so a multi-call alias is a copy of its program
 *     (or, for BusyBox, the small re-exec stub user/bbstub.c).
 *
 *   persistent root (ext2/ext4 disk with /.secosroot): the same table is
 *     reconciled against the disk.
 *       SF_BIN    system program, owned by the kernel image. Rewritten (new file
 *                 + rename, so a crash leaves the old or the new one) when its
 *                 content changed: the SHA-256 of the embedded copy is compared
 *                 with the one recorded in /var/lib/secos/manifest, and the
 *                 file's size + mtime with what was recorded when it was
 *                 written. An unchanged program costs one lookup.
 *       SF_ALIAS  extra name of a multi-call program: a relative symlink in
 *                 /bin (`ls -> busybox`), so upgrading the program upgrades
 *                 every name at once.
 *       SF_CONF   configuration: created only when missing, then the user's.
 *     A program listed in the old manifest but no longer in the kernel is
 *     removed, unless it was modified since it was written.
 *
 *   Both: /etc/secos-release and /VERSION describe the running build
 *   (rewritten only when they differ); baked SSH keys are appended to
 *   /root/.ssh/authorized_keys when missing (user-added lines are kept).
 *   /root, /home, /var, /tmp and /etc/dropbear (the SSH host key) are never
 *   touched.
 */
#include <stdint.h>
#include <stddef.h>
#include "debugcon.h"
#include "terminal.h"
#include "../fs/vfs.h"
#include "../crypto/sha256.h"
#include "../drivers/keyboard.h"
#include "sysroot.h"

extern uint64_t timer_get_ticks(void);
extern int vfs_read_all(const char*, void*, size_t);

/* ---- embedded signed programs ------------------------------------------- */
#include "../crypto/user_coreutils_elf.h"
#include "../crypto/user_init_elf.h"
#include "../crypto/user_svc_elf.h"
#include "../crypto/user_sysfuzz_elf.h"
#include "../crypto/user_uaccess_elf.h"
#include "../crypto/user_abi_test_elf.h"
#include "../crypto/user_ttytest_elf.h"
#include "../crypto/user_ptytest_elf.h"
#include "../crypto/user_kitest_elf.h"
#include "../crypto/user_pipe_test_elf.h"
#include "../crypto/user_fbdemo_elf.h"
#include "../crypto/user_guitest_elf.h"
#include "../crypto/user_guitest_nocap_elf.h"
#include "../crypto/user_loadkeys_elf.h"
#include "../crypto/user_secwm_elf.h"
#include "../crypto/user_sysinfo_elf.h"
#include "../crypto/user_swptest_elf.h"
#include "../crypto/user_sysmon_elf.h"   // LVGL 9.2 system monitor (make port-lvgl)
#ifndef SECOS_NO_BUSYBOX
#include "../crypto/user_busybox_elf.h"
#include "../crypto/user_bbstub_elf.h"
#include "busybox_applets.h"
#endif
#ifndef SECOS_NO_BASH
#include "../crypto/user_bash_elf.h"
#endif
#ifndef SECOS_NO_LUA
#include "../crypto/user_lua_elf.h"
#endif
#include "../crypto/user_power_elf.h"    // /bin/{poweroff,reboot,halt,shutdown}
#ifndef SECOS_NO_DROPBEAR
#include "../crypto/user_dropbear_elf.h"
#endif
#include "../crypto/user_netcfg_elf.h"
#include "../build/authorized_keys.h"
#if defined(SECOS_SYSROOT_TEST)
// Selftest hook (tools/selftest.sh, persistent-root gate): /bin/sysroot_probe
// is one signed program in build 1 and another in build 2, to prove that an
// upgraded kernel refreshes exactly the program that changed.
#include "../crypto/user_hello_elf.h"
#include "../crypto/user_m16_child_elf.h"
#endif

/* ---- small string helpers ------------------------------------------------ */
static size_t sr_len(const char* s){ size_t n=0; while(s[n]) n++; return n; }
static int sr_eq(const char* a, const char* b){ while(*a && *a==*b){ a++; b++; } return *a==*b; }
static int sr_eqn(const char* a, const char* b, size_t n){ for(size_t i=0;i<n;i++) if(a[i]!=b[i]) return 0; return 1; }
static void sr_cpy(char* d, const char* s, size_t cap){ size_t i=0; for(; s[i] && i<cap-1; i++) d[i]=s[i]; d[i]=0; }
static void sr_cat(char* d, const char* s, size_t cap){ size_t n=sr_len(d); sr_cpy(d+n, s, cap-n); }
static void dbg_dec(uint64_t v){
    char b[24]; int i=23; b[i]=0;
    do { b[--i]=(char)('0'+v%10); v/=10; } while(v && i>0);
    debugcon_writestring(&b[i]);
}
static void con_dec(uint64_t v){
    char b[24]; int i=23; b[i]=0;
    do { b[--i]=(char)('0'+v%10); v/=10; } while(v && i>0);
    terminal_writestring(&b[i]);
}

/* ---- root state ------------------------------------------------------------ */
static int  g_persistent;
static char g_rootdev[16] = "ramfs";
void sysroot_set_root(const char* dev, int persistent){
    g_persistent = persistent;
    sr_cpy(g_rootdev, dev ? dev : "ramfs", sizeof(g_rootdev));
}
int sysroot_is_persistent(void){ return g_persistent; }
const char* sysroot_root_dev(void){ return g_rootdev; }

/* ---- the table ------------------------------------------------------------- */
enum { SF_BIN = 1, SF_ALIAS, SF_CONF };
#define SF_DISK_ONLY 0x1     /* alias that only exists as a symlink (no ramfs copy) */
typedef struct {
    char           path[64];
    const uint8_t* data;     /* BIN/CONF content; ALIAS: what ramfs gets instead */
    size_t         len;
    const char*    target;   /* ALIAS: symlink target, relative to the link's dir */
    uint8_t        kind, flags;
} sysfile_t;
#define SF_MAX 320
static sysfile_t g_sf[SF_MAX];
static int g_nsf;

/* Add or replace (same path: the later entry wins, e.g. BusyBox over the
 * coreutils applet of the same name, netcfg's ifconfig over BusyBox's). */
static void sf_add(int kind, const char* path, const void* data, size_t len, const char* target, int flags){
    sysfile_t* e = 0;
    for(int i=0;i<g_nsf;i++) if(sr_eq(g_sf[i].path, path)){ e=&g_sf[i]; break; }
    if(!e){
        if(g_nsf>=SF_MAX){ debugcon_writestring("[SYSROOT] table full\n"); return; }
        e=&g_sf[g_nsf++];
    }
    sr_cpy(e->path, path, sizeof(e->path));
    e->data=(const uint8_t*)data; e->len=len; e->target=target; e->kind=(uint8_t)kind; e->flags=(uint8_t)flags;
}
static void bin_path(char* out, const char* name){ sr_cpy(out, "/bin/", 64); sr_cat(out, name, 64); }
static void sf_bin(const char* name, const void* d, size_t n){ char p[64]; bin_path(p,name); sf_add(SF_BIN,p,d,n,0,0); }
static void sf_alias(const char* name, const char* target, const void* d, size_t n, int fl){
    char p[64]; bin_path(p,name); sf_add(SF_ALIAS,p,d,n,target,fl);
}
static void sf_conf(const char* path, const char* text){ sf_add(SF_CONF,path,text,sr_len(text),0,0); }

/* ---- default configuration (SF_CONF: created only when missing) ------------ */
#ifndef SECOS_NO_INIT
// The default /etc/inittab: network, supervised sshd, console bash, and the GUI
// hook (`startgui --auto` is a no-op unless /etc/gui-autostart, or the build
// default on /proc/cmdline when the file is absent, enables it). Format:
// docs/devlog/M40-init.md.
static const char g_default_inittab[] =
    "# /etc/inittab - SecOS init (PID 1). One entry per line:\n"
    "#   <id>:<action>:<command> [args...]\n"
    "# id       short name, used in messages and for /var/log/<id>.log\n"
    "# action   sysinit   run at boot in file order and wait for it\n"
    "#          respawn   own session, output to /var/log/<id>.log, restarted\n"
    "#                    with backoff (1 s doubling to 30 s) when it exits\n"
    "#          console   respawn on the console as session leader with the\n"
    "#                    console as controlling tty and foreground group\n"
    "#          once      start at boot, never restarted\n"
    "#          shutdown  run (and waited for) when the system goes down\n"
    "# The command is split on blanks and run directly (no shell, no quoting).\n"
    "net:sysinit:/bin/ifup -t 10\n"
    "sshd:respawn:/bin/dropbearmulti dropbear -F -R -p 22\n"
    "console:console:/bin/bash -i\n"
    "# GUI at boot: on when /etc/gui-autostart says 1 (0 = off); without the\n"
    "# file, the build default (make GUI_AUTOSTART=1) decides. docs/GUI.md.\n"
    "gui:once:/bin/startgui --auto\n";
#endif
#ifndef SECOS_NO_BASH
static const char g_bashrc[] =
    "# /etc/bash.bashrc - read by every interactive bash (SecOS)\n"
    "# bash sets TERM=dumb when it is unset: pick the console entry instead.\n"
    "case \"$TERM\" in \"\"|dumb) export TERM=linux ;; esac\n"
    "# The kernel starts bash with an empty environment: export a PATH and HOME\n"
    "# so programs it runs (env, BusyBox, lua os.getenv) see them too.\n"
    "export PATH=\"${PATH:-/bin:/usr/bin}\" HOME=\"${HOME:-/root}\"\n";
static const char g_inputrc[] =
    "# /etc/inputrc - GNU readline defaults (SecOS console)\n"
    "set enable-bracketed-paste off\n"
    "set bell-style none\n"
    "\"\\e[1~\": beginning-of-line\n"
    "\"\\e[4~\": end-of-line\n"
    "\"\\e[7~\": beginning-of-line\n"
    "\"\\e[8~\": end-of-line\n"
    "\"\\e[H\": beginning-of-line\n"
    "\"\\e[F\": end-of-line\n"
    "\"\\eOH\": beginning-of-line\n"
    "\"\\eOF\": end-of-line\n"
    "\"\\e[3~\": delete-char\n"
    "\"\\e[2~\": overwrite-mode\n"
    "\"\\e[1;5C\": forward-word\n"
    "\"\\e[1;5D\": backward-word\n";
#endif

static void build_table(void){
    g_nsf = 0;
    // [M31] coreutils: one signed multi-call program, dispatching on argv[0].
    static const char* const cu[] = {
        "echo","cat","wc","head","tail","rev","nl","yes","seq",
        "ls","mkdir","rm","touch","cp","basename","dirname","stat",
        "true","false","uname","sleep","clear","hexdump","uptime","help","tty", 0
    };
    sf_bin("coreutils", user_coreutils_elf, user_coreutils_elf_len);
    for(int i=0; cu[i]; i++) sf_alias(cu[i], "coreutils", user_coreutils_elf, user_coreutils_elf_len, 0);
    // [M32]/[M40] init (PID 1) and its demo service; regression-test programs.
    sf_bin("init", user_init_elf, user_init_elf_len);
    sf_bin("svc", user_svc_elf, user_svc_elf_len);
    sf_bin("sysfuzz", user_sysfuzz_elf, user_sysfuzz_elf_len);
    sf_bin("uaccess_test", user_uaccess_elf, user_uaccess_elf_len);
    sf_bin("abi_test", user_abi_test_elf, user_abi_test_elf_len);
    sf_bin("ttytest", user_ttytest_elf, user_ttytest_elf_len);
    sf_bin("ptytest", user_ptytest_elf, user_ptytest_elf_len);
    sf_alias("script", "ptytest", user_ptytest_elf, user_ptytest_elf_len, 0);
    sf_bin("kitest", user_kitest_elf, user_kitest_elf_len);
    sf_bin("pipe_test", user_pipe_test_elf, user_pipe_test_elf_len);
    // [M40] GUI kernel-interface programs and the compositor + its clients.
    sf_bin("fbdemo", user_fbdemo_elf, user_fbdemo_elf_len);
    sf_bin("guitest", user_guitest_elf, user_guitest_elf_len);
    sf_bin("guitest_nocap", user_guitest_nocap_elf, user_guitest_nocap_elf_len);
    sf_bin("loadkeys", user_loadkeys_elf, user_loadkeys_elf_len);
    sf_bin("secwm", user_secwm_elf, user_secwm_elf_len);
    sf_alias("startgui", "secwm", user_secwm_elf, user_secwm_elf_len, 0);
    sf_bin("sysinfo", user_sysinfo_elf, user_sysinfo_elf_len);
    sf_bin("swptest", user_swptest_elf, user_swptest_elf_len);
    sf_bin("sysmon", user_sysmon_elf, user_sysmon_elf_len);
#ifndef SECOS_NO_BUSYBOX
    // BusyBox 1.36.1 from source: replaces the coreutils applets of the same
    // name. On ramfs every applet is the signed re-exec stub (user/bbstub.c).
    sf_bin("busybox", user_busybox_elf, user_busybox_elf_len);
    for(int i=0; busybox_applets[i]; i++)
        sf_alias(busybox_applets[i], "busybox", user_bbstub_elf, user_bbstub_elf_len, 0);
    // Single-user account database for id/whoami/ls -l/ps/tar.
    sf_conf("/etc/passwd", "root:x:0:0:root:/root:/bin/bash\n");
    sf_conf("/etc/group", "root:x:0:\n");
#endif
#ifndef SECOS_NO_BASH
    sf_bin("bash", user_bash_elf, user_bash_elf_len);
    sf_conf("/etc/bash.bashrc", g_bashrc);
    sf_conf("/etc/inputrc", g_inputrc);
#endif
#ifndef SECOS_NO_LUA
    sf_bin("lua", user_lua_elf, user_lua_elf_len);
#endif
    sf_bin("poweroff", user_power_elf, user_power_elf_len);
    sf_alias("reboot", "poweroff", user_power_elf, user_power_elf_len, 0);
    sf_alias("halt", "poweroff", user_power_elf, user_power_elf_len, 0);
    sf_alias("shutdown", "poweroff", user_power_elf, user_power_elf_len, 0);
#ifndef SECOS_NO_DROPBEAR
    sf_bin("dropbearmulti", user_dropbear_elf, user_dropbear_elf_len);
    // The usual names, on a disk root only (ramfs copies would cost 1.5 MB each).
    sf_alias("dropbear", "dropbearmulti", 0, 0, SF_DISK_ONLY);
    sf_alias("dropbearkey", "dropbearmulti", 0, 0, SF_DISK_ONLY);
    sf_alias("dbclient", "dropbearmulti", 0, 0, SF_DISK_ONLY);
#endif
    // Interface configuration (SYS_NETCTL); its ifconfig replaces BusyBox's,
    // which needs SIOCGIF* ioctls SecOS does not have.
    sf_bin("netcfg", user_netcfg_elf, user_netcfg_elf_len);
    sf_alias("ifup", "netcfg", user_netcfg_elf, user_netcfg_elf_len, 0);
    sf_alias("ifdown", "netcfg", user_netcfg_elf, user_netcfg_elf_len, 0);
    sf_alias("ifconfig", "netcfg", user_netcfg_elf, user_netcfg_elf_len, 0);
#ifndef SECOS_NO_INIT
    sf_conf("/etc/inittab", g_default_inittab);
#endif
#if defined(SECOS_SYSROOT_TEST) && SECOS_SYSROOT_TEST == 2
    sf_bin("sysroot_probe", user_m16_child_elf, user_m16_child_elf_len);
#elif defined(SECOS_SYSROOT_TEST)
    sf_bin("sysroot_probe", user_hello_elf, user_hello_elf_len);
#endif
}

/* ---- content hashes (one per distinct embedded blob) ----------------------- */
static struct { const uint8_t* p; size_t n; char hex[65]; } g_hc[48];
static int g_nhc;
static const char* blob_hash(const uint8_t* p, size_t n){
    for(int i=0;i<g_nhc;i++) if(g_hc[i].p==p && g_hc[i].n==n) return g_hc[i].hex;
    uint8_t d[32]; sha256(p, n, d);
    static const char hx[] = "0123456789abcdef";
    int slot = g_nhc < 48 ? g_nhc++ : 47;
    g_hc[slot].p=p; g_hc[slot].n=n;
    for(int i=0;i<32;i++){ g_hc[slot].hex[2*i]=hx[d[i]>>4]; g_hc[slot].hex[2*i+1]=hx[d[i]&15]; }
    g_hc[slot].hex[64]=0;
    return g_hc[slot].hex;
}

/* ---- the manifest: /var/lib/secos/manifest ---------------------------------
 *   F <sha256> <size> <mtime> <path>     a system program as written
 *   L <target> <path>                    a symlink alias
 */
#define MANIFEST     "/var/lib/secos/manifest"
#define MANIFEST_NEW "/var/lib/secos/manifest.new"
#define MF_CAP 65536
static char g_mf_old[MF_CAP];
static char g_mf_new[MF_CAP];
static size_t g_mf_newlen;
typedef struct { char kind; const char* a; const char* path; uint64_t size, mtime; int seen; } mrec_t;
static mrec_t g_mr[SF_MAX + 64];
static int g_nmr;

static uint64_t sr_atou(const char** pp){
    const char* p=*pp; uint64_t v=0;
    while(*p>='0' && *p<='9'){ v=v*10+(uint64_t)(*p-'0'); p++; }
    *pp=p; return v;
}
/* Parse g_mf_old in place (fields NUL-terminated). Unknown lines are ignored. */
static void manifest_load(void){
    g_nmr=0; g_mf_old[0]=0;
    int n = vfs_read_all(MANIFEST, g_mf_old, MF_CAP-1);
    if(n<=0) return;
    g_mf_old[n]=0;
    char* p=g_mf_old;
    while(*p && g_nmr < (int)(sizeof(g_mr)/sizeof(g_mr[0]))){
        char* line=p; while(*p && *p!='\n') p++;
        if(*p) *p++=0;
        mrec_t r; r.kind=line[0]; r.seen=0; r.size=r.mtime=0;
        if((r.kind!='F' && r.kind!='L') || line[1]!=' ') continue;
        char* f=line+2; r.a=f; while(*f && *f!=' ') f++;
        if(!*f) continue;
        *f++=0;
        if(r.kind=='F'){
            const char* q=f; r.size=sr_atou(&q); if(*q!=' ') continue; q++;
            r.mtime=sr_atou(&q); if(*q!=' ') continue; q++;
            r.path=q;
        } else r.path=f;
        if(r.path[0]!='/') continue;
        g_mr[g_nmr++]=r;
    }
}
static mrec_t* manifest_find(const char* path){
    for(int i=0;i<g_nmr;i++) if(sr_eq(g_mr[i].path, path)) return &g_mr[i];
    return 0;
}
static void mf_put(const char* s){
    size_t n=sr_len(s);
    if(g_mf_newlen+n < MF_CAP){ for(size_t i=0;i<n;i++) g_mf_new[g_mf_newlen++]=s[i]; }
}
static void mf_putu(uint64_t v){
    char b[24]; int i=23; b[i]=0;
    do { b[--i]=(char)('0'+v%10); v/=10; } while(v && i>0);
    mf_put(&b[i]);
}

/* ---- file operations ------------------------------------------------------- */
static void ensure_dir(const char* p){ if(!vfs_lookup(p)) vfs_mkdir(p); }

static void set_mode(const char* path, uint32_t mode){
    vfs_attr_t a; a.mode=mode; a.uid=0; a.gid=0; a.atime=a.mtime=0;
    vfs_setattr(path, &a, VFS_ATTR_MODE);
}

/* Write a system program crash-safely: <dir>/.<name>.sysroot-new, then rename
 * over the old one (one metadata transaction: the old or the new program). */
static int install_file(const char* path, const uint8_t* data, size_t len){
    char tmp[80]; size_t ls=0;
    for(size_t i=0; path[i]; i++) if(path[i]=='/') ls=i;
    size_t k=0;
    for(; k<=ls && k<sizeof(tmp)-1; k++) tmp[k]=path[k];
    tmp[k]=0;
    sr_cat(tmp, ".", sizeof(tmp)); sr_cat(tmp, path+ls+1, sizeof(tmp)); sr_cat(tmp, ".sysroot-new", sizeof(tmp));
    if(vfs_lookup(tmp)) vfs_remove(tmp);
    if(vfs_create(tmp, data, len)!=0){ vfs_remove(tmp); return -1; }
    set_mode(tmp, 0755);
    vfs_inode_t* old = vfs_lookup(path);
    if(old && old->type==VFS_NODE_DIR){ vfs_remove(tmp); return -1; }
    if(vfs_rename(tmp, path)!=0){
        // Fallback for a filesystem without rename-over: remove, then rename.
        vfs_remove(path);
        if(vfs_rename(tmp, path)!=0){ vfs_remove(tmp); return -1; }
    }
    return 0;
}

/* Is `path` a symlink to `target`? */
static int is_link_to(const char* path, const char* target){
    vfs_inode_t* ino=vfs_lookup(path);
    if(!ino || ino->type!=VFS_NODE_SYMLINK) return 0;
    char t[80]; int n=vfs_readlink(path, t, sizeof(t));
    return n>0 && sr_eq(t, target);
}

/* Compare file content with a buffer, without reading more than needed. */
static uint8_t g_cmpbuf[4096];
static int file_equals(const char* path, const void* data, size_t len){
    vfs_inode_t* ino=vfs_lookup(path);
    if(!ino || ino->type!=VFS_NODE_FILE || ino->size!=len || !ino->ops || !ino->ops->read) return 0;
    const uint8_t* d=(const uint8_t*)data;
    for(size_t off=0; off<len; off+=sizeof(g_cmpbuf)){
        size_t n = len-off < sizeof(g_cmpbuf) ? len-off : sizeof(g_cmpbuf);
        if(ino->ops->read(ino, off, g_cmpbuf, n) != (int)n) return 0;
        for(size_t i=0;i<n;i++) if(g_cmpbuf[i]!=d[off+i]) return 0;
    }
    return 1;
}
/* Kernel-owned text file (release stamp): rewrite only when it differs. */
static int write_if_changed(const char* path, const char* text){
    size_t n=sr_len(text);
    if(file_equals(path, text, n)) return 0;
    if(vfs_lookup(path)) vfs_remove(path);
    return vfs_create(path, text, n)==0 ? 1 : -1;
}

/* ---- provisioning ---------------------------------------------------------- */
static struct { int written, kept, linked, removed, created, failed; } g_st;

static void provision_ramfs(void){
    for(int i=0;i<g_nsf;i++){
        sysfile_t* e=&g_sf[i];
        if(e->kind==SF_CONF){
            if(!vfs_lookup(e->path)){ if(vfs_create(e->path, e->data, e->len)==0) g_st.created++; else g_st.failed++; }
            continue;
        }
        if(e->kind==SF_ALIAS && (e->flags & SF_DISK_ONLY)) continue;
        if(vfs_lookup(e->path)) vfs_remove(e->path);
        if(vfs_create(e->path, e->data, e->len)==0) g_st.written++; else g_st.failed++;
    }
}

static void provision_disk(void){
    manifest_load();
    g_mf_newlen=0;
    mf_put("# SecOS system files installed from the kernel image (kernel/sysroot.c).\n"
           "# F <sha256> <size> <mtime> <path> | L <target> <path>. Do not edit.\n");
    for(int i=0;i<g_nsf;i++){
        sysfile_t* e=&g_sf[i];
        mrec_t* r=manifest_find(e->path);
        if(r) r->seen=1;
        if(e->kind==SF_CONF){
            if(!vfs_lookup(e->path)){ if(vfs_create(e->path, e->data, e->len)==0) g_st.created++; else g_st.failed++; }
            continue;
        }
        if(e->kind==SF_ALIAS){
            if(is_link_to(e->path, e->target)) g_st.kept++;
            else {
                vfs_inode_t* ino=vfs_lookup(e->path);
                if(ino && ino->type==VFS_NODE_DIR){ g_st.failed++; continue; }
                if(ino) vfs_remove(e->path);
                if(vfs_symlink(e->target, e->path)==0) g_st.linked++;
                else { g_st.failed++; continue; }
            }
            mf_put("L "); mf_put(e->target); mf_put(" "); mf_put(e->path); mf_put("\n");
            continue;
        }
        const char* h=blob_hash(e->data, e->len);
        vfs_inode_t* ino=vfs_lookup(e->path);
        int same = ino && ino->type==VFS_NODE_FILE && ino->size==e->len && r && r->kind=='F' &&
                   sr_eq(r->a, h) && r->size==e->len && r->mtime==ino->mtime;
        if(same) g_st.kept++;
        else if(install_file(e->path, e->data, e->len)==0){
            g_st.written++;
            debugcon_writestring("[SYSROOT] wrote "); debugcon_writestring(e->path); debugcon_writestring("\n");
        } else {
            g_st.failed++;
            debugcon_writestring("[SYSROOT] FAIL: could not write "); debugcon_writestring(e->path); debugcon_writestring("\n");
            continue;
        }
        ino=vfs_lookup(e->path);
        mf_put("F "); mf_put(h); mf_put(" "); mf_putu(e->len); mf_put(" ");
        mf_putu(ino ? ino->mtime : 0); mf_put(" "); mf_put(e->path); mf_put("\n");
    }
    // Programs an older kernel installed that this one no longer carries.
    for(int i=0;i<g_nmr;i++){
        mrec_t* r=&g_mr[i];
        if(r->seen) continue;
        vfs_inode_t* ino=vfs_lookup(r->path);
        if(!ino) continue;
        int ours = (r->kind=='L') ? is_link_to(r->path, r->a)
                                  : (ino->type==VFS_NODE_FILE && ino->size==r->size && ino->mtime==r->mtime);
        if(ours && vfs_remove(r->path)==0){
            g_st.removed++;
            debugcon_writestring("[SYSROOT] removed stale "); debugcon_writestring(r->path); debugcon_writestring("\n");
        } else {
            debugcon_writestring("[SYSROOT] kept "); debugcon_writestring(r->path);
            debugcon_writestring(" (no longer shipped, modified locally)\n");
        }
    }
    // Save the manifest only when it changed (crash-safe: new file + rename).
    int n = (int)g_mf_newlen;
    int same = 0;
    { static char cur[MF_CAP]; int m=vfs_read_all(MANIFEST, cur, MF_CAP);
      same = (m==n) && sr_eqn(cur, g_mf_new, (size_t)n); }
    if(!same){
        if(vfs_lookup(MANIFEST_NEW)) vfs_remove(MANIFEST_NEW);
        if(vfs_create(MANIFEST_NEW, g_mf_new, (size_t)n)!=0 || vfs_rename(MANIFEST_NEW, MANIFEST)!=0)
            debugcon_writestring("[SYSROOT] FAIL: could not save " MANIFEST "\n");
    }
}

/* Baked SSH keys -> /root/.ssh/authorized_keys: every baked line missing from
 * the file is appended; lines already there (and the user's own) stay. */
static char g_akbuf[65536];
static void install_authorized_keys(void){
    static const char keys[] = SECOS_AUTHORIZED_KEYS;
    const size_t klen = SECOS_AUTHORIZED_KEYS_LEN;
    if(!klen){
        debugcon_writestring("[SSH] no baked authorized_keys (build with SSH_AUTHORIZED_KEYS=...)\n");
        return;
    }
    const char* ak="/root/.ssh/authorized_keys";
    ensure_dir("/root"); ensure_dir("/root/.ssh");
    vfs_inode_t* ino=vfs_lookup(ak);
    int n=0;
    if(ino){
        if(ino->size >= sizeof(g_akbuf)){ debugcon_writestring("[SSH] FAIL: authorized_keys too large\n"); return; }
        n=vfs_read_all(ak, g_akbuf, sizeof(g_akbuf));
        if(n<0) n=0;
    }
    int added=0, ok=1;
    size_t i=0;
    while(i<klen){
        size_t s=i; while(i<klen && keys[i]!='\n') i++;
        size_t l=i-s; if(i<klen) i++;
        while(l && (keys[s+l-1]=='\r' || keys[s+l-1]==' ')) l--;
        if(l==0 || keys[s]=='#') continue;
        int found=0;                                     /* exact line already present? */
        for(int p=0; p<n && !found; ){
            int q=p; while(q<n && g_akbuf[q]!='\n') q++;
            int ll=q-p; while(ll && (g_akbuf[p+ll-1]=='\r' || g_akbuf[p+ll-1]==' ')) ll--;
            if((size_t)ll==l && sr_eqn(g_akbuf+p, keys+s, l)) found=1;
            p=q+1;
        }
        if(found) continue;
        char line[4096];
        if(l+2 > sizeof(line)){ ok=0; continue; }
        size_t o=0;
        if(n>0 && g_akbuf[n-1]!='\n') line[o++]='\n';     /* finish the user's last line */
        for(size_t k=0;k<l;k++) line[o++]=keys[s+k];
        line[o++]='\n';
        int r = vfs_lookup(ak) ? vfs_write(ak, (size_t)n, line, o) : (vfs_create(ak, line, o)==0 ? (int)o : -1);
        if(r<0){ ok=0; break; }
        if(n+(int)o < (int)sizeof(g_akbuf)){ for(size_t k=0;k<o;k++) g_akbuf[n+k]=line[k]; }
        n+=(int)o; added++;
    }
    // Dropbear refuses keys under a home it does not trust: /root and ~/.ssh
    // owned by root and not group/world-writable (a root staged with
    // `mkfs.ext4 -d` carries the build user's uid). Only fix what is wrong.
    static const struct { const char* p; uint32_t mode; } perm[] = {
        { "/root", 0700 }, { "/root/.ssh", 0700 }, { "/root/.ssh/authorized_keys", 0600 } };
    for(unsigned k=0;k<3;k++){
        vfs_inode_t* x=vfs_lookup(perm[k].p);
        if(!x || ((x->mode & 07777)==perm[k].mode && x->uid==0 && x->gid==0)) continue;
        vfs_attr_t a; a.mode=perm[k].mode; a.uid=0; a.gid=0; a.atime=a.mtime=0;
        vfs_setattr(perm[k].p, &a, VFS_ATTR_MODE | VFS_ATTR_UID | VFS_ATTR_GID);
    }
    if(!ok) debugcon_writestring("[SSH] FAIL: could not install the baked authorized_keys\n");
    else {
        debugcon_writestring("[SSH] baked authorized_keys installed to /root/.ssh (added=");
        dbg_dec((uint64_t)added); debugcon_writestring(")\n");
    }
}

static int present(const char* p){ return vfs_lookup(p)!=0; }

/* The per-component markers the selftests assert on (same text as before). */
static void report_components(void){
    int cu=0; for(int i=0;i<g_nsf;i++) if(g_sf[i].target && sr_eq(g_sf[i].target,"coreutils") && present(g_sf[i].path)) cu++;
    if(present("/bin/coreutils")){
        debugcon_writestring("[M31] coreutils installed to /bin (applets="); debugcon_print_hex((uint64_t)cu+1);
        debugcon_writestring(")\n");
    }
    if(present("/bin/init") && present("/bin/svc")) debugcon_writestring("[M32] init + svc installed to /bin\n");
    else debugcon_writestring("[INIT] FAIL: could not install /bin/init\n");
    if(present("/bin/fbdemo") && present("/bin/guitest") && present("/bin/loadkeys"))
        debugcon_writestring("[M40] fbdemo + guitest + loadkeys installed to /bin\n");
    if(present("/bin/secwm") && present("/bin/startgui") && present("/bin/sysmon"))
        debugcon_writestring("[GUI] secwm + startgui + sysmon + sysinfo + swptest installed to /bin\n");
#ifndef SECOS_NO_BUSYBOX
    { int bb=0; for(int i=0;i<g_nsf;i++) if(g_sf[i].target && sr_eq(g_sf[i].target,"busybox") && present(g_sf[i].path)) bb++;
      if(present("/bin/busybox")){
          debugcon_writestring("[BB] installed BusyBox to /bin/busybox (applets="); debugcon_print_hex((uint64_t)bb);
          debugcon_writestring(")\n");
      } else debugcon_writestring("[BB] FAIL: could not install /bin/busybox\n"); }
#endif
#ifndef SECOS_NO_BASH
    debugcon_writestring(present("/bin/bash") ? "[M39] installed GNU bash to /bin/bash\n"
                                              : "[M39] FAIL: could not install /bin/bash\n");
#endif
    debugcon_writestring(present("/bin/poweroff") && present("/bin/reboot") && present("/bin/halt") && present("/bin/shutdown")
                         ? "[POWER] poweroff/reboot/halt/shutdown installed to /bin\n"
                         : "[POWER] FAIL: could not install the power commands\n");
#ifndef SECOS_NO_LUA
    debugcon_writestring(present("/bin/lua") ? "[M34] installed lua to /bin/lua\n"
                                             : "[M34] FAIL: could not install /bin/lua\n");
#endif
#ifndef SECOS_NO_DROPBEAR
    debugcon_writestring(present("/bin/dropbearmulti") ? "[SSH] installed dropbear to /bin/dropbearmulti\n"
                                                       : "[SSH] FAIL: could not install /bin/dropbearmulti\n");
#endif
    debugcon_writestring(present("/bin/netcfg") && present("/bin/ifup") && present("/bin/ifdown") && present("/bin/ifconfig")
                         ? "[NET] netcfg/ifup/ifdown/ifconfig installed to /bin\n"
                         : "[NET] FAIL: could not install netcfg\n");
}

void sysroot_provision(void){
    uint64_t t0=timer_get_ticks();
    g_st.written=g_st.kept=g_st.linked=g_st.removed=g_st.created=g_st.failed=0;
    static const char* const dirs[] = { "/bin","/etc","/var","/var/log","/var/lib","/var/lib/secos",
                                        "/root","/home","/tmp","/etc/dropbear", 0 };
    for(int i=0; dirs[i]; i++) ensure_dir(dirs[i]);
    build_table();
    if(g_persistent) provision_disk(); else provision_ramfs();
    // Release stamp of the running kernel (kernel-owned).
    write_if_changed("/etc/secos-release",
        "NAME=SecOS\nVERSION=0.1.0-dev\nBUILD_TS=" BUILD_TS "\nGIT_HASH=" GIT_HASH "\n");
    if(g_persistent) write_if_changed("/VERSION", "VERSION=0.1.0-dev\nBUILD_TS=" BUILD_TS "\nGIT_HASH=" GIT_HASH "\n");
    install_authorized_keys();
#if !defined(SECOS_NO_INIT) && defined(SECOS_INIT_TAMPER_TEST)
    // Selftest hook: one flipped code byte, so the signature check refuses
    // /bin/init and the boot must fall back to bash.
    { uint8_t b = user_init_elf[0x1000] ^ 0x01;
      vfs_write("/bin/init", 0x1000, &b, 1);
      debugcon_writestring("[INIT] TEST: /bin/init tampered\n"); }
#endif
    report_components();
    uint64_t dt=timer_get_ticks()-t0;
    debugcon_writestring(g_persistent ? "[SYSROOT] persistent root: written=" : "[SYSROOT] ramfs root: written=");
    dbg_dec((uint64_t)g_st.written); debugcon_writestring(" unchanged="); dbg_dec((uint64_t)g_st.kept);
    debugcon_writestring(" linked="); dbg_dec((uint64_t)g_st.linked);
    debugcon_writestring(" stale_removed="); dbg_dec((uint64_t)g_st.removed);
    debugcon_writestring(" configs_created="); dbg_dec((uint64_t)g_st.created);
    debugcon_writestring(" failed="); dbg_dec((uint64_t)g_st.failed);
    debugcon_writestring(" ms="); dbg_dec(dt); debugcon_writestring("\n");
    if(g_persistent && (g_st.written || g_st.linked || g_st.removed)){
        terminal_writestring("[SYSROOT] system files updated from the kernel image: ");
        con_dec((uint64_t)(g_st.written+g_st.linked)); terminal_writestring(" written, ");
        con_dec((uint64_t)g_st.removed); terminal_writestring(" removed\n");
    }
}

/* /etc/vconsole.conf: KEYMAP=<layout> (written by /bin/loadkeys). */
void sysroot_apply_settings(void){
    static char buf[1024];
    int n=vfs_read_all("/etc/vconsole.conf", buf, sizeof(buf)-1);
    if(n<0){
        debugcon_writestring("[KEYMAP] no /etc/vconsole.conf: build default layout\n");
        return;
    }
    buf[n]=0;
    for(char* p=buf; *p; ){
        char* line=p; while(*p && *p!='\n') p++;
        if(*p) *p++=0;
        while(*line==' ' || *line=='\t') line++;
        if(!sr_eqn(line, "KEYMAP=", 7)) continue;
        char* v=line+7; if(*v=='"') v++;
        char name[16]; size_t k=0;
        while(v[k] && v[k]!='"' && v[k]!=' ' && v[k]!='\r' && k<sizeof(name)-1){ name[k]=v[k]; k++; }
        name[k]=0;
        int r=keyboard_set_keymap(name);
        debugcon_writestring("[KEYMAP] /etc/vconsole.conf: KEYMAP="); debugcon_writestring(name);
        debugcon_writestring(r==0 ? " (applied)\n" : " (unknown layout, ignored)\n");
        return;
    }
    debugcon_writestring("[KEYMAP] /etc/vconsole.conf has no KEYMAP=: build default layout\n");
}
