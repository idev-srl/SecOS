/* SecOS - bash port compatibility shim. Provides the POSIX symbols bash
 * references that SecOS's libc lacks, mapped to SecOS syscalls or sane single-
 * user stubs (the signature is the trust boundary, so uid/gid are 0 and resource
 * limits are unlimited). SPDX-License-Identifier: MIT */
#include "bash_port.h"
#include "libsecos.h"
#include "termios.h"
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <stddef.h>

extern long secos_syscall(long,long,long,long,long,long);
static void* zmem(void* d, int c, unsigned long n){ char* p=d; while(n--) *p++=(char)c; return d; }

/* ---- process wait: POSIX 3-arg over SecOS SYS_WAIT / SYS_WAITANY ---- */
extern int errno;
static int enc_status(long code){
    return (code >= 128) ? (int)((code-128) & 0x7f)     /* WIFSIGNALED / WTERMSIG */
                         : (int)((code & 0xff) << 8);   /* WIFEXITED / WEXITSTATUS */
}
int bash_waitpid(pid_t pid, int* status, int options){
    if(pid == (pid_t)-1){                 /* wait for ANY child (bash's wait_for loop) */
        int code = 0;
        long got = secos_syscall(55 /*SYS_WAITANY*/, (long)&code, (long)options, 0,0,0);
        if(got == -10){ errno = ECHILD; return -1; }    /* no children */
        if(got == 0)   return 0;                        /* WNOHANG, none reaped yet */
        if(got < 0){ errno = EINTR; return -1; }
        if(status) *status = enc_status(code);
        return (int)got;
    }
    long code = secos_syscall(9 /*SYS_WAIT*/, pid, 0,0,0,0);
    if(code < 0){ errno = ECHILD; return -1; }
    if(status) *status = enc_status(code);
    return pid > 0 ? pid : 1;
}

/* ---- signals: map POSIX sigaction onto SecOS signal()/sigprocmask ---- */
int sigaction(int sig, const struct sigaction* act, struct sigaction* old){
    if(old) zmem(old, 0, sizeof(*old));
    if(act) signal(sig, act->sa_handler);
    return 0;
}
int sigemptyset(sigset_t* s){ if(s) *s=0; return 0; }
int sigfillset(sigset_t* s){ if(s) *s=~0UL; return 0; }
int sigaddset(sigset_t* s, int n){ if(s) *s |= (1UL<<(n&63)); return 0; }
int sigdelset(sigset_t* s, int n){ if(s) *s &= ~(1UL<<(n&63)); return 0; }
int sigismember(const sigset_t* s, int n){ return s ? (int)((*s>>(n&63))&1) : 0; }
int sigsuspend(const sigset_t* m){ (void)m; return -1; }
int siginterrupt(int s, int f){ (void)s; (void)f; return 0; }

/* ---- single-user identity database ---- */
static struct passwd g_pw = { (char*)"root", (char*)"x", 0, 0, (char*)"root", (char*)"/root", (char*)"/bin/bash" };
struct passwd* getpwnam(const char* n){ (void)n; return &g_pw; }
struct passwd* getpwuid(int u){ (void)u; return &g_pw; }
struct passwd* getpwent(void){ return 0; }
void setpwent(void){} void endpwent(void){}
static struct group g_gr = { (char*)"root", (char*)"x", 0, 0 };
struct group* getgrnam(const char* n){ (void)n; return &g_gr; }
struct group* getgrgid(int g){ (void)g; return &g_gr; }
struct group* getgrent(void){ return 0; }
void setgrent(void){} void endgrent(void){}

/* ---- resource limits: unlimited ---- */
int getrlimit(int r, struct rlimit* l){ (void)r; if(l){ l->rlim_cur=RLIM_INFINITY; l->rlim_max=RLIM_INFINITY; } return 0; }
int setrlimit(int r, const struct rlimit* l){ (void)r; (void)l; return 0; }
int getrusage(int w, struct rusage* u){ (void)w; if(u) zmem(u,0,sizeof(*u)); return 0; }

