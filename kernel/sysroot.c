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
 *   /root/.ssh/authorized_keys when missing (user-added lines are kept);
 *   the standard system groups are appended to /etc/group when missing.
 *   The contents of /root, /home, /var, /tmp and /etc/dropbear (the SSH host
 *   key) are never touched.
 *
 *   Ownership and modes (M42 enforces them; exec needs an x bit even for root):
 *   every table entry carries its mode/uid/gid (programs root:root 0755,
 *   configuration 0644, /etc/shadow 0600), applied when the file is written.
 *   Programs and the system directories (g_dirs) are also corrected at every
 *   boot when they are wrong, e.g. on a disk made before the modes existed.
 *   Configuration files keep whatever mode they have once created.
 */
#include <stdint.h>
#include <stddef.h>
#include "debugcon.h"
#include "terminal.h"
#include "../fs/vfs.h"
#include "../crypto/sha256.h"
#include "../drivers/keyboard.h"
#include "sysroot.h"
#include "klock.h"

extern uint64_t timer_get_ticks(void);
extern int vfs_read_all(const char*, void*, size_t);

/* ---- embedded signed programs ------------------------------------------- */
#include "../crypto/user_coreutils_elf.h"
#include "../crypto/user_init_elf.h"
#include "../crypto/user_svc_elf.h"
#include "../crypto/user_sysfuzz_elf.h"
#include "../crypto/user_smptest_elf.h"   // [M48] SMP regression checks
#include "../crypto/user_pcstress_elf.h"  // [M50] page-cache coherence stress
#include "../crypto/user_fsstress_elf.h"  // [M51] generic filesystem stress
#include "../crypto/user_uaccess_elf.h"
#include "../crypto/user_abi_test_elf.h"
#include "../crypto/user_ttytest_elf.h"
#include "../crypto/user_ptytest_elf.h"
#include "../crypto/user_kitest_elf.h"
#include "../crypto/user_pipe_test_elf.h"
#include "../crypto/user_netstress_elf.h"
#include "../crypto/user_fbdemo_elf.h"
#include "../crypto/user_guitest_elf.h"
#include "../crypto/user_guitest_nocap_elf.h"
#include "../crypto/user_loadkeys_elf.h"
#include "../crypto/user_spkg_elf.h"     // [U6] package installer v2
#include "../crypto/user_secwm_elf.h"
#include "../crypto/user_sysinfo_elf.h"
#include "../crypto/user_swptest_elf.h"
#include "../crypto/user_sysmon_elf.h"   // LVGL 9.2 system monitor (make port-lvgl)
// [M57] Driver Space v2 test drivers (edu device): positive + negative tests.
#include "../crypto/user_udrv_edu_elf.h"
#include "../crypto/user_udrv_negtest_elf.h"
#include "../crypto/user_udrv_user_elf.h"
#include "../crypto/user_taskmgr_elf.h"  // [U5] LVGL task manager (make port-lvgl)
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
#ifndef SECOS_NO_NANO
// [U8] GNU nano on musl + ncursesw (tools/port-ncurses.sh, tools/port-nano.sh):
// /usr/bin/nano, its syntax files and a small compiled terminfo database.
#include "../crypto/user_nano_elf.h"
#include "nano_syntax.h"
#include "terminfo_db.h"
#endif
#include "../crypto/user_power_elf.h"    // /bin/{poweroff,reboot,halt,shutdown}
#ifndef SECOS_NO_DROPBEAR
#include "../crypto/user_dropbear_elf.h"
#endif
#include "../crypto/user_netcfg_elf.h"
#include "../build/authorized_keys.h"
// [M43] Multi-user tools: login/su/sudo/passwd (SETID manifest, root 04755),
// usertools (useradd & co., id, whoami, groups).
#include "../crypto/user_login_elf.h"
#include "../crypto/user_su_elf.h"
#include "../crypto/user_sudo_elf.h"
#include "../crypto/user_passwd_elf.h"
#include "../crypto/user_usertools_elf.h"
// make ROOT_PASSWORD=... / ROOT_PASSWORD_HASH=...: SECOS_ROOT_PWHASH is the
// SHA-512 crypt hash computed on the build host ("" when not set).
#include "../build/root_pwhash.h"
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

