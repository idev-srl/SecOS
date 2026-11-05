/* SecOS libc — standard C library on top of the custom syscall ABI (libsecos).
 * Implements <ctype.h>, the new <string.h> entries, <stdlib.h>, <stdio.h>,
 * <time.h>, errno and assert. The raw syscall wrappers + strlen/puts/malloc/free
 * live in libsecos.c; this file must not redefine those.
 * SPDX-License-Identifier: MIT */
#include "libsecos.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/time.h>
#include <fcntl.h>

int errno = 0;

/* ============================== <ctype.h> =============================== */
int isdigit(int c){ return c >= '0' && c <= '9'; }
int isupper(int c){ return c >= 'A' && c <= 'Z'; }
int islower(int c){ return c >= 'a' && c <= 'z'; }
int isalpha(int c){ return isupper(c) || islower(c); }
int isalnum(int c){ return isalpha(c) || isdigit(c); }
int isspace(int c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\v'||c=='\f'; }
int isblank(int c){ return c==' '||c=='\t'; }
int isxdigit(int c){ return isdigit(c)||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }
int iscntrl(int c){ return (c>=0&&c<32)||c==127; }
int isprint(int c){ return c>=32&&c<127; }
int isgraph(int c){ return c>32&&c<127; }
int ispunct(int c){ return isgraph(c)&&!isalnum(c); }
int toupper(int c){ return islower(c) ? c-32 : c; }
int tolower(int c){ return isupper(c) ? c+32 : c; }

/* ============================== <string.h> ============================== */
void* memcpy(void* d, const void* s, size_t n){ unsigned char* a=d; const unsigned char* b=s; while(n--) *a++=*b++; return d; }
void* memmove(void* d, const void* s, size_t n){
    unsigned char* a=d; const unsigned char* b=s;
    if(a<b){ while(n--) *a++=*b++; }
    else { a+=n; b+=n; while(n--) *--a=*--b; }
    return d;
}
void* memset(void* d, int c, size_t n){ unsigned char* a=d; while(n--) *a++=(unsigned char)c; return d; }
int memcmp(const void* a, const void* b, size_t n){ const unsigned char* x=a,*y=b; while(n--){ if(*x!=*y) return *x-*y; x++; y++; } return 0; }
void* memchr(const void* s, int c, size_t n){ const unsigned char* p=s; while(n--){ if(*p==(unsigned char)c) return (void*)p; p++; } return 0; }

size_t strnlen(const char* s, size_t m){ size_t n=0; while(n<m && s[n]) n++; return n; }
int strcmp(const char* a, const char* b){ while(*a && *a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }
int strncmp(const char* a, const char* b, size_t n){ while(n && *a && *a==*b){a++;b++;n--;} return n? (unsigned char)*a-(unsigned char)*b : 0; }
int strcasecmp(const char* a, const char* b){ while(*a && tolower(*a)==tolower(*b)){a++;b++;} return tolower((unsigned char)*a)-tolower((unsigned char)*b); }
int strncasecmp(const char* a, const char* b, size_t n){ while(n && *a && tolower(*a)==tolower(*b)){a++;b++;n--;} return n? tolower((unsigned char)*a)-tolower((unsigned char)*b):0; }
char* strcpy(char* d, const char* s){ char* r=d; while((*d++=*s++)); return r; }
char* strncpy(char* d, const char* s, size_t n){ char* r=d; while(n && (*d=*s)){d++;s++;n--;} while(n--) *d++=0; return r; }
size_t strlcpy(char* d, const char* s, size_t sz){ size_t n=strlen(s); if(sz){ size_t c=n<sz-1?n:sz-1; memcpy(d,s,c); d[c]=0; } return n; }
char* strcat(char* d, const char* s){ char* r=d; while(*d)d++; while((*d++=*s++)); return r; }
char* strncat(char* d, const char* s, size_t n){ char* r=d; while(*d)d++; while(n-- && *s) *d++=*s++; *d=0; return r; }
char* strchr(const char* s, int c){ while(*s){ if(*s==(char)c) return (char*)s; s++; } return c==0?(char*)s:0; }
char* strrchr(const char* s, int c){ const char* last=0; do{ if(*s==(char)c) last=s; }while(*s++); return (char*)last; }
char* strstr(const char* h, const char* n){ if(!*n) return (char*)h; for(; *h; h++){ const char* a=h,*b=n; while(*a&&*b&&*a==*b){a++;b++;} if(!*b) return (char*)h; } return 0; }
char* strdup(const char* s){ size_t n=strlen(s)+1; char* p=malloc(n); if(p) memcpy(p,s,n); return p; }
size_t strspn(const char* s, const char* acc){ const char* p=s; while(*p && strchr(acc,*p)) p++; return p-s; }
size_t strcspn(const char* s, const char* rej){ const char* p=s; while(*p && !strchr(rej,*p)) p++; return p-s; }
char* strpbrk(const char* s, const char* acc){ for(; *s; s++) if(strchr(acc,*s)) return (char*)s; return 0; }
char* strtok(char* str, const char* delim){
    static char* save;
    if(str) save=str;
    if(!save) return 0;
    save += strspn(save, delim);
    if(!*save){ save=0; return 0; }
    char* tok=save;
    save += strcspn(save, delim);
    if(*save){ *save++=0; } else save=0;
    return tok;
}
char* strerror(int e){
    switch(e){
        case 0: return "Success";
        case ENOENT: return "No such file or directory";
        case EPERM:  return "Operation not permitted";
        case ESRCH:  return "No such process";
        case EINTR:  return "Interrupted system call";
        case EIO:    return "Input/output error";
        case ECHILD: return "No child processes";
        case EAGAIN: return "Resource temporarily unavailable";
        case ENOTTY: return "Inappropriate ioctl for device";
        case ENOTDIR:return "Not a directory";
        case EISDIR: return "Is a directory";
        case EBADF:  return "Bad file descriptor";
        case ENOMEM: return "Out of memory";
        case EACCES: return "Permission denied";
        case EEXIST: return "File exists";
        case EINVAL: return "Invalid argument";
        case ENOSPC: return "No space left on device";
        case EPIPE:  return "Broken pipe";
        case ENOSYS: return "Function not implemented";
        case EFAULT: return "Bad address";
        case EBUSY:  return "Device or resource busy";
        case EXDEV:  return "Invalid cross-device link";
        case EROFS:  return "Read-only file system";
        case ENAMETOOLONG: return "File name too long";
        case ENOTEMPTY: return "Directory not empty";
        case ELOOP:  return "Too many levels of symbolic links";
        case ERANGE: return "Numerical result out of range";
        case ETIMEDOUT: return "Connection timed out";
        default:     return "Unknown error";
    }
}

/* ============================== <stdlib.h> ============================== */
/* First-fit allocator over sbrk (moved here from libsecos.c so realloc can read
 * the block size). Header precedes each payload; the free list is singly linked. */
typedef struct mblock { size_t size; struct mblock* next; int is_free; } mblock_t;
static mblock_t* g_heap = 0;

void* malloc(size_t size){
    if(!size) return 0;
    size = (size + 7UL) & ~7UL;
    mblock_t* prev=0; mblock_t* b=g_heap;
    while(b){ if(b->is_free && b->size>=size){ b->is_free=0; return (void*)(b+1); } prev=b; b=b->next; }
    mblock_t* nb = (mblock_t*)sbrk((long)(sizeof(mblock_t)+size));
    if((long)nb==-1L) return 0;
    nb->size=size; nb->is_free=0; nb->next=0;
    if(prev) prev->next=nb; else g_heap=nb;
    return (void*)(nb+1);
}
void free(void* p){ if(!p) return; mblock_t* b=(mblock_t*)p-1; b->is_free=1; }
void* calloc(size_t nm, size_t sz){ size_t n=nm*sz; if(sz && n/sz!=nm) return 0; void* p=malloc(n); if(p) memset(p,0,n); return p; }
void* realloc(void* p, size_t sz){
    if(!p) return malloc(sz);
    if(!sz){ free(p); return 0; }
    size_t old = ((mblock_t*)p - 1)->size;
    if(sz <= old) return p;                  /* shrink/fit in place */
    void* n = malloc(sz);
    if(!n) return 0;
    memcpy(n, p, old);
    free(p);
    return n;
}
int abs(int x){ return x<0?-x:x; }
long labs(long x){ return x<0?-x:x; }

long strtol(const char* s, char** end, int base){
    const char* p=s; while(isspace(*p)) p++;
    int neg=0; if(*p=='+'||*p=='-'){ neg=(*p=='-'); p++; }
    if((base==0||base==16) && p[0]=='0' && (p[1]=='x'||p[1]=='X')){ p+=2; base=16; }
    else if(base==0 && p[0]=='0'){ base=8; }
    else if(base==0){ base=10; }
    long v=0;
    for(;;){
        int c=*p, d;
        if(isdigit(c)) d=c-'0';
        else if(isalpha(c)) d=tolower(c)-'a'+10;
        else break;
        if(d>=base) break;
        v=v*base+d; p++;
    }
    if(end) *end=(char*)p;
    return neg?-v:v;
}
unsigned long strtoul(const char* s, char** end, int base){ return (unsigned long)strtol(s,end,base); }
int atoi(const char* s){ return (int)strtol(s,0,10); }
long atol(const char* s){ return strtol(s,0,10); }

/* strtod: decimal floating point (integer mantissa + single 10^exp scale).
 * Handles sign, fraction and e/E exponent. Good enough to round-trip the numbers
 * SecOS's %g printf produces (used by lua's lexer). */
double strtod(const char* s, char** end){
    const char* s0=s;
    while(isspace((unsigned char)*s)) s++;
    int neg=0; if(*s=='+'||*s=='-'){ neg=(*s=='-'); s++; }
    unsigned long long mant=0; int mdig=0, exp10=0, any=0;
    while(isdigit((unsigned char)*s)){ if(mdig<18){ mant=mant*10+(unsigned)(*s-'0'); mdig++; } else exp10++; s++; any=1; }
    if(*s=='.'){ s++; while(isdigit((unsigned char)*s)){ if(mdig<18){ mant=mant*10+(unsigned)(*s-'0'); mdig++; exp10--; } s++; any=1; } }
    if(!any){ if(end)*end=(char*)s0; return 0.0; }
    if(*s=='e'||*s=='E'){ const char* sv=s; s++; int eneg=0; if(*s=='+'||*s=='-'){ eneg=(*s=='-'); s++; }
        if(isdigit((unsigned char)*s)){ int ex=0; while(isdigit((unsigned char)*s)){ ex=ex*10+(*s-'0'); s++; } exp10 += eneg?-ex:ex; }
        else s=sv; }
    double val=(double)mant;
    if(exp10>0){ for(int i=0;i<exp10;i++) val*=10.0; }
    else if(exp10<0){ for(int i=0;i<-exp10;i++) val/=10.0; }
    if(end)*end=(char*)s;
    return neg? -val : val;
}
float strtof(const char* s, char** end){ return (float)strtod(s,end); }
double atof(const char* s){ return strtod(s,0); }
long long strtoll(const char* s, char** end, int base){ return (long long)strtol(s,end,base); }
unsigned long long strtoull(const char* s, char** end, int base){ return (unsigned long long)strtoul(s,end,base); }

static unsigned long g_rand = 123456789UL;
int rand(void){ g_rand = g_rand*1103515245UL + 12345UL; return (int)((g_rand>>16) & RAND_MAX); }
void srand(unsigned seed){ g_rand = seed; }

/* simple insertion sort (stable enough, fine for small userland sets) */
void qsort(void* base, size_t n, size_t sz, int(*cmp)(const void*,const void*)){
    char* a=base; char tmp[256];
    if(sz>sizeof(tmp)) return;            /* bounded scratch; userland sorts are small */
    for(size_t i=1;i<n;i++){
        memcpy(tmp, a+i*sz, sz);
        size_t j=i;
        while(j>0 && cmp(a+(j-1)*sz, tmp)>0){ memcpy(a+j*sz, a+(j-1)*sz, sz); j--; }
        memcpy(a+j*sz, tmp, sz);
    }
}
void* bsearch(const void* key, const void* base, size_t n, size_t sz, int(*cmp)(const void*,const void*)){
    const char* a=base; size_t lo=0, hi=n;
    while(lo<hi){ size_t m=(lo+hi)/2; int r=cmp(key, a+m*sz); if(r<0) hi=m; else if(r>0) lo=m+1; else return (void*)(a+m*sz); }
    return 0;
}
/* [M39] In-process environment. environ is a NULL-terminated array of "KEY=VAL"
 * strings, seeded at startup from the envp execve() handed us (__libc_init_env,
 * called by crt0; a spawned program starts empty).
 * getenv/setenv/unsetenv/putenv operate on it. Bounded (64 vars, static storage)
 * — enough for a shell's own variables. */
#define ENV_MAX 64
#define ENV_LEN 256
static char  env_store[ENV_MAX][ENV_LEN];
static char* env_ptrs[ENV_MAX + 1];
char** environ = env_ptrs;

/* crt0: copy the initial environment (kernel-built on our stack) into env_store
 * so setenv's in-place rewrite never runs off the end of a stack string. */
void __libc_init_env(char** envp){
    int n = 0;
    for(int i=0; envp && envp[i] && n < ENV_MAX; i++){
        const char* src = envp[i]; size_t k = 0;
        while(src[k] && k < ENV_LEN-1){ env_store[n][k] = src[k]; k++; }
        env_store[n][k] = 0;
        env_ptrs[n] = env_store[n]; n++;
    }
    env_ptrs[n] = 0;
}

static int env_name_eq(const char* entry, const char* name, size_t nlen){
    for(size_t i=0;i<nlen;i++) if(entry[i]!=name[i]) return 0;
    return entry[nlen]=='=';
}
char* getenv(const char* name){
    if(!name) return 0;
    size_t nlen=0; while(name[nlen]) nlen++;
    for(int i=0; environ[i]; i++)
        if(env_name_eq(environ[i], name, nlen)) return environ[i] + nlen + 1;
    return 0;
}
int setenv(const char* name, const char* value, int overwrite){
    if(!name || !*name) return -1;
    size_t nlen=0; while(name[nlen]) nlen++;
    int n=0; for(; environ[n]; n++) if(env_name_eq(environ[n], name, nlen)){
        if(!overwrite) return 0;
        char* e=environ[n]; size_t k=nlen+1; e[k-1]='=';
        for(size_t i=0; value[i] && k<ENV_LEN-1; i++) e[k++]=value[i]; e[k]=0;
        return 0;
    }
    if(n>=ENV_MAX) return -1;
    /* A storage slot no live entry uses (unsetenv shifts environ[], so slot n
     * may still back another variable). */
    int slot=-1;
    for(int st=0; st<ENV_MAX && slot<0; st++){
        int busy=0; for(int j=0; environ[j]; j++) if(environ[j]==env_store[st]){ busy=1; break; }
        if(!busy) slot=st;
    }
    if(slot<0) return -1;
    char* e=env_store[slot]; size_t k=0;
    for(size_t i=0; name[i] && k<ENV_LEN-1; i++) e[k++]=name[i];
    if(k<ENV_LEN-1) e[k++]='=';
    for(size_t i=0; value[i] && k<ENV_LEN-1; i++) e[k++]=value[i]; e[k]=0;
    environ[n]=e; environ[n+1]=0;
    return 0;
}
int unsetenv(const char* name){
    if(!name) return -1;
    size_t nlen=0; while(name[nlen]) nlen++;
    for(int i=0; environ[i]; i++) if(env_name_eq(environ[i], name, nlen)){
        int j=i; while(environ[j+1]){ environ[j]=environ[j+1]; j++; } environ[j]=0;
        return 0;
    }
    return 0;
}
int putenv(char* s){
    if(!s) return -1;
    size_t eq=0; while(s[eq] && s[eq]!='=') eq++;
    if(s[eq]!='=') return -1;
    char name[ENV_LEN]; size_t i=0; for(; i<eq && i<ENV_LEN-1; i++) name[i]=s[i]; name[i]=0;
    return setenv(name, s+eq+1, 1);
}
void abort(void){ write(2,"abort\n",6); _exit(134); }

/* [M39] Single-user identity + misc POSIX stubs that source ports (a shell, many
 * utilities) reference. SecOS's trust model is the signature, not uid — so every
 * signed process runs as uid/gid 0 (root). These are correct for that model. */
int getuid(void){ return 0; }
int geteuid(void){ return 0; }
int getgid(void){ return 0; }
int getegid(void){ return 0; }
int setuid(int u){ (void)u; return 0; }
int setgid(int g){ (void)g; return 0; }
int setgroups(unsigned n, const int* g){ (void)n; (void)g; return 0; }
int getgroups(int n, int* g){ (void)n; (void)g; return 0; }
/* umask() is the kernel's (SYS_UMASK, libsecos.c): it applies to open/mkdir. */

/* Wall clock (CLOCK_REALTIME = RTC anchored to the TSC, UTC). */
int gettimeofday(struct timeval* tv, void* tz){
    if(tz){ struct timezone* z = (struct timezone*)tz; z->tz_minuteswest = 0; z->tz_dsttime = 0; }
    if(!tv) return 0;
    struct timespec ts;
    if(clock_gettime(CLOCK_REALTIME, &ts) != 0) return -1;
    tv->tv_sec  = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}
int settimeofday(const struct timeval* tv, const struct timezone* tz){
    (void)tz;
    if(!tv) return 0;
    if(tv->tv_usec < 0 || tv->tv_usec >= 1000000){ errno = EINVAL; return -1; }
    struct timespec ts = { tv->tv_sec, tv->tv_usec * 1000 };
    return clock_settime(CLOCK_REALTIME, &ts);
}

/* fcntl: kernel-backed (SYS_FCNTL): F_DUPFD / F_DUPFD_CLOEXEC honour the minimum
 * fd, F_GETFD / F_SETFD carry FD_CLOEXEC (closed by execve), F_GETFL / F_SETFL
 * the status flags. Record-locking commands are accepted as no-ops. */
int fcntl(int fd, int cmd, ...){
    va_list ap; va_start(ap, cmd);
    long arg = va_arg(ap, long); va_end(ap);
    if(cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW) return 0;
    long r = secos_syscall(61 /*SYS_FCNTL*/, fd, cmd, arg, 0, 0);
    if(r < 0){ errno = (int)-r; return -1; }
    return (int)r;
}

/* execl family: collect the variadic argv (bounded) and call execve/execvp. */
#define EXECL_MAX 256
static int execl_collect(const char* arg, va_list* ap, char** av){
    int n = 0;
    av[n++] = (char*)arg;
    while(arg){
        arg = va_arg(*ap, const char*);
        if(n >= EXECL_MAX){ errno = E2BIG; return -1; }
        av[n++] = (char*)arg;
    }
    return 0;
}
int execl(const char* path, const char* arg, ...){
    char* av[EXECL_MAX + 1]; va_list ap; va_start(ap, arg);
    int r = execl_collect(arg, &ap, av); va_end(ap);
    return r < 0 ? -1 : execve(path, av, environ);
}
int execlp(const char* file, const char* arg, ...){
    char* av[EXECL_MAX + 1]; va_list ap; va_start(ap, arg);
    int r = execl_collect(arg, &ap, av); va_end(ap);
    return r < 0 ? -1 : execvp(file, av);
}
int execle(const char* path, const char* arg, ...){
    char* av[EXECL_MAX + 1]; va_list ap; va_start(ap, arg);
    int r = execl_collect(arg, &ap, av);
    char* const* envp = r < 0 ? 0 : va_arg(ap, char* const*);
    va_end(ap);
    return r < 0 ? -1 : execve(path, av, envp);
}

/* ============================== <dirent.h> ============================= */
#include <dirent.h>
DIR* opendir(const char* path){
    DIR* d = malloc(sizeof(DIR)); if(!d) return 0;
    d->buf = malloc(16384);
    if(!d->buf){ free(d); return 0; }
    long n = getdents(path, d->buf, 16384);
    if(n < 0){ free(d->buf); free(d); return 0; }
    d->len = (int)n; d->pos = 0;
    return d;
}
struct dirent* readdir(DIR* d){
    if(!d || d->pos + 256 > d->len) return 0;
    char* rec = d->buf + d->pos; d->pos += 256;
    char t = rec[0];
    d->ent.d_type = t=='d'?DT_DIR : t=='l'?DT_LNK : DT_REG;
    strlcpy(d->ent.d_name, rec+1, sizeof(d->ent.d_name));
    return &d->ent;
}
int closedir(DIR* d){ if(d){ free(d->buf); free(d); } return 0; }

/* ============================== <time.h> =============================== */
/* Real Unix time (UTC). Broken-down time is always UTC: there is no time-zone
 * database, so localtime == gmtime and mktime == timegm. */
time_t time(time_t* t){
    struct timespec ts; time_t s = (clock_gettime(CLOCK_REALTIME, &ts) == 0) ? ts.tv_sec : (time_t)-1;
    if(t) *t = s;
    return s;
}
clock_t clock(void){
    struct timespec ts;
    if(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return (clock_t)-1;
    return (clock_t)(ts.tv_sec * CLOCKS_PER_SEC + ts.tv_nsec / (1000000000 / CLOCKS_PER_SEC));
}
double difftime(time_t a, time_t b){ return (double)(a - b); }
int timespec_get(struct timespec* ts, int base){ return (base == TIME_UTC && clock_gettime(CLOCK_REALTIME, ts) == 0) ? base : 0; }
int clock_nanosleep(clockid_t clk, int flags, const struct timespec* req, struct timespec* rem){
    struct timespec rel = *req;
    if(flags & TIMER_ABSTIME){
        struct timespec now;
        if(clock_gettime(clk, &now) != 0) return EINVAL;
        rel.tv_sec = req->tv_sec - now.tv_sec; rel.tv_nsec = req->tv_nsec - now.tv_nsec;
        if(rel.tv_nsec < 0){ rel.tv_nsec += 1000000000; rel.tv_sec--; }
        if(rel.tv_sec < 0) return 0;
        rem = 0;
    }
    return nanosleep(&rel, rem) == 0 ? 0 : errno;    /* POSIX: returns the error */
}
long  timezone = 0;
int   daylight = 0;
char* tzname[2] = { (char*)"UTC", (char*)"UTC" };
void tzset(void){}

/* Days since 1970-01-01 <-> civil date (H. Hinnant's algorithms). */
static long days_from_civil(long y, unsigned m, unsigned d){
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
struct tm* gmtime_r(const time_t* t, struct tm* r){
    long s = (long)*t, days = s / 86400, rem = s % 86400;
    if(rem < 0){ rem += 86400; days--; }
    r->tm_hour = (int)(rem / 3600); r->tm_min = (int)(rem % 3600 / 60); r->tm_sec = (int)(rem % 60);
    r->tm_wday = (int)(((days % 7) + 11) % 7);       /* 1970-01-01 was a Thursday */
    long z = days + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    r->tm_year = (int)(y - 1900); r->tm_mon = (int)m - 1; r->tm_mday = (int)d;
    r->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    r->tm_isdst = 0; r->tm_gmtoff = 0; r->tm_zone = "UTC";
    return r;
}
struct tm* localtime_r(const time_t* t, struct tm* r){ return gmtime_r(t, r); }
struct tm* gmtime(const time_t* t){ static struct tm r; return gmtime_r(t, &r); }
struct tm* localtime(const time_t* t){ return gmtime(t); }
time_t timegm(struct tm* tm){
    long y = tm->tm_year + 1900L, mon = tm->tm_mon;
    y += mon / 12; mon %= 12; if(mon < 0){ mon += 12; y--; }
    long days = days_from_civil(y, (unsigned)mon + 1, 1) + tm->tm_mday - 1;
    time_t t = (time_t)(days * 86400 + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec);
    gmtime_r(&t, tm);                                /* normalize the fields */
    return t;
}
time_t mktime(struct tm* tm){ return timegm(tm); }

static const char* const g_wday[] = { "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday" };
static const char* const g_mon[]  = { "January","February","March","April","May","June","July",
                                      "August","September","October","November","December" };
static int is_leap(int y){ return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
/* ISO 8601 week number and week-based year (for %G %g %V). */
static int iso_week(const struct tm* tm, int* year){
    int y = tm->tm_year + 1900;
    int wday = (tm->tm_wday + 6) % 7;                  /* Monday = 0 */
    int wk = (tm->tm_yday - wday + 10) / 7;
    if(wk < 1){
        y--;
        int yday = tm->tm_yday + 365 + is_leap(y);
        wk = (yday - wday + 10) / 7;
    } else if(wk == 53 && tm->tm_yday - wday + 3 >= 365 + is_leap(y)){
        wk = 1; y++;                                   /* belongs to week 1 of next year */
    }
    *year = y;
    return wk;
}
size_t strftime(char* s, size_t max, const char* fmt, const struct tm* tm){
    size_t n = 0;
    char tmp[64];
    if(max == 0) return 0;
    for(; *fmt; fmt++){
        const char* add = tmp;
        tmp[0] = 0;
        if(*fmt != '%'){ tmp[0] = *fmt; tmp[1] = 0; }
        else {
            fmt++;
            if(*fmt == 'E' || *fmt == 'O') fmt++;       /* locale modifiers: ignored */
            int h12 = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
            int iy;
            switch(*fmt){
            case 'a': snprintf(tmp, sizeof tmp, "%.3s", g_wday[tm->tm_wday % 7]); break;
            case 'A': add = g_wday[tm->tm_wday % 7]; break;
            case 'b': case 'h': snprintf(tmp, sizeof tmp, "%.3s", g_mon[tm->tm_mon % 12]); break;
            case 'B': add = g_mon[tm->tm_mon % 12]; break;
            case 'c': snprintf(tmp, sizeof tmp, "%.3s %.3s %2d %02d:%02d:%02d %d", g_wday[tm->tm_wday % 7], g_mon[tm->tm_mon % 12],
                               tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900); break;
            case 'C': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_year + 1900) / 100); break;
            case 'd': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mday); break;
            case 'D': case 'x': snprintf(tmp, sizeof tmp, "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, (tm->tm_year + 1900) % 100); break;
            case 'e': snprintf(tmp, sizeof tmp, "%2d", tm->tm_mday); break;
            case 'F': snprintf(tmp, sizeof tmp, "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
            case 'G': iso_week(tm, &iy); snprintf(tmp, sizeof tmp, "%d", iy); break;
            case 'g': iso_week(tm, &iy); snprintf(tmp, sizeof tmp, "%02d", iy % 100); break;
            case 'H': snprintf(tmp, sizeof tmp, "%02d", tm->tm_hour); break;
            case 'I': snprintf(tmp, sizeof tmp, "%02d", h12); break;
            case 'j': snprintf(tmp, sizeof tmp, "%03d", tm->tm_yday + 1); break;
            case 'k': snprintf(tmp, sizeof tmp, "%2d", tm->tm_hour); break;
            case 'l': snprintf(tmp, sizeof tmp, "%2d", h12); break;
            case 'm': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mon + 1); break;
            case 'M': snprintf(tmp, sizeof tmp, "%02d", tm->tm_min); break;
            case 'n': add = "\n"; break;
            case 'p': add = tm->tm_hour < 12 ? "AM" : "PM"; break;
            case 'P': add = tm->tm_hour < 12 ? "am" : "pm"; break;
            case 'r': snprintf(tmp, sizeof tmp, "%02d:%02d:%02d %s", h12, tm->tm_min, tm->tm_sec, tm->tm_hour < 12 ? "AM" : "PM"); break;
            case 'R': snprintf(tmp, sizeof tmp, "%02d:%02d", tm->tm_hour, tm->tm_min); break;
            case 's': { struct tm c = *tm; snprintf(tmp, sizeof tmp, "%ld", (long)timegm(&c)); break; }
            case 'S': snprintf(tmp, sizeof tmp, "%02d", tm->tm_sec); break;
            case 't': add = "\t"; break;
            case 'T': case 'X': snprintf(tmp, sizeof tmp, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
            case 'u': snprintf(tmp, sizeof tmp, "%d", tm->tm_wday ? tm->tm_wday : 7); break;
            case 'U': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_yday + 7 - tm->tm_wday) / 7); break;
            case 'V': snprintf(tmp, sizeof tmp, "%02d", iso_week(tm, &iy)); break;
            case 'w': snprintf(tmp, sizeof tmp, "%d", tm->tm_wday); break;
            case 'W': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7); break;
            case 'y': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_year + 1900) % 100); break;
            case 'Y': snprintf(tmp, sizeof tmp, "%d", tm->tm_year + 1900); break;
            case 'z': add = "+0000"; break;
            case 'Z': add = "UTC"; break;
            case '%': add = "%"; break;
            case 0: fmt--; add = ""; break;
            default: tmp[0] = '%'; tmp[1] = *fmt; tmp[2] = 0; break;
            }
        }
        for(; *add; add++){ if(n + 1 >= max) return 0; s[n++] = *add; }
    }
    s[n] = 0;
    return n;
}
char* asctime_r(const struct tm* tm, char* buf){
    snprintf(buf, 26, "%.3s %.3s %2d %02d:%02d:%02d %d\n", g_wday[tm->tm_wday % 7], g_mon[tm->tm_mon % 12],
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
    return buf;
}
char* asctime(const struct tm* tm){ static char b[32]; return asctime_r(tm, b); }
char* ctime_r(const time_t* t, char* buf){ struct tm r; return asctime_r(gmtime_r(t, &r), buf); }
char* ctime(const time_t* t){ static char b[32]; return ctime_r(t, b); }

/* <utime.h> */
#include <utime.h>
int utime(const char* path, const struct utimbuf* times){
    time_t a, m;
    if(times){ a = times->actime; m = times->modtime; } else { a = m = time(0); }
    return utimes(path, (unsigned long)a, (unsigned long)m);
}

/* ===================== <poll.h> / <sys/select.h> ======================= */
#include <poll.h>
#include <sys/select.h>
int ppoll(struct pollfd* fds, nfds_t n, const struct timespec* tmo, const unsigned long* sigmask){
    int ms = -1;
    if(tmo) ms = (int)(tmo->tv_sec * 1000 + (tmo->tv_nsec + 999999) / 1000000);
    unsigned long old = 0;
    if(sigmask) sigprocmask(2 /*SIG_SETMASK*/, sigmask, &old);
    int r = poll(fds, n, ms);
    int e = errno;
    if(sigmask) sigprocmask(2, &old, 0);
    errno = e;
    return r;
}
/* select() over poll(): one pollfd per descriptor set in any of the sets (the
 * kernel polls <= 64 descriptors per call). Exceptional conditions map to
 * POLLPRI, which no SecOS descriptor raises. */
static int select_ms(int nfds, fd_set* r, fd_set* w, fd_set* e, int ms){
    struct pollfd pf[64];
    int n = 0;
    if(nfds < 0 || nfds > FD_SETSIZE){ errno = EINVAL; return -1; }
    for(int fd = 0; fd < nfds; fd++){
        short ev = 0;
        if(r && FD_ISSET(fd, r)) ev |= POLLIN;
        if(w && FD_ISSET(fd, w)) ev |= POLLOUT;
        if(e && FD_ISSET(fd, e)) ev |= POLLPRI;
        if(!ev) continue;
        if(n == 64){ errno = EINVAL; return -1; }
        pf[n].fd = fd; pf[n].events = ev; pf[n].revents = 0; n++;
    }
    if(poll(pf, (nfds_t)n, ms) < 0) return -1;
    for(int i = 0; i < n; i++) if(pf[i].revents & POLLNVAL){ errno = EBADF; return -1; }
    if(r) FD_ZERO(r);
    if(w) FD_ZERO(w);
    if(e) FD_ZERO(e);
    int cnt = 0;
    for(int i = 0; i < n; i++){
        short re = pf[i].revents;
        if(r && (pf[i].events & POLLIN) && (re & (POLLIN | POLLHUP | POLLERR))){ FD_SET(pf[i].fd, r); cnt++; }
        if(w && (pf[i].events & POLLOUT) && (re & (POLLOUT | POLLERR))){ FD_SET(pf[i].fd, w); cnt++; }
        if(e && (re & POLLPRI)){ FD_SET(pf[i].fd, e); cnt++; }
    }
    return cnt;
}
int select(int nfds, fd_set* r, fd_set* w, fd_set* e, struct timeval* tmo){
    int ms = -1;
    struct timespec t0 = { 0, 0 };
    if(tmo){
        if(tmo->tv_sec < 0 || tmo->tv_usec < 0){ errno = EINVAL; return -1; }
        ms = (int)(tmo->tv_sec * 1000 + (tmo->tv_usec + 999) / 1000);
        clock_gettime(CLOCK_MONOTONIC, &t0);
    }
    int rv = select_ms(nfds, r, w, e, ms);
    if(tmo && rv >= 0){                                  /* Linux: timeout -> time left */
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        long used = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
        long left = tmo->tv_sec * 1000000L + tmo->tv_usec - used;
        if(left < 0 || rv == 0) left = 0;
        tmo->tv_sec = left / 1000000; tmo->tv_usec = left % 1000000;
    }
    return rv;
}
int pselect(int nfds, fd_set* r, fd_set* w, fd_set* e, const struct timespec* tmo, const unsigned long* sigmask){
    int ms = -1;
    if(tmo) ms = (int)(tmo->tv_sec * 1000 + (tmo->tv_nsec + 999999) / 1000000);
    unsigned long old = 0;
    if(sigmask) sigprocmask(2, sigmask, &old);
    int rv = select_ms(nfds, r, w, e, ms);
    int er = errno;
    if(sigmask) sigprocmask(2, &old, 0);
    errno = er;
    return rv;
}

/* ===================== <sys/uio.h> / <sys/statvfs.h> =================== */
#include <sys/uio.h>
#include <sys/statvfs.h>
#include <sys/statfs.h>
#include <sys/sysinfo.h>
long readv(int fd, const struct iovec* iov, int cnt){
    long tot = 0;
    if(cnt < 0){ errno = EINVAL; return -1; }
    for(int i = 0; i < cnt; i++){
        if(!iov[i].iov_len) continue;
        long r = read(fd, iov[i].iov_base, iov[i].iov_len);
        if(r < 0) return tot ? tot : -1;
        tot += r;
        if((size_t)r < iov[i].iov_len) break;            /* short read: stop here */
    }
    return tot;
}
long writev(int fd, const struct iovec* iov, int cnt){
    long tot = 0;
    if(cnt < 0){ errno = EINVAL; return -1; }
    for(int i = 0; i < cnt; i++){
        if(!iov[i].iov_len) continue;
        long r = write(fd, iov[i].iov_base, iov[i].iov_len);
        if(r < 0) return tot ? tot : -1;
        tot += r;
        if((size_t)r < iov[i].iov_len) break;
    }
    return tot;
}
static void statfs_to_vfs(const struct statfs* s, struct statvfs* v){
    v->f_bsize = (unsigned long)s->f_bsize; v->f_frsize = (unsigned long)(s->f_frsize ? s->f_frsize : s->f_bsize);
    v->f_blocks = s->f_blocks; v->f_bfree = s->f_bfree; v->f_bavail = s->f_bavail;
    v->f_files = s->f_files; v->f_ffree = s->f_ffree; v->f_favail = s->f_ffree;
    v->f_fsid = (unsigned long)(unsigned)s->f_fsid.__val[0]; v->f_flag = (unsigned long)s->f_flags & 1;
    v->f_namemax = (unsigned long)s->f_namelen;
}
int statvfs(const char* path, struct statvfs* v){ struct statfs s; if(statfs(path, &s) != 0) return -1; statfs_to_vfs(&s, v); return 0; }
int fstatvfs(int fd, struct statvfs* v){ struct statfs s; if(fstatfs(fd, &s) != 0) return -1; statfs_to_vfs(&s, v); return 0; }
int get_nprocs(void){
    int fd = open("/proc/stat", O_RDONLY), n = 0;
    if(fd < 0) return 1;
    char b[1024]; long r = read(fd, b, sizeof(b) - 1); close(fd);
    if(r <= 0) return 1;
    b[r] = 0;
    for(char* p = b; (p = strstr(p, "\ncpu")) != 0; p++) n++;
    return n ? n : 1;
}
int get_nprocs_conf(void){ return get_nprocs(); }

/* ============================== <assert.h> ============================= */
void __assert_fail(const char* expr, const char* file, int line){
    char buf[256]; int n=snprintf(buf,sizeof buf,"assert failed: %s (%s:%d)\n",expr,file,line);
    write(2,buf,n); _exit(134);
}

/* ============================== <unistd.h> extras ====================== */
unsigned int sleep(unsigned int s){
    struct timespec rq = { (long)s, 0 }, rm = { 0, 0 };
    if(nanosleep(&rq, &rm) == 0) return 0;
    return (unsigned)rm.tv_sec + (rm.tv_nsec ? 1 : 0);    /* interrupted: seconds left */
}
int usleep(unsigned long us){
    struct timespec rq = { (long)(us / 1000000), (long)(us % 1000000) * 1000 };
    return nanosleep(&rq, 0);
}
/* A terminal is whatever answers TCGETS (the console and its dups, /dev/tty);
 * a redirected stdio fd (pipe, file) is not. */
int ioctl(int fd, unsigned long request, void* arg);
int isatty(int fd){ int t[9]; return ioctl(fd, 0x5401 /*TCGETS*/, t) == 0; }

/* ============================ printf core ============================== */
/* A sink-based formatter so printf/fprintf stream directly and snprintf bounds. */
typedef struct { void (*put)(void*, const char*, size_t); void* arg; int count; } sink_t;
static void emit(sink_t* s, const char* p, size_t n){ s->put(s->arg, p, n); s->count += (int)n; }
static void emitc(sink_t* s, char c){ s->put(s->arg, &c, 1); s->count++; }

/* ===== floating point formatting (%f/%e/%g) — hardware double via SSE =====
 * dtoa_round extracts `nsig` significant decimal digits with a single exact
 * power-of-ten scale (≈0.5 ULP, no per-digit accumulation); verified against
 * glibc printf for the lua-relevant %.14g cases. Not bit-exact for the
 * round-half-to-even tie case (we round half away from zero). */
static int dtoa_round(double v, int nsig, char* digs){
    if(nsig<1) nsig=1;
    if(nsig>17) nsig=17;
    if(v<=0){ for(int i=0;i<nsig;i++) digs[i]='0'; return 0; }
    int e=0; { double t=v; while(t>=1e16){t/=1e16;e+=16;} while(t>=10.0){t/=10.0;e++;} while(t<1.0){t*=10.0;e--;} }
    int k=nsig-1-e; int ap=k<0?-k:k; double p=1.0; for(int i=0;i<ap;i++) p*=10.0;
    double scaled=(k>=0)? v*p : v/p;
    unsigned long long m=(unsigned long long)(scaled+0.5);
    char tmp[24]; int tn=0; if(m==0) tmp[tn++]='0'; while(m){ tmp[tn++]=(char)('0'+(int)(m%10)); m/=10; }
    if(tn>nsig) e++;
    int start=tn-1; for(int i=0;i<nsig;i++){ int idx=start-i; digs[i]=(idx>=0)?tmp[idx]:'0'; }
    return e;
}
static int fmt_fp_body(char* out, double v, char conv, int prec, int alt){
    char digs[40]; int n=0; if(prec<0) prec=6;
    if(conv=='e'){
        int nsig=prec+1; if(nsig<1) nsig=1; int e=dtoa_round(v,nsig,digs);
        out[n++]=digs[0]; if(prec>0||alt){ out[n++]='.'; for(int i=0;i<prec;i++) out[n++]=(i+1<nsig)?digs[i+1]:'0'; }
        out[n++]='e'; out[n++]=e<0?'-':'+'; int ae=e<0?-e:e; char eb[6]; int en=0;
        if(ae==0) eb[en++]='0';
        while(ae){ eb[en++]='0'+ae%10; ae/=10; }
        if(en<2) eb[en++]='0';
        while(en) out[n++]=eb[--en];
        return n;
    }
    if(conv=='f'){
        int e0=0; { double t=v; if(t>0){ while(t>=1e16){t/=1e16;e0+=16;} while(t>=10.0){t/=10.0;e0++;} while(t<1.0){t*=10.0;e0--;} } }
        int intdigits=e0>=0?e0+1:0; int nsig=intdigits+prec; if(nsig<1) nsig=1; if(nsig>17) nsig=17;
        int e=dtoa_round(v,nsig,digs); if(v==0.0) e=0; int di=0;
        if(e>=0){ for(int i=0;i<=e;i++) out[n++]=(di<nsig)?digs[di++]:'0'; } else out[n++]='0';
        if(prec>0||alt){ out[n++]='.'; int fc=0;
            if(e<0){ for(int i=0;i<(-e-1)&&fc<prec;i++){ out[n++]='0'; fc++; } }
            while(fc<prec){ out[n++]=(di<nsig)?digs[di++]:'0'; fc++; } }
        return n;
    }
    /* %g: shortest of %e/%f with P significant digits, trailing zeros stripped */
    { int P=(prec<0)?6:(prec==0?1:prec); if(v==0.0) return fmt_fp_body(out,0.0,'f',alt?P-1:0,alt);
      int X=dtoa_round(v,P,digs); int useF=(X>=-4 && X<P);
      int body=useF? fmt_fp_body(out,v,'f',P-1-X,alt) : fmt_fp_body(out,v,'e',P-1,alt);
      if(!alt){ int epos=-1; for(int i=0;i<body;i++) if(out[i]=='e'){ epos=i; break; }
        int end=(epos>=0)?epos:body; int hd=0; for(int i=0;i<end;i++) if(out[i]=='.'){ hd=1; break; }
        if(hd){ int j=end-1; while(j>=0&&out[j]=='0') j--; if(j>=0&&out[j]=='.') j--;
          int cut=end-1-j; if(cut>0){ for(int i=j+1;i<body-cut;i++) out[i]=out[i+cut]; body-=cut; } } }
      return body; }
}
/* Emit a double with sign/width/precision/flags via the active sink. */
static void emit_double(sink_t* s, double v, char conv, int prec, int width,
                        int left, int zero, int plus, int space, int alt){
    char sign=0; union { double d; unsigned long u; } bu; bu.d=v;
    if(bu.u>>63){ sign='-'; v=-v; } else if(plus) sign='+'; else if(space) sign=' ';
    int upper=(conv>='A'&&conv<='Z'); char lc=upper?(char)(conv+32):conv;
    char body[400]; int bn;
    unsigned long e=(bu.u>>52)&0x7FF, frac=(unsigned long)(bu.u&0xFFFFFFFFFFFFFUL);
    if(e==0x7FF){ const char* w = frac? (upper?"NAN":"nan") : (upper?"INF":"inf");
        bn=0; while(w[bn]){ body[bn]=w[bn]; bn++; } sign = (frac)?0:sign; }
    else { bn=fmt_fp_body(body, v, lc, prec, alt); if(upper) for(int i=0;i<bn;i++) if(body[i]=='e') body[i]='E'; }
    int slen=sign?1:0; int total=bn+slen; int pad=width>total?width-total:0;
    if(!left && !zero) for(int i=0;i<pad;i++) emitc(s,' ');
    if(sign) emitc(s,sign);
    if(!left && zero) for(int i=0;i<pad;i++) emitc(s,'0');
    emit(s,body,bn);
    if(left) for(int i=0;i<pad;i++) emitc(s,' ');
}

static int fmt_core(sink_t* s, const char* fmt, va_list ap){
    char numbuf[32];
    for(; *fmt; fmt++){
        if(*fmt != '%'){ emitc(s, *fmt); continue; }
        fmt++;
        /* flags */
        int left=0, zero=0, plus=0, space=0, alt=0, go=1;
        while(go){
            switch(*fmt){
                case '-': left=1; break; case '0': zero=1; break; case '+': plus=1; break;
                case ' ': space=1; break; case '#': alt=1; break; default: go=0; break;
            }
            if(go) fmt++;
        }
        /* width */
        int width=0; if(*fmt=='*'){ width=va_arg(ap,int); fmt++; if(width<0){left=1;width=-width;} }
        else while(isdigit(*fmt)){ width=width*10+(*fmt-'0'); fmt++; }
        /* precision */
        int prec=-1; if(*fmt=='.'){ fmt++; prec=0; if(*fmt=='*'){ prec=va_arg(ap,int); fmt++; }
                                    else while(isdigit(*fmt)){ prec=prec*10+(*fmt-'0'); fmt++; } }
        /* length */
        int lng=0; while(*fmt=='l'){ lng++; fmt++; } if(*fmt=='z'){ lng=2; fmt++; } else if(*fmt=='h'){ fmt++; }

        char conv=*fmt;
        const char* str=0; char tmp[2]; int neg=0; unsigned long uv=0; int base=10; int upper=0; const char* prefix=0;
        switch(conv){
            case 'c': tmp[0]=(char)va_arg(ap,int); tmp[1]=0; str=tmp; prec=-1; break;
            case 's': str=va_arg(ap,const char*); if(!str) str="(null)"; break;
            case '%': tmp[0]='%'; tmp[1]=0; str=tmp; break;
            case 'd': case 'i': {
                long v = lng? va_arg(ap,long) : (long)va_arg(ap,int);
                if(v<0){ neg=1; uv=(unsigned long)(-v); } else uv=(unsigned long)v;
                goto numconv;
            }
            case 'u': uv = lng? va_arg(ap,unsigned long) : (unsigned long)va_arg(ap,unsigned); goto numconv;
            case 'o': base=8; uv = lng? va_arg(ap,unsigned long):(unsigned long)va_arg(ap,unsigned); if(alt)prefix="0"; goto numconv;
            case 'x': base=16; uv = lng? va_arg(ap,unsigned long):(unsigned long)va_arg(ap,unsigned); if(alt&&uv)prefix="0x"; goto numconv;
            case 'X': base=16; upper=1; uv = lng? va_arg(ap,unsigned long):(unsigned long)va_arg(ap,unsigned); if(alt&&uv)prefix="0X"; goto numconv;
            case 'p': base=16; uv=(unsigned long)va_arg(ap,void*); prefix="0x"; goto numconv;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
                emit_double(s, va_arg(ap,double), (conv=='a'||conv=='A')?'g':conv, prec, width, left, zero, plus, space, alt);
                continue;
            default: emitc(s,'%'); if(conv) emitc(s,conv); continue;
            numconv: {
                const char* digs = upper? "0123456789ABCDEF":"0123456789abcdef";
                char* q = numbuf + sizeof(numbuf); *--q = 0;
                if(uv==0) *--q='0';
                while(uv){ *--q = digs[uv % base]; uv /= base; }
                int ndig = (int)((numbuf + sizeof(numbuf) - 1) - q);
                /* precision = min digits (zero-pad) */
                int zpad = (prec>ndig)? prec-ndig : 0;
                int signlen = (neg||plus||space)?1:0;
                int plen = prefix? (int)strlen(prefix):0;
                int total = ndig + zpad + signlen + plen;
                int pad = width>total? width-total : 0;
                if(!left && !zero) for(int i=0;i<pad;i++) emitc(s,' ');
                if(neg) emitc(s,'-'); else if(plus) emitc(s,'+'); else if(space) emitc(s,' ');
                if(prefix) emit(s,prefix,plen);
                if(!left && zero && prec<0) for(int i=0;i<pad;i++) emitc(s,'0');
                for(int i=0;i<zpad;i++) emitc(s,'0');
                emit(s,q,ndig);
                if(left) for(int i=0;i<pad;i++) emitc(s,' ');
                continue;
            }
        }
        /* string-ish (%c %s %%) with width/precision */
        { size_t len = strlen(str); if(prec>=0 && (size_t)prec<len) len=prec;
          int pad = width>(int)len? width-(int)len:0;
          if(!left) for(int i=0;i<pad;i++) emitc(s,' ');
          emit(s,str,len);
          if(left) for(int i=0;i<pad;i++) emitc(s,' ');
        }
    }
    return s->count;
}

/* sinks */
static void sink_fd(void* a, const char* p, size_t n){ write((int)(long)a, p, n); }
struct bufsink { char* p; size_t cap; size_t len; };
static void sink_buf(void* a, const char* p, size_t n){
    struct bufsink* b=a;
    for(size_t i=0;i<n;i++){ if(b->len+1 < b->cap) b->p[b->len]=p[i]; b->len++; }
}

int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap){
    struct bufsink b={buf,size,0};
    sink_t s={sink_buf,&b,0};
    fmt_core(&s, fmt, ap);
    if(size){ size_t t=b.len<size-1?b.len:size-1; buf[t]=0; }
    return (int)b.len;
}
int snprintf(char* buf, size_t size, const char* fmt, ...){ va_list ap; va_start(ap,fmt); int r=vsnprintf(buf,size,fmt,ap); va_end(ap); return r; }
int sprintf(char* buf, const char* fmt, ...){ va_list ap; va_start(ap,fmt); int r=vsnprintf(buf,(size_t)1<<30,fmt,ap); va_end(ap); return r; }
int dprintf(int fd, const char* fmt, ...){ va_list ap; va_start(ap,fmt); sink_t s={sink_fd,(void*)(long)fd,0}; int r=fmt_core(&s,fmt,ap); va_end(ap); return r; }

