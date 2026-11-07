/*
 * procfs.c — [M23] /proc virtual filesystem, Linux-compatible layout.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Generated-on-read kernel introspection in the text formats procps/busybox
 * tools parse (ps, top, free, uptime, pidof, mount, df, dmesg):
 *
 *   /proc/{meminfo,uptime,loadavg,stat,version,cpuinfo,mounts,filesystems,
 *          kmsg,cmdline}
 *   /proc/self            -> symlink to the caller's pid
 *   /proc/<pid>/{stat,status,statm,cmdline,comm,environ,maps}
 *   /proc/<pid>/{cwd,exe,root} (symlinks), /proc/<pid>/fd/ (one symlink per
 *                                open descriptor)
 *
 * Listing /proc enumerates the live pids. Content is produced fresh on every
 * read (under one lock, into a static buffer) and the requested slice
 * returned; a process's data is snapshotted inside process_foreach (proc_lock
 * held), so a task exiting concurrently is never dereferenced afterwards. A
 * pure namespace: no create/write.
 */
#include "vfs.h"
#include "pmm.h"
#include "process.h"
#include "cred.h"
#include "spinlock.h"
#include "kiface.h"
#include "klog.h"
#include "kclock.h"
#include "../kernel/signal.h"
#include <stddef.h>
#include <stdint.h>

extern uint64_t timer_get_ticks(void);
extern uint32_t timer_get_frequency(void);
extern process_t* sched_get_current(void);
extern void process_foreach(void (*cb)(process_t*, void*), void* user);
extern uint64_t tsc_hz(void);

/* ------------------------------------------------------------ text builder */
struct sb { char* p; int cap; int len; };
static void sb_init(struct sb* s, char* b, int c){ s->p=b; s->cap=c; s->len=0; }
static void sb_putc(struct sb* s, char c){ if(s->len < s->cap-1) s->p[s->len++]=c; }
static void sb_str(struct sb* s, const char* t){ while(t && *t) sb_putc(s, *t++); }
static void sb_u64w(struct sb* s, uint64_t v, int width, char pad){
    char tmp[24]; int n=0;
    do { tmp[n++]=(char)('0'+(v%10)); v/=10; } while(v);
    while(n<width) tmp[n++]=pad;
    while(n--) sb_putc(s, tmp[n]);
}
static void sb_u64(struct sb* s, uint64_t v){ sb_u64w(s, v, 0, ' '); }
static void sb_i64(struct sb* s, int64_t v){ if(v<0){ sb_putc(s,'-'); sb_u64(s,(uint64_t)(-v)); } else sb_u64(s,(uint64_t)v); }
static void sb_hex(struct sb* s, uint64_t v, int width){
    static const char h[]="0123456789abcdef";
    char tmp[20]; int n=0;
    do { tmp[n++]=h[v&15]; v>>=4; } while(v);
    while(n<width) tmp[n++]='0';
    while(n--) sb_putc(s, tmp[n]);
}
static void sb_oct4(struct sb* s, uint32_t v){
    for(int sh=9; sh>=0; sh-=3) sb_putc(s, (char)('0'+((v>>sh)&7)));
}
/* fixed point with 2 decimals: v / div */
static void sb_fix2(struct sb* s, uint64_t v, uint64_t div){
    sb_u64(s, v/div); sb_putc(s,'.');
    sb_u64w(s, (v%div)*100/div, 2, '0');
}
static void sb_kv_kb(struct sb* s, const char* k, uint64_t kb){
    sb_str(s, k); int n=0; while(k[n]) n++;
    sb_u64w(s, kb, 16 - n > 0 ? 16 - n + 8 : 8, ' ');
    sb_str(s, " kB\n");
}

/* ------------------------------------------------------------ node keys */
enum pk {
    PK_ROOT=1, PK_SELF,
    PK_MEMINFO, PK_UPTIME, PK_LOADAVG, PK_STAT, PK_VERSION, PK_CPUINFO,
    PK_MOUNTS, PK_FILESYSTEMS, PK_KMSG, PK_CMDLINE_K, PK_SMP, PK_LOCKSTAT,
    PK_PID_DIR, PK_PID_STAT, PK_PID_STATUS, PK_PID_STATM, PK_PID_CMDLINE,
    PK_PID_COMM, PK_PID_ENVIRON, PK_PID_MAPS, PK_PID_CWD, PK_PID_EXE,
    PK_PID_ROOT, PK_PID_FDDIR, PK_PID_FD,
};
#define KEY(kind,pid,fd) ((uint64_t)(kind) | ((uint64_t)(pid)<<8) | ((uint64_t)(fd)<<40))
#define K_KIND(k) ((int)((k)&0xff))
#define K_PID(k)  ((uint32_t)(((k)>>8)&0xffffffffu))
#define K_FD(k)   ((int)(((k)>>40)&0xff))

static const struct { const char* name; int kind; } g_top[] = {
    {"meminfo",PK_MEMINFO}, {"uptime",PK_UPTIME}, {"loadavg",PK_LOADAVG},
    {"stat",PK_STAT}, {"version",PK_VERSION}, {"cpuinfo",PK_CPUINFO},
    {"mounts",PK_MOUNTS}, {"filesystems",PK_FILESYSTEMS}, {"kmsg",PK_KMSG},
    {"cmdline",PK_CMDLINE_K}, {"smp",PK_SMP}, {"lockstat",PK_LOCKSTAT},
};
#define NTOP (int)(sizeof(g_top)/sizeof(g_top[0]))
static const struct { const char* name; int kind; } g_pidf[] = {
    {"stat",PK_PID_STAT}, {"status",PK_PID_STATUS}, {"statm",PK_PID_STATM},
    {"cmdline",PK_PID_CMDLINE}, {"comm",PK_PID_COMM}, {"environ",PK_PID_ENVIRON},
    {"maps",PK_PID_MAPS}, {"cwd",PK_PID_CWD}, {"exe",PK_PID_EXE},
    {"root",PK_PID_ROOT}, {"fd",PK_PID_FDDIR},
};
#define NPIDF (int)(sizeof(g_pidf)/sizeof(g_pidf[0]))

static int seq(const char* a, const char* b){ int i=0; while(a[i]&&b[i]){ if(a[i]!=b[i]) return 0; i++; } return a[i]==b[i]; }