/* ---- locking [M51-C6] ---------------------------------------------------------
 * Everything below (the table g_sf, the blob-hash cache, the manifest buffers
 * g_mf_* / g_mr, g_cmpbuf, g_akbuf, the stats g_st and the function-local
 * static buffers) is scratch state of one provisioning run. sysroot_lock
 * (K11) makes each exported entry one critical section: provision,
 * apply_settings and install_exec never interleave. Inside it only VFS (K20/
 * K30 and below), the keyboard lock (S50) and the heap are taken; no user
 * memory, no block, no spawn. The entries do not call one another.
 * g_persistent / g_rootdev are written once by sysroot_set_root at boot,
 * before the first user process exists, and only read afterwards (procfs). */
static klock_t sysroot_lock = KLOCK_INIT(LK_K11_SYSROOT, "sysroot");

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
    uint16_t       mode;     /* permission bits (07777) of the installed file */
    uint32_t       uid, gid; /* its owner */
} sysfile_t;
#define SF_MAX 320
static sysfile_t g_sf[SF_MAX];
static int g_nsf;

/* Add or replace (same path: the later entry wins, e.g. BusyBox over the
 * coreutils applet of the same name, netcfg's ifconfig over BusyBox's). */
static void sf_add(int kind, const char* path, const void* data, size_t len, const char* target, int flags,
                   uint16_t mode){
    sysfile_t* e = 0;
    for(int i=0;i<g_nsf;i++) if(sr_eq(g_sf[i].path, path)){ e=&g_sf[i]; break; }
    if(!e){
        if(g_nsf>=SF_MAX){ debugcon_writestring("[SYSROOT] table full\n"); return; }
        e=&g_sf[g_nsf++];
    }
    sr_cpy(e->path, path, sizeof(e->path));
    e->data=(const uint8_t*)data; e->len=len; e->target=target; e->kind=(uint8_t)kind; e->flags=(uint8_t)flags;
    e->mode=mode; e->uid=0; e->gid=0;        /* every system file is root:root */
}
static void bin_path(char* out, const char* name){ sr_cpy(out, "/bin/", 64); sr_cat(out, name, 64); }
static void sf_bin(const char* name, const void* d, size_t n){ char p[64]; bin_path(p,name); sf_add(SF_BIN,p,d,n,0,0,0755); }
/* [M43] A set-user-id root program. The bit takes effect only because the
 * program's signed manifest carries MANIFEST_FLAG_SETID (kernel/cred.c). */
static void sf_bin_setuid(const char* name, const void* d, size_t n){ char p[64]; bin_path(p,name); sf_add(SF_BIN,p,d,n,0,0,04755); }
/* An alias is a symlink on disk (its mode is the target's) and a copy of the
 * program on ramfs, which gets the program's 0755. */
static void sf_alias(const char* name, const char* target, const void* d, size_t n, int fl){
    char p[64]; bin_path(p,name); sf_add(SF_ALIAS,p,d,n,target,fl,0755);
}
static void sf_conf_mode(const char* path, const char* text, uint16_t mode){ sf_add(SF_CONF,path,text,sr_len(text),0,0,mode); }
static void sf_conf(const char* path, const char* text){ sf_conf_mode(path, text, 0644); }
/* [U8] A system data file (not a program): kernel-owned like a program
 * (rewritten when it changes, removed when no longer shipped), mode 0644. */
static void sf_data(const char* path, const void* d, size_t n){ sf_add(SF_BIN,path,d,n,0,0,0644); }

/* System directories: created when missing (SD_CREATE) and given this owner
 * and mode at every boot when they differ. Only the directories themselves:
 * what users keep inside /root, /home, /var and /tmp is never changed. The
 * virtual filesystems (/dev, /proc, /sys) set their own modes. */
#define SD_CREATE 0x1
static const struct { const char* p; uint16_t mode; uint8_t flags; } g_dirs[] = {
    { "/",              0755,  0 },
    { "/bin",           0755,  SD_CREATE },
    { "/etc",           0755,  SD_CREATE },
    { "/etc/dropbear",  0700,  SD_CREATE },   /* the SSH host key */
    { "/etc/skel",      0755,  SD_CREATE },   /* copied into new homes (useradd) */
    { "/usr",           0755,  SD_CREATE },
    { "/usr/bin",       0755,  SD_CREATE },
    { "/usr/share",     0755,  SD_CREATE },
    { "/usr/local",     0755,  0 },
    { "/usr/local/bin", 0755,  0 },
    { "/lib",           0755,  0 },
    { "/opt",           0755,  0 },
    { "/mnt",           0755,  0 },
    { "/var",           0755,  SD_CREATE },
    { "/var/log",       0755,  SD_CREATE },
    { "/var/lib",       0755,  SD_CREATE },
    { "/var/lib/secos", 0755,  SD_CREATE },
    { "/var/tmp",       01777, SD_CREATE },
    { "/root",          0700,  SD_CREATE },
    { "/home",          0755,  SD_CREATE },
    { "/tmp",           01777, SD_CREATE },
};