/* ============================== <stdio.h> ============================== */
static FILE _stdin  = { 0, 1, -1, {0}, 0 };
static FILE _stdout = { 1, 2, -1, {0}, 0 };
static FILE _stderr = { 2, 2, -1, {0}, 0 };
FILE* stdin  = &_stdin;
FILE* stdout = &_stdout;
FILE* stderr = &_stderr;

int fflush(FILE* f){ if(!f) return 0; if(f->wlen){ write(f->fd,(char*)f->wbuf,f->wlen); f->wlen=0; } return 0; }
/* exit() flushes stdio like a hosted libc; _exit() does not. crt0 routes the
 * return from main() through exit(), so output with no trailing newline (e.g.
 * `head -c 32 /dev/zero`, `printf x`) is no longer lost at process exit. */
void exit(int code){ fflush(stdout); fflush(stderr); _exit(code); }
static void file_putn(FILE* f, const char* p, size_t n){
    for(size_t i=0;i<n;i++){ f->wbuf[f->wlen++]=p[i]; if(f->wlen==BUFSIZ || p[i]=='\n') fflush(f); }
}
static void sink_file(void* a, const char* p, size_t n){ file_putn((FILE*)a, p, n); }

int vfprintf(FILE* f, const char* fmt, va_list ap){ sink_t s={sink_file,f,0}; int r=fmt_core(&s,fmt,ap); return r; }
int fprintf(FILE* f, const char* fmt, ...){ va_list ap; va_start(ap,fmt); int r=vfprintf(f,fmt,ap); va_end(ap); return r; }
int printf(const char* fmt, ...){ va_list ap; va_start(ap,fmt); int r=vfprintf(stdout,fmt,ap); va_end(ap); return r; }