/* ---- time / clocks ---- */
clock_t times(struct tms* t){ unsigned long ms=getticks(); if(t){ t->tms_utime=ms; t->tms_stime=0; t->tms_cutime=0; t->tms_cstime=0; } return (clock_t)ms; }
unsigned int alarm(unsigned int s){ (void)s; return 0; }
int setitimer(int w, const struct itimerval* n, struct itimerval* o){ (void)w;(void)n; if(o) zmem(o,0,sizeof(*o)); return 0; }
int getitimer(int w, struct itimerval* o){ (void)w; if(o) zmem(o,0,sizeof(*o)); return 0; }
struct tm { int tm_sec,tm_min,tm_hour,tm_mday,tm_mon,tm_year,tm_wday,tm_yday,tm_isdst; };
static struct tm g_tm;
struct tm* localtime(const long* t){ (void)t; zmem(&g_tm,0,sizeof(g_tm)); g_tm.tm_year=126; return &g_tm; }
struct tm* gmtime(const long* t){ return localtime(t); }
long mktime(struct tm* t){ (void)t; return 0; }
void tzset(void){}
unsigned long strftime(char* s, unsigned long m, const char* f, const struct tm* t){ (void)f;(void)t; if(m){ s[0]=0; } return 0; }

/* ---- fd / stdio helpers ---- */
int isascii(int c){ return (c>=0 && c<128); }
int fchmod(int fd, int mode){ (void)fd;(void)mode; return 0; }
int fstat(int fd, struct stat* st){ if(!st) return -1; zmem(st,0,sizeof(*st)); st->st_mode = (fd<=2)?S_IFCHR:S_IFREG; st->st_nlink=1; return 0; }
int fileno(void* f){ return f ? *(int*)f : -1; }                 /* SecOS FILE: int fd is first member */
void clearerr(void* f){ (void)f; }
int setvbuf(void* f, char* b, int m, unsigned long s){ (void)f;(void)b;(void)m;(void)s; return 0; }
int fpurge(void* f){ (void)f; return 0; }
int getdtablesize(void){ return 32; }
int gethostname(char* n, unsigned long l){ const char* h="secos"; unsigned long i=0; for(; h[i] && i<l-1; i++) n[i]=h[i]; if(l) n[i]=0; return 0; }
char* ttyname(int fd){ return isatty(fd) ? (char*)"/dev/tty" : (char*)0; }
int eaccess(const char* p, int mode){ (void)mode; struct stat st; return stat(p,&st)==0 ? 0 : -1; }

/* ---- temp files ---- */
static unsigned long g_tmpseq = 0x1234;
static void fill_template(char* t){ char* x=t; while(*x) x++; while(x>t && *(x-1)=='X'){ x--; g_tmpseq=g_tmpseq*1103515245+12345; *x='a'+(char)(g_tmpseq%26); } }
int mkstemp(char* t){ fill_template(t); return open(t, O_RDWR|O_CREAT|O_EXCL, 0600); }
char* mktemp(char* t){ fill_template(t); return t; }
char* mkdtemp(char* t){ fill_template(t); return mkdir(t,0700)==0 ? t : (char*)0; }

/* ---- string / number ---- */
void bcopy(const void* s, void* d, unsigned long n){ const char* a=s; char* b=d; if(b<a) while(n--) *b++=*a++; else { a+=n; b+=n; while(n--) *--b=*--a; } }
char* strsignal(int s){ static char buf[24]; const char* d="Signal "; int i=0; for(; d[i]; i++) buf[i]=d[i]; if(s>=100){ buf[i++]='0'+s/100; s%=100; } if(s>=10){ buf[i++]='0'+s/10; } buf[i++]='0'+s%10; buf[i]=0; return buf; }
long double strtold(const char* s, char** e){ extern double strtod(const char*,char**); return (long double)strtod(s,e); }
unsigned long strtoumax(const char* s, char** e, int b){ extern unsigned long long strtoull(const char*,char**,int); return (unsigned long)strtoull(s,e,b); }
long strtoimax(const char* s, char** e, int b){ extern long long strtoll(const char*,char**,int); return (long)strtoll(s,e,b); }
imaxdiv_t imaxdiv(long n, long d){ imaxdiv_t r; r.quot = d? n/d : 0; r.rem = d? n%d : 0; return r; }