/* Standard system groups (Debian gids, the ones kernel/cred.h, devfs, pty and
 * the input nodes use): the default /etc/group, and appended to an existing
 * one when neither the name nor the gid is there yet. */
static const struct { const char* name; const char* line; } g_groups[] = {
    { "root",  "root:x:0:\n" },
    { "tty",   "tty:x:5:\n" },
    { "disk",  "disk:x:6:\n" },
    { "wheel", "wheel:x:10:root\n" },
    { "video", "video:x:44:\n" },
    { "users", "users:x:100:\n" },
    { "input", "input:x:105:\n" },
};
static const char g_default_passwd[] = "root:x:0:0:root:/root:/bin/bash\n";

/* ---- default configuration (SF_CONF: created only when missing) ------------ */
#ifndef SECOS_NO_INIT
// The default /etc/inittab: network, supervised sshd, console login (or root bash), and the GUI
// hook (`startgui --auto` is a no-op unless /etc/gui-autostart, or the build
// default on /proc/cmdline when the file is absent, enables it). Format:
// docs/devlog/M40-init.md.
#define INITTAB_TEXT(console_line) \
    "# /etc/inittab - SecOS init (PID 1). One entry per line:\n" \
    "#   <id>:<action>:<command> [args...]\n" \
    "# id       short name, used in messages and for /var/log/<id>.log\n" \
    "# action   sysinit   run at boot in file order and wait for it\n" \
    "#          respawn   own session, output to /var/log/<id>.log, restarted\n" \
    "#                    with backoff (1 s doubling to 30 s) when it exits\n" \
    "#          console   respawn on the console as session leader with the\n" \
    "#                    console as controlling tty and foreground group\n" \
    "#          once      start at boot, never restarted\n" \
    "#          shutdown  run (and waited for) when the system goes down\n" \
    "# The command is split on blanks and run directly (no shell, no quoting).\n" \
    "net:sysinit:/bin/ifup -t 10\n" \
    "sshd:respawn:/bin/dropbearmulti dropbear -F -R -p 22\n" \
    console_line \
    "# GUI at boot: on when /etc/gui-autostart says 1 (0 = off); without the\n" \
    "# file, the build default (make GUI_AUTOSTART=1) decides. docs/GUI.md.\n" \
    "gui:once:/bin/startgui --auto\n"
// [M43] The console runs /bin/login (getty-style, docs/USERS.md); a build with
// CONSOLE=root (-DSECOS_CONSOLE_ROOT, what the selftest uses) keeps the root
// bash of before. The text before M43 is exactly the root variant.
static const char g_inittab_login[] = INITTAB_TEXT("console:console:/bin/login\n");
static const char g_inittab_root[]  = INITTAB_TEXT("console:console:/bin/bash -i\n");
#ifdef SECOS_CONSOLE_ROOT
#define g_default_inittab g_inittab_root
#else
#define g_default_inittab g_inittab_login
#endif
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

// [M43] Login shells (login, su -, sudo -i) read /etc/profile; new homes get
// /etc/skel. Created only when missing, like every SF_CONF.
static const char g_profile[] =
    "# /etc/profile - read by login shells (SecOS)\n"
    "export PATH=\"${PATH:-/usr/local/bin:/usr/bin:/bin}\"\n"
    "umask 022\n"
    "# Interactive bash: the system-wide settings too.\n"
    "if [ -n \"$BASH_VERSION\" ] && [ -n \"$PS1\" ] && [ -f /etc/bash.bashrc ]; then\n"
    "    . /etc/bash.bashrc\n"
    "fi\n";
static const char g_skel_profile[] =
    "# ~/.profile - read by login shells\n"
    "if [ -n \"$BASH_VERSION\" ] && [ -f \"$HOME/.bashrc\" ]; then\n"
    "    . \"$HOME/.bashrc\"\n"
    "fi\n";