/* ------------------------------------------------------------ process snapshot */
#define PS_MAXVMA 64
struct psnap {
    int      found;
    uint32_t pid, ppid, pgid, cpu;
    int32_t  pin;            /* hard affinity (cpu_pin), -1 = any online CPU */
    int      state;
    char     comm[16];
    char     cmdline[256]; int cmdlen;
    char     cwd[256];
    uint64_t utime, stime, start_ns, nvcsw;
    uint64_t vsz, rss_pages, brk_start, brk_cur, stack_top;
    uint64_t sig_pending, sig_blocked, sig_ign, sig_catch;
    uint32_t umask;
    int      exit_code;
    int      on_console;
    struct { uint8_t kind; uint8_t pipe_w; const void* obj; const vfs_fs_ops_t* ops; char path[128]; } fd[32];
    int      nvma;
    struct { uint64_t start, end, flags; uint8_t type; uint8_t has_inode; } vma[PS_MAXVMA];
    struct proc_cred cred;   /* [M43] owner of /proc/<pid> = the effective ids */
    int      inspect_ok;     /* [M43] the reader may see the private files */
};
enum { FDK_NONE=0, FDK_FILE, FDK_PIPE, FDK_TTY, FDK_SOCK };
static struct psnap g_ps;
static uint32_t g_ps_want;

static void snap_cb(process_t* p, void* u){
    (void)u;
    if(p->pid != g_ps_want || g_ps.found) return;
    struct psnap* s = &g_ps;
    s->found = 1;
    s->pid = p->pid; s->ppid = p->ppid; s->pgid = p->pgid; s->cpu = p->cpu_affinity; s->pin = p->cpu_pin;
    s->cred = p->cred;
    s->inspect_ok = cred_may_inspect(sched_get_current(), p);
    s->state = p->state;
    for(int i=0;i<16;i++) s->comm[i]=p->ki.comm[i];
    s->comm[15]=0;
    s->cmdlen = p->ki.cmdline_len > sizeof(s->cmdline) ? (int)sizeof(s->cmdline) : p->ki.cmdline_len;
    for(int i=0;i<s->cmdlen;i++) s->cmdline[i]=p->ki.cmdline[i];
    for(int i=0;i<256;i++){ s->cwd[i]=p->cwd[i]; if(!p->cwd[i]) break; }
    s->cwd[255]=0;
    s->utime=p->ki.utime_ticks; s->stime=p->ki.stime_ticks; s->start_ns=p->ki.start_ns; s->nvcsw=p->ki.nvcsw;
    /* [M50-B1] The address space (VMA set, page tables, brk) under the
     * target's mm_lock: it may munmap/brk/exec on another CPU meanwhile.
     * proc_lock (S30, process_foreach) -> mm_lock (S35). */
    uint64_t mfl = mm_lock(p);
    s->vsz = vma_total_bytes(&p->vmas);
    s->rss_pages = (p->state!=PROC_ZOMBIE && p->space) ? vmm_count_user_pages(p->space) : 0;
    s->brk_start=p->brk_start; s->brk_cur=p->brk_cur; s->stack_top=p->stack_top;
    s->nvma=0;
    for(uint32_t i=0;i<p->vmas.count && s->nvma<PS_MAXVMA;i++){
        const vma_t* v=&p->vmas.v[i];
        if(v->type==VMA_TYPE_NONE) continue;
        s->vma[s->nvma].start=v->start; s->vma[s->nvma].end=v->end; s->vma[s->nvma].flags=v->flags;
        s->vma[s->nvma].type=v->type; s->vma[s->nvma].has_inode = v->file_inode ? 1 : 0;
        s->nvma++;
    }
    mm_unlock(p, mfl);
    s->sig_pending=p->sig_pending; s->sig_blocked=p->sig_blocked;
    s->sig_ign=0; s->sig_catch=0;
    for(int i=1;i<32;i++){ if(p->sig_handler[i]==1) s->sig_ign|=1ull<<(i-1); else if(p->sig_handler[i]>1) s->sig_catch|=1ull<<(i-1); }
    s->umask=p->ki.umask; s->exit_code=p->exit_code;
    s->on_console = (!p->fds[0].used) || (!p->fds[0].inode && !p->fds[0].is_pipe);
    for(int i=0;i<32;i++){
        struct proc_fd_entry* e=&p->fds[i];
        s->fd[i].kind=FDK_NONE; s->fd[i].path[0]=0; s->fd[i].obj=0; s->fd[i].ops=0;
        if(!e->used) continue;
        if(e->is_pipe){ s->fd[i].kind=FDK_PIPE; s->fd[i].pipe_w=(uint8_t)e->pipe_w; s->fd[i].obj=e->inode; }
        else if(e->is_sock){ s->fd[i].kind=FDK_SOCK; s->fd[i].obj=(void*)(uintptr_t)(e->is_sock-1); }
        else if(!e->inode) s->fd[i].kind=FDK_TTY;
        else {
            const vfs_inode_t* ino=(const vfs_inode_t*)e->inode;
            s->fd[i].kind=FDK_FILE; s->fd[i].ops=ino->ops;
            int k=0; for(; ino->path[k] && k<127; k++) s->fd[i].path[k]=ino->path[k];
            s->fd[i].path[k]=0;
        }
    }
}
static int snap(uint32_t pid){
    g_ps.found=0; g_ps_want=pid;
    process_foreach(snap_cb, 0);
    return g_ps.found;
}

struct pidlist { uint32_t pid[64]; int n; };
static void pids_cb(process_t* p, void* u){
    struct pidlist* l=(struct pidlist*)u;
    if(p->pid==0 || l->n>=64) return;
    l->pid[l->n++]=p->pid;
}
static void list_pids(struct pidlist* l){
    l->n=0; process_foreach(pids_cb, l);
    for(int i=1;i<l->n;i++){ uint32_t v=l->pid[i]; int j=i-1; while(j>=0 && l->pid[j]>v){ l->pid[j+1]=l->pid[j]; j--; } l->pid[j+1]=v; }
}

static char state_char(int st){
    switch(st){ case PROC_BLOCKED: return 'S'; case PROC_STOPPED: return 'T'; case PROC_ZOMBIE: return 'Z'; default: return 'R'; }
}
static const char* state_name(int st){
    switch(st){ case PROC_BLOCKED: return "S (sleeping)"; case PROC_STOPPED: return "T (stopped)";
                case PROC_ZOMBIE: return "Z (zombie)"; default: return "R (running)"; }
}