/* ---- dynamic loading: unsupported (enable -f) ---- */
void* dlopen(const char* p, int f){ (void)p;(void)f; return 0; }
void* dlsym(void* h, const char* s){ (void)h;(void)s; return 0; }
int dlclose(void* h){ (void)h; return 0; }
char* dlerror(void){ return (char*)"dynamic loading not supported on SecOS"; }

/* ---- locale: C locale only ---- */
int locale_mb_cur_max(void){ return 1; }
void set_default_locale(void){}
void set_default_locale_vars(void){}
void set_default_lang(void){}
int set_locale_var(const char* v, const char* val){ (void)v;(void)val; return 1; }
int set_lang(const char* v, const char* val){ (void)v;(void)val; return 1; }
char* singlequote_translations = (char*)"";

/* ---- bits from the excluded source files ---- */
int mailstat(const char* p, struct stat* st){ (void)p;(void)st; return -1; }
int isnetconn(int fd){ (void)fd; return 0; }
const char* fnx_fromfs(char* s, unsigned long n){ (void)n; return s; }
int shtimer_select(int n, void* r, void* w, void* e, void* t){ (void)n;(void)r;(void)w;(void)e;(void)t; return 0; }

/* tty helpers (lib/sh/shtty.c excluded): SecOS console is cooked-by-default */
int ttgetattr(int fd, struct termios* t){ return tcgetattr(fd, t); }
int ttsetattr(int fd, struct termios* t){ return tcsetattr(fd, 0, t); }
struct termios* ttattr(int fd){ static struct termios t; return tcgetattr(fd,&t)==0 ? &t : (struct termios*)0; }
int ttfd_cbreak(int fd, struct termios* t){ (void)fd; if(t) t->c_lflag &= ~ICANON; return 0; }
int ttfd_noecho(int fd, struct termios* t){ (void)fd; if(t) t->c_lflag &= ~ECHO; return 0; }
int ttfd_onechar(int fd, struct termios* t){ (void)fd; (void)t; return 0; }
int ttfd_eightbit(int fd, struct termios* t){ (void)fd;(void)t; return 0; }
int ttfd_nopflush(int fd, struct termios* t){ (void)fd;(void)t; return 0; }
int ttfd_nottyeof(int fd, struct termios* t){ (void)fd;(void)t; return 0; }

/* ---- bash's RNG (lib/sh/random.c excluded) ---- */
static unsigned long g_rng = 0x2545F491;
unsigned int get_urandom32(void){ g_rng = g_rng*6364136223846793005UL + 1442695040888963407UL; return (unsigned int)(g_rng>>33); }
void sbrand(unsigned long seed){ g_rng = seed ? seed : 1; }
void seedrand(void){ g_rng ^= getticks() + 0x9e3779b9; }
void seedrand32(void){ seedrand(); }
int brand(void){ g_rng = g_rng*1103515245+12345; return (int)((g_rng>>16) & 0x7fff); }
unsigned int brand32(void){ return get_urandom32(); }
void sbrand32(unsigned int s){ g_rng = s ? s : 1; }

int setresuid(int r,int e,int s){ (void)r;(void)e;(void)s; return 0; }
int setresgid(int r,int e,int s){ (void)r;(void)e;(void)s; return 0; }
int setreuid(int r,int e){ (void)r;(void)e; return 0; }
int setregid(int r,int e){ (void)r;(void)e; return 0; }
long sysconf(int name){ (void)name; return -1; }
int access(const char* p, int mode){ (void)mode; struct stat st; return stat(p,&st)==0 ? 0 : -1; }