int fputc(int c, FILE* f){ char ch=(char)c; file_putn(f,&ch,1); return (unsigned char)c; }
int putc(int c, FILE* f){ return fputc(c,f); }
int putchar(int c){ return fputc(c,stdout); }
int fputs(const char* s, FILE* f){ file_putn(f,s,strlen(s)); return 0; }

int fgetc(FILE* f){
    if(f->ungot>=0){ int c=f->ungot; f->ungot=-1; return c; }
    unsigned char c; long r=read(f->fd,&c,1);
    if(r<=0){ f->flags|=4; return EOF; }
    return c;
}
int getc(FILE* f){ return fgetc(f); }
int getchar(void){ return fgetc(stdin); }
int ungetc(int c, FILE* f){ if(c==EOF) return EOF; f->ungot=c; return c; }
char* fgets(char* buf, int size, FILE* f){
    int i=0; if(size<=0) return 0;
    while(i<size-1){ int c=fgetc(f); if(c==EOF){ if(i==0) return 0; break; } buf[i++]=(char)c; if(c=='\n') break; }
    buf[i]=0; return buf;
}

size_t fwrite(const void* ptr, size_t sz, size_t nm, FILE* f){ size_t n=sz*nm; if(!n) return 0; file_putn(f,(const char*)ptr,n); return nm; }
size_t fread(void* ptr, size_t sz, size_t nm, FILE* f){
    size_t total=sz*nm, got=0; char* p=ptr;
    while(got<total){ int c=fgetc(f); if(c==EOF) break; p[got++]=(char)c; }
    return sz? got/sz : 0;
}