/* Absolute path of an fd's file: mount point + mount-relative inode path. */
static void fd_target(struct sb* s, int i){
    switch(g_ps.fd[i].kind){
    case FDK_PIPE: sb_str(s,"pipe:["); sb_u64(s, ((uint64_t)(uintptr_t)g_ps.fd[i].obj>>4)&0xffffff); sb_putc(s,']'); return;
    case FDK_TTY:  sb_str(s,"/dev/tty"); return;
    case FDK_SOCK: sb_str(s,"socket:["); sb_u64(s, (uint64_t)(uintptr_t)g_ps.fd[i].obj); sb_putc(s,']'); return;
    case FDK_FILE: {
        const char* mp = vfs_mount_point_of(g_ps.fd[i].ops);
        if(mp && !(mp[0]=='/' && mp[1]==0)){ sb_str(s, mp); if(g_ps.fd[i].path[0]=='/' && g_ps.fd[i].path[1]==0) return; }
        sb_str(s, g_ps.fd[i].path);
        return; }
    }
}

/* ------------------------------------------------------------ generators */
static void gen_meminfo(struct sb* s){
    uint64_t total=pmm_get_total_memory()/1024, free=pmm_get_free_memory()/1024;
    sb_kv_kb(s,"MemTotal:",total);
    sb_kv_kb(s,"MemFree:",free);
    sb_kv_kb(s,"MemAvailable:",free);
    sb_kv_kb(s,"Buffers:",0);
    sb_kv_kb(s,"Cached:",0);
    sb_kv_kb(s,"SwapCached:",0);
    sb_kv_kb(s,"Active:",total-free);
    sb_kv_kb(s,"Inactive:",0);
    sb_kv_kb(s,"SwapTotal:",0);
    sb_kv_kb(s,"SwapFree:",0);
    sb_kv_kb(s,"Shmem:",0);
    sb_kv_kb(s,"Slab:",0);
}
static uint64_t idle_ticks_total(void){
    uint64_t t=0; kiface_cpu_stat_t c;
    for(uint32_t i=0;i<kiface_cpu_count();i++){ kiface_cpu_stat(i,&c); t+=c.idle; }
    return t;
}
static void gen_uptime(struct sb* s){
    uint64_t ns = kclock_mono_ns();
    sb_fix2(s, ns/10000000ull, 100); sb_putc(s,' ');           /* seconds.cs */
    uint32_t hz = timer_get_frequency(); if(!hz) hz=1000;
    sb_fix2(s, idle_ticks_total()*100/hz, 100); sb_putc(s,'\n');
}
struct run_ctx { int run, tot; };
static void run_cb(process_t* p, void* u){
    struct run_ctx* r=(struct run_ctx*)u; if(p->pid==0) return;
    r->tot++;
    if(p->state==PROC_READY||p->state==PROC_RUNNING||p->state==PROC_NEW) r->run++;
}
static void gen_loadavg(struct sb* s){
    uint64_t l[3]; kiface_loadavg(l);
    for(int i=0;i<3;i++){ sb_fix2(s, l[i]*100>>11, 100); sb_putc(s,' '); }
    struct run_ctx r={0,0}; process_foreach(run_cb,&r);
    sb_u64(s,(uint64_t)(r.run?r.run:1)); sb_putc(s,'/'); sb_u64(s,(uint64_t)r.tot); sb_putc(s,' ');
    sb_u64(s, kiface_last_pid()); sb_putc(s,'\n');
}
static void gen_cpu_line(struct sb* s, const char* name, const kiface_cpu_stat_t* c){
    uint32_t hz = timer_get_frequency(); if(!hz) hz=1000;
    sb_str(s,name);
    sb_putc(s,' '); sb_u64(s, c->user*100/hz);
    sb_str(s," 0 "); sb_u64(s, c->sys*100/hz);
    sb_putc(s,' '); sb_u64(s, c->idle*100/hz);
    sb_str(s," 0 0 0 0 0 0\n");
}
static void gen_stat(struct sb* s){
    kiface_cpu_stat_t tot={0,0,0}, c;
    uint32_t n=kiface_cpu_count();
    for(uint32_t i=0;i<n;i++){ kiface_cpu_stat(i,&c); tot.user+=c.user; tot.sys+=c.sys; tot.idle+=c.idle; }
    gen_cpu_line(s,"cpu ",&tot);
    for(uint32_t i=0;i<n;i++){
        char nm[8]="cpu"; int k=3; uint32_t v=i; char d[4]; int dn=0; do{ d[dn++]=(char)('0'+v%10); v/=10; }while(v);
        while(dn--) nm[k++]=d[dn];
        nm[k]=0;
        kiface_cpu_stat(i,&c); gen_cpu_line(s,nm,&c);
    }
    sb_str(s,"intr 0\nctxt "); sb_u64(s,kiface_ctxt());
    sb_str(s,"\nbtime "); sb_u64(s,(kclock_real_ns()-kclock_mono_ns())/1000000000ull);
    sb_str(s,"\nprocesses "); sb_u64(s,kiface_forks());
    struct run_ctx r={0,0}; process_foreach(run_cb,&r);
    sb_str(s,"\nprocs_running "); sb_u64(s,(uint64_t)(r.run?r.run:1));
    sb_str(s,"\nprocs_blocked 0\n");
}
static void gen_version(struct sb* s){
    sb_str(s,"SecOS version 0.1.0-dev (gcc) #1 SMP build "); sb_str(s, BUILD_TS);
    sb_str(s," git "); sb_str(s, GIT_HASH); sb_putc(s,'\n');
}
static void cpuid(uint32_t leaf, uint32_t r[4]){
    __asm__ volatile("cpuid" : "=a"(r[0]),"=b"(r[1]),"=c"(r[2]),"=d"(r[3]) : "a"(leaf),"c"(0));
}
static void gen_cpuinfo(struct sb* s){
    uint32_t r[4]; char vendor[13]; char brand[49];
    cpuid(0,r);
    for(int i=0;i<4;i++){ vendor[i]=(char)(r[1]>>(8*i)); vendor[4+i]=(char)(r[3]>>(8*i)); vendor[8+i]=(char)(r[2]>>(8*i)); }
    vendor[12]=0;
    brand[0]=0;
    cpuid(0x80000000u,r);
    if(r[0]>=0x80000004u){
        for(uint32_t l=0;l<3;l++){ cpuid(0x80000002u+l,r); for(int j=0;j<16;j++) brand[l*16+j]=(char)(r[j/4]>>(8*(j%4))); }
        brand[48]=0;
    }
    const char* b=brand; while(*b==' ') b++;
    cpuid(1,r);
    uint32_t fam=(r[0]>>8)&15, model=(r[0]>>4)&15, step=r[0]&15;
    if(fam==15) fam+=(r[0]>>20)&0xff;
    if(fam==6||fam>=15) model+=((r[0]>>16)&15)<<4;
    for(uint32_t i=0;i<kiface_cpu_count();i++){
        sb_str(s,"processor\t: "); sb_u64(s,i);
        sb_str(s,"\nvendor_id\t: "); sb_str(s,vendor);
        sb_str(s,"\ncpu family\t: "); sb_u64(s,fam);
        sb_str(s,"\nmodel\t\t: "); sb_u64(s,model);
        sb_str(s,"\nmodel name\t: "); sb_str(s,*b?b:"x86_64 CPU");
        sb_str(s,"\nstepping\t: "); sb_u64(s,step);
        sb_str(s,"\ncpu MHz\t\t: "); sb_u64(s,tsc_hz()/1000000ull); sb_str(s,".000");
        sb_str(s,"\ncpu cores\t: "); sb_u64(s,kiface_cpu_count());
        sb_str(s,"\nflags\t\t: fpu tsc pae nx lm sse sse2");
        sb_str(s,"\nbogomips\t: "); sb_u64(s,2*tsc_hz()/1000000ull); sb_str(s,".00");
        sb_str(s,"\n\n");
    }
}
static const char* mnt_type(const char* fs){
    if(seq(fs,"procfs")) return "proc";
    if(seq(fs,"devfs")) return "devtmpfs";
    if(seq(fs,"fat32")) return "vfat";
    return fs;
}
static void gen_mounts(struct sb* s){
    int n = vfs_mount_count();
    for(int i=0;i<n;i++){
        const char* mp=0; const char* fs=0;
        if(vfs_mount_info(i,&mp,&fs)!=0) continue;
        const char* type=mnt_type(fs?fs:"none");
        const char* src=type;                    /* virtual FS: source == type */
        int dev=0;
        /* disk filesystems name their device + real type (ext4 vs ext2) */
        const vfs_fs_ops_t* ops = vfs_mount_ops_at(i);
        if(ops && ops->mntinfo){ const char* a=0; const char* b=0; if(ops->mntinfo(&a,&b)==0){ if(a){ src=a; dev=1; } if(b) type=b; } }
        if(dev) sb_str(s,"/dev/");
        sb_str(s, src); sb_putc(s,' '); sb_str(s, mp?mp:"?"); sb_putc(s,' ');
        sb_str(s, type);
        sb_str(s, (ops && (ops->flags & VFS_FS_RDONLY)) ? " ro,relatime 0 0\n" : " rw,relatime 0 0\n");
    }
}
static void gen_filesystems(struct sb* s){
    sb_str(s,"nodev\tramfs\nnodev\tproc\nnodev\tdevtmpfs\nnodev\tsysfs\n\tvfat\n\texfat\n\tntfs\n\text2\n\text3\n\text4\n");
}