static const char g_skel_bashrc[] =
    "# ~/.bashrc - read by interactive bash\n"
    "PS1='\\u@\\h:\\w\\$ '\n";

static const char* default_group(void){
    static char buf[256];
    buf[0]=0;
    for(unsigned i=0;i<sizeof(g_groups)/sizeof(g_groups[0]);i++) sr_cat(buf, g_groups[i].line, sizeof(buf));
    return buf;
}
/* /etc/shadow for the accounts in /etc/passwd (the one on disk when there is
 * one, else the default), each with a locked password. Used only when
 * /etc/shadow is missing. */
static const char* default_shadow(void){
    static char pw[4096], buf[4096];
    int n=vfs_read_all("/etc/passwd", pw, sizeof(pw)-1);
    if(n<0){ n=(int)sr_len(g_default_passwd); sr_cpy(pw, g_default_passwd, sizeof(pw)); }
    pw[n]=0;
    buf[0]=0;
    for(char* p=pw; *p; ){
        char* line=p; while(*p && *p!='\n') p++;
        if(*p) *p++=0;
        char* c=line; while(*c && *c!=':') c++;
        if(*c!=':' || c==line || line[0]=='#') continue;
        *c=0;
        if(sr_len(buf)+sr_len(line)+24 >= sizeof(buf)) break;
        sr_cat(buf, line, sizeof(buf)); sr_cat(buf, ":!::0:99999:7:::\n", sizeof(buf));
    }
    return buf;
}

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
    sf_bin("smptest", user_smptest_elf, user_smptest_elf_len);
    sf_bin("pcstress", user_pcstress_elf, user_pcstress_elf_len);
    sf_bin("fsstress", user_fsstress_elf, user_fsstress_elf_len);
    sf_bin("uaccess_test", user_uaccess_elf, user_uaccess_elf_len);
    sf_bin("abi_test", user_abi_test_elf, user_abi_test_elf_len);
    sf_bin("ttytest", user_ttytest_elf, user_ttytest_elf_len);
    sf_bin("ptytest", user_ptytest_elf, user_ptytest_elf_len);
    sf_alias("script", "ptytest", user_ptytest_elf, user_ptytest_elf_len, 0);
    sf_bin("kitest", user_kitest_elf, user_kitest_elf_len);
    sf_bin("pipe_test", user_pipe_test_elf, user_pipe_test_elf_len);
    sf_bin("netstress", user_netstress_elf, user_netstress_elf_len);   // [M47]
    // [M40] GUI kernel-interface programs and the compositor + its clients.
    sf_bin("fbdemo", user_fbdemo_elf, user_fbdemo_elf_len);
    sf_bin("guitest", user_guitest_elf, user_guitest_elf_len);
    sf_bin("guitest_nocap", user_guitest_nocap_elf, user_guitest_nocap_elf_len);
    sf_bin("loadkeys", user_loadkeys_elf, user_loadkeys_elf_len);
    sf_bin("spkg", user_spkg_elf, user_spkg_elf_len);                 // [U6]
    sf_bin("secwm", user_secwm_elf, user_secwm_elf_len);
    sf_alias("startgui", "secwm", user_secwm_elf, user_secwm_elf_len, 0);
    sf_bin("sysinfo", user_sysinfo_elf, user_sysinfo_elf_len);
    sf_bin("swptest", user_swptest_elf, user_swptest_elf_len);
    sf_bin("sysmon", user_sysmon_elf, user_sysmon_elf_len);
    // [M57] ring-3 driver programs (v3 DRIVER manifest; udrv_user: plain manifest).
    sf_bin("udrv_edu", user_udrv_edu_elf, user_udrv_edu_elf_len);
    sf_bin("udrv_negtest", user_udrv_negtest_elf, user_udrv_negtest_elf_len);
    sf_bin("udrv_user", user_udrv_user_elf, user_udrv_user_elf_len);
    sf_bin("taskmgr", user_taskmgr_elf, user_taskmgr_elf_len);   // [U5]
#ifndef SECOS_NO_BUSYBOX
    // BusyBox 1.36.1 from source: replaces the coreutils applets of the same
    // name. On ramfs every applet is the signed re-exec stub (user/bbstub.c).
    sf_bin("busybox", user_busybox_elf, user_busybox_elf_len);
    for(int i=0; busybox_applets[i]; i++)
        sf_alias(busybox_applets[i], "busybox", user_bbstub_elf, user_bbstub_elf_len, 0);