FILE* fdopen(int fd, const char* mode){ (void)mode; FILE* f=malloc(sizeof(FILE)); if(!f) return 0; f->fd=fd; f->flags=3; f->ungot=-1; f->wlen=0; return f; }
FILE* fopen(const char* path, const char* mode){
    int flags = O_RDONLY;
    if(strchr(mode,'w')||strchr(mode,'a')) flags=O_WRONLY;
    if(strchr(mode,'+')) flags=O_RDWR;
    int fd=open(path,flags);
    if(fd<0){ errno=ENOENT; return 0; }
    return fdopen(fd,mode);
}
int fclose(FILE* f){ if(!f) return EOF; fflush(f); int fd=f->fd; if(f!=&_stdin&&f!=&_stdout&&f!=&_stderr){ close(fd); free(f); } return 0; }
FILE* freopen(const char* path, const char* mode, FILE* f){ if(f && f!=&_stdin && f!=&_stdout && f!=&_stderr) fclose(f); return path? fopen(path,mode) : f; }
int strcoll(const char* a, const char* b){ return strcmp(a,b); }    /* no locale collation */
size_t strxfrm(char* d, const char* s, size_t n){ size_t l=strlen(s); if(n){ size_t c=l<n-1?l:n-1; memcpy(d,s,c); d[c]=0; } return l; }
int feof(FILE* f){ return f->flags&4?1:0; }
int ferror(FILE* f){ return f->flags&8?1:0; }
long ftell(FILE* f){ return lseek(f->fd,0,SEEK_CUR); }
int  fseek(FILE* f, long off, int whence){ fflush(f); return lseek(f->fd,off,whence)<0?-1:0; }
void perror(const char* s){ if(s&&*s){ fputs(s,stderr); fputs(": ",stderr); } fputs(strerror(errno),stderr); fputc('\n',stderr); fflush(stderr); }