/* --- per pid (g_ps snapshot) --- */
static void gen_pid_stat(struct sb* s){
    uint32_t hz = timer_get_frequency(); if(!hz) hz=1000;
    uint64_t code_lo=0, code_hi=0;
    for(int i=0;i<g_ps.nvma;i++) if(!(g_ps.vma[i].flags & VMM_FLAG_NOEXEC) && !(g_ps.vma[i].flags & VMM_FLAG_PROTNONE)){
        if(!code_lo || g_ps.vma[i].start<code_lo) code_lo=g_ps.vma[i].start;
        if(g_ps.vma[i].end>code_hi) code_hi=g_ps.vma[i].end;
    }
    uint32_t fg = signal_get_foreground_pgid();
    sb_u64(s,g_ps.pid); sb_str(s," ("); sb_str(s,g_ps.comm); sb_str(s,") ");
    sb_putc(s,state_char(g_ps.state)); sb_putc(s,' ');
    sb_u64(s,g_ps.ppid); sb_putc(s,' '); sb_u64(s,g_ps.pgid); sb_putc(s,' '); sb_u64(s,g_ps.pgid); sb_putc(s,' ');
    sb_u64(s, g_ps.on_console ? 0x0401 : 0); sb_putc(s,' ');                    /* tty_nr: tty1 */
    sb_i64(s, g_ps.on_console ? (int64_t)(fg?fg:g_ps.pgid) : -1); sb_putc(s,' ');  /* tpgid */
    sb_str(s,"0 0 0 0 0 ");                                                     /* flags minflt cminflt majflt cmajflt */
    sb_u64(s,g_ps.utime*100/hz); sb_putc(s,' '); sb_u64(s,g_ps.stime*100/hz);
    sb_str(s," 0 0 20 0 1 0 ");                                                 /* cutime cstime prio nice threads itreal */
    sb_u64(s,g_ps.start_ns/10000000ull); sb_putc(s,' ');                        /* starttime (USER_HZ) */
    sb_u64(s,g_ps.vsz); sb_putc(s,' '); sb_u64(s,g_ps.rss_pages);
    sb_str(s," 18446744073709551615 ");
    sb_u64(s,code_lo); sb_putc(s,' '); sb_u64(s,code_hi); sb_putc(s,' '); sb_u64(s,g_ps.stack_top);
    sb_str(s," 0 0 ");                                                          /* kstkesp kstkeip */
    sb_u64(s,g_ps.sig_pending&0xffffffffu); sb_putc(s,' '); sb_u64(s,g_ps.sig_blocked&0xffffffffu); sb_putc(s,' ');
    sb_u64(s,g_ps.sig_ign); sb_putc(s,' '); sb_u64(s,g_ps.sig_catch);
    sb_str(s," 0 0 0 17 "); sb_u64(s,g_ps.cpu);                                 /* wchan nswap cnswap exit_signal cpu */
    sb_str(s," 0 0 0 0 0 0 0 "); sb_u64(s,g_ps.brk_start);                      /* .. start_data end_data start_brk */
    sb_str(s," 0 0 0 0 "); sb_i64(s, g_ps.state==PROC_ZOMBIE ? g_ps.exit_code : 0); sb_putc(s,'\n');
}
static uint64_t vm_kb_where(int stack){
    uint64_t b=0;
    for(int i=0;i<g_ps.nvma;i++){
        int is_stack = (g_ps.vma[i].end==g_ps.stack_top) || (g_ps.vma[i].start<g_ps.stack_top && g_ps.vma[i].end>=g_ps.stack_top);
        if(stack ? is_stack : (!is_stack && (g_ps.vma[i].flags & VMM_FLAG_RW))) b+=g_ps.vma[i].end-g_ps.vma[i].start;
    }
    return b/1024;
}
static void gen_pid_status(struct sb* s){
    sb_str(s,"Name:\t"); sb_str(s,g_ps.comm);
    sb_str(s,"\nUmask:\t"); sb_oct4(s,g_ps.umask);
    sb_str(s,"\nState:\t"); sb_str(s,state_name(g_ps.state));
    sb_str(s,"\nTgid:\t"); sb_u64(s,g_ps.pid);
    sb_str(s,"\nNgid:\t0\nPid:\t"); sb_u64(s,g_ps.pid);
    sb_str(s,"\nPPid:\t"); sb_u64(s,g_ps.ppid);
    sb_str(s,"\nTracerPid:\t0\nUid:\t"); sb_u64(s,g_ps.cred.ruid); sb_putc(s,'\t'); sb_u64(s,g_ps.cred.euid);
    sb_putc(s,'\t'); sb_u64(s,g_ps.cred.suid); sb_putc(s,'\t'); sb_u64(s,g_ps.cred.euid);   /* fsuid = euid */
    sb_str(s,"\nGid:\t"); sb_u64(s,g_ps.cred.rgid); sb_putc(s,'\t'); sb_u64(s,g_ps.cred.egid);
    sb_putc(s,'\t'); sb_u64(s,g_ps.cred.sgid); sb_putc(s,'\t'); sb_u64(s,g_ps.cred.egid);
    sb_str(s,"\nFDSize:\t32\nGroups:\t");
    for(uint32_t i=0;i<g_ps.cred.ngroups && i<CRED_NGROUPS;i++){ sb_u64(s,g_ps.cred.groups[i]); sb_putc(s,' '); }
    sb_putc(s,'\n');
    sb_kv_kb(s,"VmPeak:",g_ps.vsz/1024);
    sb_kv_kb(s,"VmSize:",g_ps.vsz/1024);
    sb_kv_kb(s,"VmLck:",0);
    sb_kv_kb(s,"VmHWM:",g_ps.rss_pages*4);
    sb_kv_kb(s,"VmRSS:",g_ps.rss_pages*4);
    sb_kv_kb(s,"VmData:",vm_kb_where(0));
    sb_kv_kb(s,"VmStk:",vm_kb_where(1));
    sb_str(s,"Threads:\t1\nSigQ:\t0/0\nSigPnd:\t"); sb_hex(s,g_ps.sig_pending,16);
    sb_str(s,"\nShdPnd:\t0000000000000000\nSigBlk:\t"); sb_hex(s,g_ps.sig_blocked,16);
    sb_str(s,"\nSigIgn:\t"); sb_hex(s,g_ps.sig_ign,16);
    sb_str(s,"\nSigCgt:\t"); sb_hex(s,g_ps.sig_catch,16);
    sb_str(s,"\nCpus_allowed_list:\t");      /* same set as sched_getaffinity */
    if(g_ps.pin >= 0) sb_u64(s,(uint64_t)g_ps.pin);
    else {
        extern uint32_t smp_cpu_count(void); extern int smp_cpu_online(uint32_t);
        uint32_t n = smp_cpu_count(); if(n == 0) n = 1;
        int first = 1;
        for(uint32_t i = 0; i < n; ){
            if(!smp_cpu_online(i)){ i++; continue; }
            uint32_t j = i; while(j + 1 < n && smp_cpu_online(j + 1)) j++;
            if(!first) sb_putc(s, ',');
            first = 0; sb_u64(s, i);
            if(j > i){ sb_putc(s, '-'); sb_u64(s, j); }
            i = j + 1;
        }
    }
    sb_str(s,"\nvoluntary_ctxt_switches:\t"); sb_u64(s,g_ps.nvcsw);
    sb_str(s,"\nnonvoluntary_ctxt_switches:\t0\n");
}
static void gen_pid_statm(struct sb* s){
    uint64_t sz=g_ps.vsz/4096;
    sb_u64(s,sz); sb_putc(s,' '); sb_u64(s,g_ps.rss_pages); sb_str(s," 0 0 0 ");
    sb_u64(s,vm_kb_where(0)/4); sb_str(s," 0\n");
}
static void gen_pid_maps(struct sb* s){
    for(int i=0;i<g_ps.nvma;i++){
        uint64_t f=g_ps.vma[i].flags;
        int none = (f & VMM_FLAG_PROTNONE) ? 1 : 0;
        sb_hex(s,g_ps.vma[i].start,8); sb_putc(s,'-'); sb_hex(s,g_ps.vma[i].end,8); sb_putc(s,' ');
        sb_putc(s, none?'-':'r'); sb_putc(s, (!none && (f&VMM_FLAG_RW))?'w':'-');
        sb_putc(s, (!none && !(f&VMM_FLAG_NOEXEC))?'x':'-');
        sb_putc(s, g_ps.vma[i].type==VMA_TYPE_PHYS ? 's' : 'p');   // [M40] device map is shared
        sb_str(s," 00000000 00:00 0");
        const char* name=0;
        if(g_ps.vma[i].start<g_ps.stack_top && g_ps.vma[i].end>=g_ps.stack_top) name="[stack]";
        else if(g_ps.vma[i].start>=g_ps.brk_start && g_ps.vma[i].end<=g_ps.brk_cur && g_ps.brk_cur>g_ps.brk_start) name="[heap]";
        else if(g_ps.vma[i].type==VMA_TYPE_FILE && !g_ps.vma[i].has_inode) name=g_ps.comm;
        else if(g_ps.vma[i].type==VMA_TYPE_PHYS) name="/dev/fb0";   // the only device map (M40)
        if(name){ sb_str(s,"                          "); if(name==g_ps.comm && g_ps.cmdlen && g_ps.cmdline[0]=='/') sb_str(s,g_ps.cmdline); else sb_str(s,name); }
        sb_putc(s,'\n');
    }
}
static void gen_pid_link(struct sb* s, int kind, int fd){
    switch(kind){
    case PK_PID_CWD: sb_str(s, g_ps.cwd[0]?g_ps.cwd:"/"); break;
    case PK_PID_ROOT: sb_str(s,"/"); break;
    case PK_PID_EXE:
        if(g_ps.cmdlen && g_ps.cmdline[0]=='/') sb_str(s,g_ps.cmdline);
        else { sb_str(s,"/bin/"); sb_str(s,g_ps.comm); }
        break;
    case PK_PID_FD: fd_target(s, fd); break;
    }
}