#endif
    // Account database (id/whoami/ls -l/ps/tar, login, sshd). The shadow file
    // locks every password ('!'): root logs in by SSH key or on the console.
    sf_conf("/etc/passwd", g_default_passwd);
    sf_conf("/etc/group", default_group());
    sf_conf_mode("/etc/shadow", default_shadow(), 0600);
#ifndef SECOS_NO_BASH
    sf_bin("bash", user_bash_elf, user_bash_elf_len);
    sf_conf("/etc/bash.bashrc", g_bashrc);
    sf_conf("/etc/inputrc", g_inputrc);
#endif
#ifndef SECOS_NO_LUA
    sf_bin("lua", user_lua_elf, user_lua_elf_len);
#endif
#ifndef SECOS_NO_NANO
    sf_add(SF_BIN, "/usr/bin/nano", user_nano_elf, user_nano_elf_len, 0, 0, 0755);
    for(int i=0; nano_syntax[i].name; i++){
        char p[64]; sr_cpy(p, "/usr/share/nano/", sizeof(p)); sr_cat(p, nano_syntax[i].name, sizeof(p));
        sf_data(p, nano_syntax[i].text, sr_len(nano_syntax[i].text));
    }
    for(int i=0; terminfo_db[i].path; i++){
        char p[64]; sr_cpy(p, "/usr/share/terminfo/", sizeof(p)); sr_cat(p, terminfo_db[i].path, sizeof(p));
        sf_data(p, terminfo_db[i].data, terminfo_db[i].len);
    }
    sf_conf("/etc/nanorc", nano_nanorc);   // user/nano/nanorc, via nano_syntax.h
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
    // [M43] Accounts. login/su/sudo/passwd are the only set-uid programs.
    // usertools comes after BusyBox so its id/whoami win (they read the real
    // ids; BusyBox's applets of the same name are replaced).
    sf_bin_setuid("login", user_login_elf, user_login_elf_len);
    sf_bin_setuid("su", user_su_elf, user_su_elf_len);
    sf_bin_setuid("sudo", user_sudo_elf, user_sudo_elf_len);
    sf_bin_setuid("passwd", user_passwd_elf, user_passwd_elf_len);
    sf_bin("usertools", user_usertools_elf, user_usertools_elf_len);
    { static const char* const ut[] = { "useradd","userdel","usermod","groupadd","groupdel","chpasswd",
                                        "id","whoami","groups", 0 };
      for(int i=0; ut[i]; i++) sf_alias(ut[i], "usertools", user_usertools_elf, user_usertools_elf_len, 0); }
    sf_conf("/etc/profile", g_profile);
    sf_conf("/etc/skel/.profile", g_skel_profile);
    sf_conf("/etc/skel/.bashrc", g_skel_bashrc);
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
static struct { const uint8_t* p; size_t n; char hex[65]; } g_hc[128];
static int g_nhc;
static const char* blob_hash(const uint8_t* p, size_t n){
    for(int i=0;i<g_nhc;i++) if(g_hc[i].p==p && g_hc[i].n==n) return g_hc[i].hex;
    uint8_t d[32]; sha256(p, n, d);
    static const char hx[] = "0123456789abcdef";
    int slot = g_nhc < 128 ? g_nhc++ : 127;
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

/* Give `path` this owner and mode unless it already has them (no write, so
 * no journal transaction, when nothing is wrong). A symlink is left alone: its
 * own mode means nothing and chmod would follow it. 1 = changed, 0 = already
 * right or absent, -1 = failed. */
static int fix_attr(const char* path, uint32_t mode, uint32_t uid, uint32_t gid){
    vfs_inode_t* x=vfs_lookup(path);
    if(!x || x->type==VFS_NODE_SYMLINK) return 0;
    if((x->mode & 07777u)==mode && (x->mode & 0170000u) && x->uid==uid && x->gid==gid) return 0;
    vfs_attr_t a; a.mode=mode; a.uid=uid; a.gid=gid; a.atime=a.mtime=0;
    return vfs_lsetattr(path, &a, VFS_ATTR_MODE | VFS_ATTR_UID | VFS_ATTR_GID)==0 ? 1 : -1;
}

/* Write a system program crash-safely: <dir>/.<name>.sysroot-new, then rename
 * over the old one (one metadata transaction: the old or the new program). */
static int install_file(const char* path, const uint8_t* data, size_t len, uint32_t mode){
    char tmp[80]; size_t ls=0;
    for(size_t i=0; path[i]; i++) if(path[i]=='/') ls=i;
    size_t k=0;
    for(; k<=ls && k<sizeof(tmp)-1; k++) tmp[k]=path[k];
    tmp[k]=0;
    sr_cat(tmp, ".", sizeof(tmp)); sr_cat(tmp, path+ls+1, sizeof(tmp)); sr_cat(tmp, ".sysroot-new", sizeof(tmp));
    if(vfs_lookup(tmp)) vfs_remove(tmp);
    if(vfs_create(tmp, data, len)!=0){ vfs_remove(tmp); return -1; }
    fix_attr(tmp, mode, 0, 0);
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
static struct { int written, kept, linked, removed, created, failed, fixed; } g_st;

/* Configuration: created (with its mode) only when missing, then the user's. */
static void install_conf(const sysfile_t* e){
    if(vfs_lookup(e->path)) return;
    if(vfs_create(e->path, e->data, e->len)==0){ g_st.created++; fix_attr(e->path, e->mode, e->uid, e->gid); }
    else g_st.failed++;
}
static void count_fix(int r){ if(r>0) g_st.fixed++; else if(r<0) g_st.failed++; }

static void provision_ramfs(void){
    for(int i=0;i<g_nsf;i++){
        sysfile_t* e=&g_sf[i];
        if(e->kind==SF_CONF){ install_conf(e); continue; }
        if(e->kind==SF_ALIAS && (e->flags & SF_DISK_ONLY)) continue;
        if(vfs_lookup(e->path)) vfs_remove(e->path);
        if(vfs_create(e->path, e->data, e->len)==0){ g_st.written++; fix_attr(e->path, e->mode, e->uid, e->gid); }
        else g_st.failed++;
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
        if(e->kind==SF_CONF){ install_conf(e); continue; }
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
        if(same){
            g_st.kept++;
            count_fix(fix_attr(e->path, e->mode, e->uid, e->gid));   /* e.g. a 0644 program from an older disk */
        } else if(install_file(e->path, e->data, e->len, e->mode)==0){
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

/* Append each standard group missing from /etc/group (by name and by gid);
 * the existing lines, and any group the user added, stay as they are. */
static void ensure_groups(void){
    static char buf[8192];
    int n=vfs_read_all("/etc/group", buf, sizeof(buf)-1);
    if(n<0) return;
    buf[n]=0;
    for(unsigned g=0; g<sizeof(g_groups)/sizeof(g_groups[0]); g++){
        const char* want=g_groups[g].line;
        size_t nl=sr_len(g_groups[g].name);
        const char* wgid=want+nl+3;                      /* "<name>:x:<gid>:" */
        size_t gl=0; while(wgid[gl] && wgid[gl]!=':') gl++;
        int found=0;
        for(int p=0; p<n && !found; ){
            int q=p; while(q<n && buf[q]!='\n') q++;
            const char* l=buf+p; int ll=q-p;
            if(ll>(int)nl && sr_eqn(l, g_groups[g].name, nl) && l[nl]==':') found=1;
            int f=0, s=0;                                /* third field = gid */
            for(int k=0;k<ll && f<2;k++) if(l[k]==':' && ++f==2) s=k+1;
            if(!found && f==2){
                int e=s; while(e<ll && l[e]!=':') e++;
                if((size_t)(e-s)==gl && sr_eqn(l+s, wgid, gl)) found=1;
            }
            p=q+1;
        }
        if(found) continue;
        char line[64]; size_t o=0;
        if(n>0 && buf[n-1]!='\n') line[o++]='\n';
        for(size_t k=0; want[k] && o<sizeof(line)-1; k++) line[o++]=want[k];
        if(vfs_write("/etc/group", (size_t)n, line, o)!=(int)o){ g_st.failed++; return; }
        if(n+(int)o < (int)sizeof(buf)-1){ for(size_t k=0;k<o;k++) buf[n+k]=line[k]; n+=(int)o; buf[n]=0; }
        debugcon_writestring("[SYSROOT] added group "); debugcon_writestring(g_groups[g].name);
        debugcon_writestring(" to /etc/group\n");
    }
}

/* [M43] /etc/inittab is the administrator's once it exists, except while it is
 * still one of the kernel's own default texts, unmodified: then it follows the
 * console policy of the running build (login, or root bash with CONSOLE=root),
 * so switching the build switches an untouched system too. */
static void update_default_inittab(void){
#ifndef SECOS_NO_INIT
    const char* want = g_default_inittab;
    const char* other = (want == g_inittab_login) ? g_inittab_root : g_inittab_login;
    if(file_equals("/etc/inittab", want, sr_len(want))) return;
    if(!file_equals("/etc/inittab", other, sr_len(other))) return;
    if(install_file("/etc/inittab", (const uint8_t*)want, sr_len(want), 0644)==0){
        debugcon_writestring(want == g_inittab_login ? "[SYSROOT] default /etc/inittab: console login\n"
                                                     : "[SYSROOT] default /etc/inittab: root console (CONSOLE=root)\n");
    } else g_st.failed++;
#endif
}

/* [M43] make ROOT_PASSWORD=...: give root the hash computed on the build host,
 * but only while root's password is still the never-set "!" of a fresh
 * /etc/shadow; a password set (or locked) on the system is never overridden.
 * The plain password is not in the image, only its SHA-512 crypt hash. */
static void apply_root_password(void){
    static const char h[] = SECOS_ROOT_PWHASH;
    if(!h[0]) return;
    static char buf[16384], out[16384];
    int n=vfs_read_all("/etc/shadow", buf, sizeof(buf)-1);
    if(n<0){ debugcon_writestring("[SYSROOT] FAIL: no /etc/shadow for ROOT_PASSWORD\n"); return; }
    buf[n]=0;
    int ls=-1;                                           /* start of the root line */
    for(int p=0; p<n; ){
        if(sr_eqn(buf+p, "root:", 5)){ ls=p; break; }
        while(p<n && buf[p]!='\n') p++;
        p++;
    }
    if(ls<0){ debugcon_writestring("[SYSROOT] FAIL: no root entry in /etc/shadow\n"); return; }
    int f1=ls+5, f2=f1; while(f2<n && buf[f2]!=':' && buf[f2]!='\n') f2++;
    if(!(f2==f1+1 && buf[f1]=='!')){
        debugcon_writestring("[SYSROOT] root password already set on the system: ROOT_PASSWORD not applied\n");
        return;
    }
    size_t hl=sr_len(h), o=0;
    if((size_t)n + hl + 2 >= sizeof(out)) return;
    for(int i=0;i<f1;i++) out[o++]=buf[i];
    for(size_t i=0;i<hl;i++) out[o++]=h[i];
    for(int i=f2;i<n;i++) out[o++]=buf[i];
    if(install_file("/etc/shadow", (const uint8_t*)out, o, 0600)==0)
        debugcon_writestring("[SYSROOT] root password set from the build (ROOT_PASSWORD)\n");
    else { g_st.failed++; debugcon_writestring("[SYSROOT] FAIL: could not set the root password\n"); }
    for(size_t i=0;i<o;i++) out[i]=0;
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
#ifndef SECOS_NO_NANO
    debugcon_writestring(present("/usr/bin/nano") && present("/usr/share/terminfo/x/xterm") &&
                         present("/usr/share/terminfo/l/linux") && present("/usr/share/nano/c.nanorc")
                         ? "[NANO] installed GNU nano to /usr/bin/nano (+ syntax files, terminfo)\n"
                         : "[NANO] FAIL: could not install /usr/bin/nano\n");
#endif
#ifndef SECOS_NO_DROPBEAR
    debugcon_writestring(present("/bin/dropbearmulti") ? "[SSH] installed dropbear to /bin/dropbearmulti\n"
                                                       : "[SSH] FAIL: could not install /bin/dropbearmulti\n");
#endif
    { vfs_inode_t* su=vfs_lookup("/bin/su");
      int setuid_ok = su && (su->mode & 04000) && su->uid==0;
      debugcon_writestring(present("/bin/login") && setuid_ok && present("/bin/sudo") && present("/bin/passwd") &&
                           present("/bin/useradd") && present("/bin/id")
                           ? "[USERS] login/su/sudo/passwd (set-uid) + useradd/userdel/usermod/groupadd/groupdel/chpasswd/id/whoami/groups installed to /bin\n"
                           : "[USERS] FAIL: could not install the account tools\n"); }
    debugcon_writestring(present("/bin/netcfg") && present("/bin/ifup") && present("/bin/ifdown") && present("/bin/ifconfig")
                         ? "[NET] netcfg/ifup/ifdown/ifconfig installed to /bin\n"
                         : "[NET] FAIL: could not install netcfg\n");
}

/* [U8] Create the missing parent directories of the table's entries outside
 * g_dirs (e.g. /usr/share/terminfo/x), root:root 0755. Directories that
 * exist are left as they are. */
static void ensure_parents(void){
    for(int i=0;i<g_nsf;i++){
        char d[64]; sr_cpy(d, g_sf[i].path, sizeof(d));
        size_t ls=0; for(size_t k=0; d[k]; k++) if(d[k]=='/') ls=k;
        if(ls==0) continue;
        d[ls]=0;
        if(vfs_lookup(d)) continue;
        for(size_t k=1; k<=ls; k++){                  /* mkdir -p */
            if(d[k]!='/' && d[k]!=0) continue;
            char c=d[k]; d[k]=0;
            if(!vfs_lookup(d)){
                if(vfs_mkdir(d)==0) fix_attr(d, 0755, 0, 0);
                else { g_st.failed++; d[k]=c; break; }
            }
            d[k]=c;
        }
    }
}

static void sysroot_provision_locked(void);
void sysroot_provision(void){
    klock_lock(&sysroot_lock);
    sysroot_provision_locked();
    klock_unlock(&sysroot_lock);
}
static void sysroot_provision_locked(void){
    KLOCK_ASSERT_HELD(&sysroot_lock);
    uint64_t t0=timer_get_ticks();
    g_st.written=g_st.kept=g_st.linked=g_st.removed=g_st.created=g_st.failed=g_st.fixed=0;
    for(unsigned i=0;i<sizeof(g_dirs)/sizeof(g_dirs[0]);i++){
        if(g_dirs[i].flags & SD_CREATE) ensure_dir(g_dirs[i].p);
        count_fix(fix_attr(g_dirs[i].p, g_dirs[i].mode, 0, 0));
    }
    build_table();
    ensure_parents();
    if(g_persistent) provision_disk(); else provision_ramfs();
    ensure_groups();
    apply_root_password();
    update_default_inittab();
    // Release stamp of the running kernel (kernel-owned).
    write_if_changed("/etc/secos-release",
        "NAME=SecOS\nVERSION=0.1.0-dev\nBUILD_TS=" BUILD_TS "\nGIT_HASH=" GIT_HASH "\n");
    count_fix(fix_attr("/etc/secos-release", 0644, 0, 0));
    if(g_persistent){
        write_if_changed("/VERSION", "VERSION=0.1.0-dev\nBUILD_TS=" BUILD_TS "\nGIT_HASH=" GIT_HASH "\n");
        count_fix(fix_attr("/VERSION", 0644, 0, 0));
    }
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
    debugcon_writestring(" modes_fixed="); dbg_dec((uint64_t)g_st.fixed);
    debugcon_writestring(" failed="); dbg_dec((uint64_t)g_st.failed);
    debugcon_writestring(" ms="); dbg_dec(dt); debugcon_writestring("\n");
    if(g_persistent && (g_st.written || g_st.linked || g_st.removed)){
        terminal_writestring("[SYSROOT] system files updated from the kernel image: ");
        con_dec((uint64_t)(g_st.written+g_st.linked)); terminal_writestring(" written, ");
        con_dec((uint64_t)g_st.removed); terminal_writestring(" removed\n");
    }
}

/* /etc/vconsole.conf: KEYMAP=<layout> (written by /bin/loadkeys). */
static void sysroot_apply_settings_locked(void);
void sysroot_apply_settings(void){
    klock_lock(&sysroot_lock);
    sysroot_apply_settings_locked();
    klock_unlock(&sysroot_lock);
}
static void sysroot_apply_settings_locked(void){
    KLOCK_ASSERT_HELD(&sysroot_lock);
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

int sysroot_install_exec(const char* path, const void* data, size_t len){
    klock_lock(&sysroot_lock);
    int r=vfs_create(path, data, len);
    if(r==0) fix_attr(path, 0755, 0, 0);   /* best effort: a FAT32 mount has fixed modes */
    klock_unlock(&sysroot_lock);
    return r;
}