/* ========================= pseudo-terminals ============================= */
/* Unix 98 ptys: /dev/ptmx hands out a master, /dev/pts/N is its slave. The
 * kernel keeps a new slave locked until unlockpt(); there is no ownership to
 * fix up (single user), so grantpt() only checks that fd is a master. */
#include <pty.h>
#include <utmp.h>
#include <termios.h>

int posix_openpt(int flags){ return open("/dev/ptmx", flags); }
int grantpt(int fd){ unsigned n; return ioctl(fd, TIOCGPTN, &n) < 0 ? -1 : 0; }
int unlockpt(int fd){ int unlock = 0; return ioctl(fd, TIOCSPTLCK, &unlock) < 0 ? -1 : 0; }
int ptsname_r(int fd, char* buf, size_t len){
    unsigned n;
    if(!buf){ errno = EINVAL; return EINVAL; }
    if(ioctl(fd, TIOCGPTN, &n) < 0) return errno;
    int w = snprintf(buf, len, "/dev/pts/%u", n);
    if(w < 0 || (size_t)w >= len){ errno = ERANGE; return ERANGE; }
    return 0;
}
char* ptsname(int fd){
    static char name[32];
    return ptsname_r(fd, name, sizeof(name)) == 0 ? name : (char*)0;
}

int openpty(int* amaster, int* aslave, char* name,
            const struct termios* termp, const struct winsize* winp){
    char path[32];
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if(m < 0) return -1;
    if(grantpt(m) < 0 || unlockpt(m) < 0 || ptsname_r(m, path, sizeof(path)) != 0){
        int e = errno; close(m); errno = e; return -1;
    }
    int s = open(path, O_RDWR | O_NOCTTY);
    if(s < 0){ int e = errno; close(m); errno = e; return -1; }
    if(termp) tcsetattr(s, TCSANOW, termp);
    if(winp) ioctl(s, TIOCSWINSZ, (void*)winp);
    if(name) strcpy(name, path);
    *amaster = m; *aslave = s;
    return 0;
}