/* ------------------------------------------------------------ inode pool */
static char        g_genbuf[16384];
static vfs_inode_t g_ipool[64];
static uint64_t    g_ikey[64];
static int         g_inext;
static spinlock_t  g_proc_lock = SPINLOCK_INIT_CLASS(LK_S15_SUBSYS, "procfs");

static int is_link(int k){ return k==PK_SELF||k==PK_PID_CWD||k==PK_PID_EXE||k==PK_PID_ROOT||k==PK_PID_FD; }
/* [M48] /proc/smp: per-CPU scheduler and IPI counters (SecOS-specific). */
static void gen_smp(struct sb* s){
    extern uint32_t smp_cpu_count(void); extern uint32_t smp_online_count(void);
    extern int lapic_x2apic_mode(void);
    extern uint64_t tlb_stat_shootdowns(void); extern uint64_t tlb_stat_timeouts(void);
    extern void sched_cpu_stats(uint32_t, uint64_t*, uint64_t*, uint64_t*, uint64_t*);
    extern uint32_t smp_cpu_lapic_id(uint32_t);
    sb_str(s,"cpus "); sb_u64(s, smp_cpu_count());
    sb_str(s," online "); sb_u64(s, smp_online_count());
    sb_str(s," x2apic "); sb_u64(s, (uint64_t)lapic_x2apic_mode());
    sb_str(s," shootdowns "); sb_u64(s, tlb_stat_shootdowns());
    sb_str(s," shootdown_timeouts "); sb_u64(s, tlb_stat_timeouts());
    sb_putc(s,'\n');
    for(uint32_t i=0;i<smp_cpu_count();i++){
        uint64_t sw, mg, it, ir; sched_cpu_stats(i,&sw,&mg,&it,&ir);
        sb_str(s,"cpu"); sb_u64(s,i);
        sb_str(s," apic "); sb_u64(s, smp_cpu_lapic_id(i));
        sb_str(s," switches "); sb_u64(s,sw);
        sb_str(s," migrations "); sb_u64(s,mg);
        sb_str(s," ipi_tlb "); sb_u64(s,it);
        sb_str(s," ipi_resched "); sb_u64(s,ir);
        sb_putc(s,'\n');
    }
    /* [M50] kernel lock mode + how many syscalls still take the BKL */
    extern int g_bkl_mode; extern uint32_t syscall_bkl_mask_count(void);
    sb_str(s,"bkl "); sb_str(s, g_bkl_mode ? "on" : "off");
    sb_str(s," mask "); sb_u64(s, syscall_bkl_mask_count()); sb_str(s,"/256");
#ifdef SECOS_LOCKDEP
    sb_str(s," lockdep on");
#endif
    sb_putc(s,'\n');
}

