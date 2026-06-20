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
        case EBADF:  return "Bad file descriptor";
        case ENOMEM: return "Out of memory";
        case EACCES: return "Permission denied";
        case EEXIST: return "File exists";
        case EINVAL: return "Invalid argument";
        case ENOSPC: return "No space left on device";
        case EPIPE:  return "Broken pipe";
        case ENOSYS: return "Function not implemented";
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
char* getenv(const char* name){ (void)name; return 0; }   /* no environment yet */
void exit(int code){ _exit(code); }
void abort(void){ write(2,"abort\n",6); _exit(134); }

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
time_t time(time_t* t){ time_t s=(time_t)(getticks()/1000); if(t)*t=s; return s; }
clock_t clock(void){ return (clock_t)getticks(); }

/* ============================== <assert.h> ============================= */
void __assert_fail(const char* expr, const char* file, int line){
    char buf[256]; int n=snprintf(buf,sizeof buf,"assert failed: %s (%s:%d)\n",expr,file,line);
    write(2,buf,n); _exit(134);
}

/* ============================== <unistd.h> extras ====================== */
unsigned int sleep(unsigned int s){ sleep_ticks(s*1000); return 0; }
int usleep(unsigned long us){ unsigned t=(unsigned)(us/1000); if(!t)t=1; sleep_ticks(t); return 0; }
int isatty(int fd){ return fd==0||fd==1||fd==2; }

/* ============================ printf core ============================== */
/* A sink-based formatter so printf/fprintf stream directly and snprintf bounds. */
typedef struct { void (*put)(void*, const char*, size_t); void* arg; int count; } sink_t;
static void emit(sink_t* s, const char* p, size_t n){ s->put(s->arg, p, n); s->count += (int)n; }
static void emitc(sink_t* s, char c){ s->put(s->arg, &c, 1); s->count++; }

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
int feof(FILE* f){ return f->flags&4?1:0; }
int ferror(FILE* f){ return f->flags&8?1:0; }
long ftell(FILE* f){ return lseek(f->fd,0,SEEK_CUR); }
int  fseek(FILE* f, long off, int whence){ fflush(f); return lseek(f->fd,off,whence)<0?-1:0; }
void perror(const char* s){ if(s&&*s){ fputs(s,stderr); fputs(": ",stderr); } fputs(strerror(errno),stderr); fputc('\n',stderr); fflush(stderr); }