int login_tty(int fd){
    setsid();                              /* fails only for a group leader: go on */
    if(ioctl(fd, TIOCSCTTY, (void*)0) < 0) return -1;
    for(int i = 0; i < 3; i++) if(dup2(fd, i) < 0) return -1;
    if(fd > 2) close(fd);
    return 0;
}

int forkpty(int* amaster, char* name,
            const struct termios* termp, const struct winsize* winp){
    int m, s;
    if(openpty(&m, &s, name, termp, winp) < 0) return -1;
    int pid = fork();
    if(pid < 0){ int e = errno; close(m); close(s); errno = e; return -1; }
    if(pid == 0){
        close(m);
        if(login_tty(s) < 0) _exit(1);
        return 0;
    }
    close(s);
    *amaster = m;
    return pid;
}

/* ttyname: a pty slave is found by matching st_ino against /dev/pts/N; any
 * other terminal is the console. Weak, so a port's own stub still links. */
__attribute__((weak)) int ttyname_r(int fd, char* buf, size_t len){
    struct stat st, pst;
    if(!isatty(fd)){ errno = ENOTTY; return ENOTTY; }
    long r = secos_syscall(56 /*SYS_FSTAT*/, fd, (long)&st, 0, 0, 0);
    const char* found = "/dev/console";
    char path[32];
    if(r == 0){
        for(unsigned n = 0; n < 64; n++){
            snprintf(path, sizeof(path), "/dev/pts/%u", n);
            if(stat(path, &pst) == 0 && pst.st_ino == st.st_ino){ found = path; break; }
        }
    }
    if(strlen(found) + 1 > len){ errno = ERANGE; return ERANGE; }
    strcpy(buf, found);
    return 0;
}
__attribute__((weak)) char* ttyname(int fd){
    static char name[32];
    return ttyname_r(fd, name, sizeof(name)) == 0 ? name : (char*)0;
}