/* [M50] /proc/lockstat: per-lock acquire / contended / max-wait counts. */
static void lockstat_emit(void* ctx, const char* line){ sb_str((struct sb*)ctx, line); }
static void gen_lockstat(struct sb* s){ lockstat_generate(lockstat_emit, s); }

/* [M43] What Linux guards with a ptrace-style check: only root or a reader
 * whose effective ids equal all of the target's ids (cred_may_inspect). */
static int is_private(int k){ return k==PK_PID_ENVIRON||k==PK_PID_MAPS||k==PK_PID_CWD||k==PK_PID_EXE||
                                     k==PK_PID_ROOT||k==PK_PID_FDDIR||k==PK_PID_FD; }
static int is_dir(int k){ return k==PK_ROOT||k==PK_PID_DIR||k==PK_PID_FDDIR; }

/* Generate node `key` into g_genbuf; returns its length (-1 = gone). Caller
 * holds g_proc_lock. kmsg is served straight from the log ring instead. */
static int gen_key(uint64_t key){
    struct sb s; sb_init(&s, g_genbuf, sizeof(g_genbuf));
    int k=K_KIND(key);
    if(k>=PK_PID_DIR){
        if(!snap(K_PID(key))) return -1;
        if(k==PK_PID_FD && (K_FD(key)>=32 || g_ps.fd[K_FD(key)].kind==FDK_NONE)) return -1;
        if(is_private(k) && !g_ps.inspect_ok) return -1;          /* [M43] */
    }
    switch(k){
    case PK_MEMINFO: gen_meminfo(&s); break;
    case PK_UPTIME: gen_uptime(&s); break;
    case PK_LOADAVG: gen_loadavg(&s); break;
    case PK_STAT: gen_stat(&s); break;
    case PK_VERSION: gen_version(&s); break;
    case PK_CPUINFO: gen_cpuinfo(&s); break;
    case PK_MOUNTS: gen_mounts(&s); break;
    case PK_FILESYSTEMS: gen_filesystems(&s); break;
    case PK_CMDLINE_K: {
        /* Build defaults, read by user space when the matching /etc setting
         * is absent (secwm --auto: /etc/gui-autostart). root= is the root the
         * kernel mounted (ramfs or the system disk). */
        extern const char* sysroot_root_dev(void);
        sb_str(&s,"BOOT_IMAGE=/boot/kernel.bin root=");
        const char* rd = sysroot_root_dev();
        if (rd[0]=='r' && rd[1]=='a') sb_str(&s, rd); else { sb_str(&s,"/dev/"); sb_str(&s, rd); }
#ifdef SECOS_KEYMAP_DEFAULT
        sb_str(&s," secos.keymap=" SECOS_KEYMAP_DEFAULT);
#endif
#ifdef SECOS_GUI_AUTOSTART
        sb_str(&s," secos.gui_autostart=1");
#else
        sb_str(&s," secos.gui_autostart=0");
#endif
#ifdef SECOS_CONSOLE_ROOT
        sb_str(&s," secos.console=root");                /* [M43] make CONSOLE=root */
#else
        sb_str(&s," secos.console=login");
#endif
        sb_putc(&s,'\n'); break; }
    case PK_SMP: gen_smp(&s); break;
    case PK_LOCKSTAT: gen_lockstat(&s); break;
    case PK_SELF: { process_t* c=sched_get_current(); if(!c||!c->pid) return -1; sb_u64(&s,c->pid); break; }
    case PK_PID_STAT: gen_pid_stat(&s); break;
    case PK_PID_STATUS: gen_pid_status(&s); break;
    case PK_PID_STATM: gen_pid_statm(&s); break;
    case PK_PID_CMDLINE: for(int i=0;i<g_ps.cmdlen;i++) sb_putc(&s,g_ps.cmdline[i]); break;
    case PK_PID_COMM: sb_str(&s,g_ps.comm); sb_putc(&s,'\n'); break;
    case PK_PID_ENVIRON: break;
    case PK_PID_MAPS: gen_pid_maps(&s); break;
    case PK_PID_CWD: case PK_PID_EXE: case PK_PID_ROOT: case PK_PID_FD: gen_pid_link(&s,k,K_FD(key)); break;
    default: break;
    }
    g_genbuf[s.len]=0;
    return s.len;
}

static int kmsg_len(void){ uint64_t a,b; klog_window(0,&a,&b); return (int)(b-a); }

static void name_of(uint64_t key, char* out){
    int k=K_KIND(key); int o=0;
    out[0]=0;
    for(int i=0;i<NTOP;i++) if(g_top[i].kind==k){ const char* n=g_top[i].name; while(*n) out[o++]=*n++; out[o]=0; return; }
    if(k==PK_SELF){ out[0]='s'; out[1]='e'; out[2]='l'; out[3]='f'; out[4]=0; return; }
    if(k==PK_PID_DIR || k==PK_PID_FD){
        uint64_t v = (k==PK_PID_DIR) ? K_PID(key) : (uint64_t)K_FD(key);
        char d[24]; int n=0; do{ d[n++]=(char)('0'+v%10); v/=10; }while(v);
        while(n--) out[o++]=d[n];
        out[o]=0; return;
    }
    for(int i=0;i<NPIDF;i++) if(g_pidf[i].kind==k){ const char* n=g_pidf[i].name; while(*n) out[o++]=*n++; out[o]=0; return; }
}

/* Fill a node (pool slot or caller-provided) for key. Caller holds the lock. */
static void fill_node(vfs_inode_t* ino, uint64_t key, const char* path){
    int k=K_KIND(key);
    int j=0; for(; path[j] && j<255; j++) ino->path[j]=path[j];
    ino->path[j]=0;
    ino->type = is_dir(k) ? VFS_NODE_DIR : is_link(k) ? VFS_NODE_SYMLINK : VFS_NODE_FILE;
    ino->fs_data = (void*)(uintptr_t)key;
    ino->mode = is_dir(k) ? (0x4000|0555) : is_link(k) ? (0xA000|0777) : (0x8000|0444);
    ino->uid = ino->gid = 0;
    if(k>=PK_PID_DIR && snap(K_PID(key))){                 /* [M43] owned by the process's euid */
        ino->uid = g_ps.cred.euid; ino->gid = g_ps.cred.egid;
        if(k==PK_PID_ENVIRON) ino->mode = 0x8000|0400;
        if(k==PK_PID_MAPS)    ino->mode = 0x8000|0444;
        if(k==PK_PID_FDDIR)   ino->mode = 0x4000|0500;
    }
    ino->nlink = is_dir(k) ? 2 : 1;
    uint64_t now = kclock_real_sec();
    ino->atime = ino->mtime = ino->ctime = now;
    if(is_dir(k)) ino->size=0;
    else if(k==PK_KMSG) ino->size=(size_t)kmsg_len();
    else { int n=gen_key(key); ino->size = n>0 ? (size_t)n : 0; }
}

static vfs_inode_t* pool_get(uint64_t key, const char* path){
    int slot=-1;
    for(int i=0;i<64;i++) if(g_ikey[i]==key){ slot=i; break; }
    if(slot<0){ slot=g_inext; g_inext=(g_inext+1)%64; g_ikey[slot]=key; }
    fill_node(&g_ipool[slot], key, path);
    return &g_ipool[slot];
}

static int parse_u32(const char* s, int n, uint32_t* out){
    if(n<=0 || n>9) return -1;
    uint32_t v=0; for(int i=0;i<n;i++){ if(s[i]<'0'||s[i]>'9') return -1; v=v*10+(uint32_t)(s[i]-'0'); }
    *out=v; return 0;
}

/* Parse a mount-relative path into a key (0 = no such node). */
static uint64_t path_key(const char* rel){
    if(!rel||rel[0]!='/') return 0;
    const char* p=rel+1;
    if(*p==0) return KEY(PK_ROOT,0,0);
    char c1[32]; int n1=0; while(p[n1] && p[n1]!='/' && n1<31){ c1[n1]=p[n1]; n1++; } c1[n1]=0;
    if(p[n1] && p[n1]!='/') return 0;
    const char* rest = p+n1; while(*rest=='/') rest++;
    uint32_t pid=0;
    if(seq(c1,"self")){
        process_t* c=sched_get_current(); if(!c||!c->pid) return 0;
        if(!*rest) return KEY(PK_SELF,0,0);
        pid=c->pid;
    } else if(parse_u32(c1,n1,&pid)!=0){
        if(*rest) return 0;
        for(int i=0;i<NTOP;i++) if(seq(g_top[i].name,c1)) return KEY(g_top[i].kind,0,0);
        return 0;
    }
    if(!snap(pid)) return 0;
    if(!*rest) return KEY(PK_PID_DIR,pid,0);
    char c2[32]; int n2=0; while(rest[n2] && rest[n2]!='/' && n2<31){ c2[n2]=rest[n2]; n2++; } c2[n2]=0;
    if(rest[n2] && rest[n2]!='/') return 0;
    const char* r3=rest+n2; while(*r3=='/') r3++;
    for(int i=0;i<NPIDF;i++) if(seq(g_pidf[i].name,c2)){
        if(g_pidf[i].kind==PK_PID_FDDIR && *r3){
            uint32_t fd; int n3=0; while(r3[n3] && r3[n3]!='/') n3++;
            if(r3[n3] || parse_u32(r3,n3,&fd)!=0 || fd>=32 || g_ps.fd[fd].kind==FDK_NONE) return 0;
            return KEY(PK_PID_FD,pid,fd);
        }
        if(*r3) return 0;
        return KEY(g_pidf[i].kind,pid,0);
    }
    return 0;
}

/* ------------------------------------------------------------ VFS ops */
static vfs_inode_t* procfs_lookup(const char* rel){
    spin_lock(&g_proc_lock);
    uint64_t key = path_key(rel);
    vfs_inode_t* r = key ? pool_get(key, rel) : NULL;
    spin_unlock(&g_proc_lock);
    return r;
}

static int procfs_readdir(const char* dir, vfs_iter_cb cb, void* user){
    if(!cb) return -1;
    spin_lock(&g_proc_lock);
    uint64_t key = path_key(dir);
    int k = K_KIND(key);
    if(!key || !is_dir(k)){ spin_unlock(&g_proc_lock); return -1; }
    static vfs_inode_t ent;                      /* transient (callers copy) */
    static uint64_t keys[128]; int nk=0;
    if(k==PK_ROOT){
        for(int i=0;i<NTOP;i++) keys[nk++]=KEY(g_top[i].kind,0,0);
        process_t* c=sched_get_current(); if(c && c->pid) keys[nk++]=KEY(PK_SELF,0,0);
        struct pidlist l; list_pids(&l);
        for(int i=0;i<l.n && nk<128;i++) keys[nk++]=KEY(PK_PID_DIR,l.pid[i],0);
    } else if(k==PK_PID_DIR){
        for(int i=0;i<NPIDF;i++) keys[nk++]=KEY(g_pidf[i].kind,K_PID(key),0);
    } else {
        if(snap(K_PID(key)) && g_ps.inspect_ok)            /* [M43] fd/ is private */
            for(int i=0;i<32;i++) if(g_ps.fd[i].kind!=FDK_NONE) keys[nk++]=KEY(PK_PID_FD,K_PID(key),i);
    }
    /* The callbacks only copy names (getdents) or count, so they run under the
     * lock that also guards the static key/name tables. */
    static char names[128][24];
    for(int i=0;i<nk;i++) name_of(keys[i], names[i]);
    for(int i=0;i<nk;i++){
        char path[256]; int o=0;
        for(int j=0; dir[j] && o<200; j++) path[o++]=dir[j];
        if(o==0 || path[o-1]!='/') path[o++]='/';
        for(int j=0; names[i][j] && o<255; j++) path[o++]=names[i][j];
        path[o]=0;
        int kk=K_KIND(keys[i]);
        for(int j=0;j<=o;j++) ent.path[j]=path[j];
        ent.type = is_dir(kk) ? VFS_NODE_DIR : is_link(kk) ? VFS_NODE_SYMLINK : VFS_NODE_FILE;
        ent.size = 0; ent.fs_data = (void*)(uintptr_t)keys[i]; ent.ops = NULL;
        cb(&ent, user);
    }
    spin_unlock(&g_proc_lock);
    return 0;
}

static int procfs_read(vfs_inode_t* inode, size_t off, void* buf, size_t len){
    if(!inode||!inode->fs_data) return -1;
    uint64_t key=(uint64_t)(uintptr_t)inode->fs_data;
    int k=K_KIND(key);
    if(is_dir(k)) return -1;
    if(k==PK_KMSG){                              /* the log ring, snapshot window */
        uint64_t a,b; klog_window(0,&a,&b);
        if(a+off>=b) return 0;
        if(len>b-(a+off)) len=(size_t)(b-(a+off));
        return (int)klog_copy(a+off,(char*)buf,len);
    }
    spin_lock(&g_proc_lock);
    int total=gen_key(key);                      /* regenerate live */
    int r;
    if(total<0) r=-1;
    else if((int)off>=total) r=0;
    else {
        size_t avail=(size_t)total-off; if(len>avail) len=avail;
        for(size_t i=0;i<len;i++) ((uint8_t*)buf)[i]=(uint8_t)g_genbuf[off+i];
        r=(int)len;
    }
    spin_unlock(&g_proc_lock);
    return r;
}

static int procfs_readlink(const char* rel, char* buf, size_t len){
    if(!buf||len==0) return -1;
    spin_lock(&g_proc_lock);
    uint64_t key=path_key(rel);
    int r=-1;
    if(key && is_link(K_KIND(key))){
        int n=gen_key(key);
        if(n>0){
            if((size_t)n>len-1) n=(int)len-1;
            for(int i=0;i<n;i++) buf[i]=g_genbuf[i];
            buf[n]=0; r=n;
        }
    }
    spin_unlock(&g_proc_lock);
    return r;
}

static int procfs_ro_w(vfs_inode_t* a, size_t b, const void* c, size_t d){ (void)a;(void)b;(void)c;(void)d; return -1; }
static int procfs_ro_create(const char* p, const void* d, size_t s){ (void)p;(void)d;(void)s; return -1; }
static int procfs_ro_mkdir(const char* p){ (void)p; return -1; }
static int procfs_ro_remove(const char* p){ (void)p; return -1; }
static int procfs_ro_rename(const char* a, const char* b){ (void)a;(void)b; return -1; }
static int procfs_ro_trunc(const char* p, size_t s){ (void)p;(void)s; return -1; }
static int procfs_statfs(vfs_statfs_t* st){ st->type=0x9fa0; st->bsize=st->frsize=4096; st->namelen=255; return 0; }

static vfs_fs_ops_t procfs_ops = {
    .flags = VFS_FS_NOCACHE,
    .lookup = procfs_lookup,
    .readdir = procfs_readdir,
    .read = procfs_read,
    .write = procfs_ro_w,
    .create = procfs_ro_create,
    .mkdir = procfs_ro_mkdir,
    .remove = procfs_ro_remove,
    .rename = procfs_ro_rename,
    .truncate = procfs_ro_trunc,
    .readlink = procfs_readlink,
    .statfs = procfs_statfs,
};

int procfs_mount(const char* mount_point){
    extern int vfs_mount(const char*, const vfs_fs_ops_t*, const char*);
    return vfs_mount(mount_point, &procfs_ops, "procfs");
}
