/* sysfuzz — seeded ring-3 syscall fuzzer for the SecOS kernel.
 * SPDX-License-Identifier: MIT
 *
 * Calls every syscall number (plus out-of-range ones) with random and
 * edge-case arguments: NULL, kernel addresses, non-canonical addresses,
 * unaligned pointers, huge lengths, negative fds, never-faulted demand pages,
 * unmapped holes, PROT_NONE pages, buffers straddling the end of a mapping.
 *
 * Contract under test: whatever the arguments, the KERNEL must not fault in
 * ring 0, panic or hang. A syscall with bad arguments must return an error.
 *
 * Each batch runs in a forked child so a deliberate bad *memory* access that
 * gets the child killed ([KILL]) does not stop the fuzzer. A ring-0 fault
 * halts the kernel, which the harness detects on debugcon. The child only
 * touches a private /tmp/fz sandbox and never closes/replaces console fds.
 *
 * Every pointer/length class is fuzzed (bad user pointers must return -EFAULT
 * via the uaccess exception-table fixups, never fault ring 0). The only
 * argument shaping is to keep the run NON-BLOCKING, so a batch always ends:
 *  - read/write target a private regular file (/tmp/fz/w, recreated per batch)
 *    or an invalid fd, never the TTY (fd 0 blocks on a line) or a pipe the
 *    fuzzer created itself (a self-deadlock: empty pipe with our own writer, or
 *    a full one with our own reader); that file fd is never closed/dup2'd over;
 *  - msg_recv is preceded by a 1-byte msg_send on the same channel, so a valid
 *    channel is never empty (M17 blocks the caller on an empty channel);
 *  - sigreturn (which kills a caller outside a handler) is drawn rarely;
 *  - ptys: each batch opens a pty pair (/dev/ptmx + its slave) O_NONBLOCK and
 *    read/write/ioctl hit both ends; O_NONBLOCK is re-asserted before every
 *    pty read/write (a fuzzed FIONBIO may clear it), and the child ignores the
 *    terminal signals (^C/^Z/^\ in written bytes, SIGHUP on a fuzzed master
 *    close, SIGTTIN/SIGTTOU) and never re-defaults them, so a batch can
 *    neither stop nor block on its own terminal.
 *
 * Usage: sysfuzz [-s seed] [-b batches] [-c calls] [-r batch] [-v] [-F]
 *   -r N   replay only batch N verbosely (reproducer)
 *   -F     run batches in-process (no fork), for verbose panic triage
 */
#include "libsecos.h"
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>

/* Syscall numbers (mirror kernel/syscall.h; not exported to user headers). */
#define SYS_YIELD 0
#define SYS_EXIT 1
#define SYS_WRITE 2
#define SYS_READ 3
#define SYS_OPEN 4
#define SYS_CLOSE 5
#define SYS_GETPID 6
#define SYS_DRIVER 7
#define SYS_SPAWN 8
#define SYS_WAIT 9
#define SYS_GETTICKS 10
#define SYS_MSG_SEND 11
#define SYS_MSG_RECV 12
#define SYS_SLEEP 13
#define SYS_MMAP 14
#define SYS_MUNMAP 15
#define SYS_BRK 16
#define SYS_MPROTECT 17
#define SYS_FORK 18
#define SYS_LSEEK 19
#define SYS_STAT 20
#define SYS_SOCKET 21
#define SYS_CONNECT 22
#define SYS_BIND 23
#define SYS_LISTEN 24
#define SYS_ACCEPT 25
#define SYS_SEND 26
#define SYS_RECV 27
#define SYS_SENDTO 28
#define SYS_RECVFROM 29
#define SYS_SOCKCLOSE 30
#define SYS_PIPE 31
#define SYS_CHMOD 32
#define SYS_CHOWN 33
#define SYS_UTIMES 34
#define SYS_READLINK 35
#define SYS_SYMLINK 36
#define SYS_LSTAT 37
#define SYS_MOUNT 38
#define SYS_UMOUNT 39
#define SYS_GETDENTS 40
#define SYS_CREATE 41
#define SYS_MKDIR 42
#define SYS_UNLINK 43
#define SYS_SIGACTION 44
#define SYS_SIGRETURN 45
#define SYS_KILL 46
#define SYS_SIGPROCMASK 47
#define SYS_SETPGID 48
#define SYS_DUP2 49
#define SYS_DUP 50
#define SYS_CHDIR 51
#define SYS_GETCWD 52
#define SYS_IOCTL 53
#define SYS_GETPPID 54
#define SYS_WAITANY 55
#define SYS_FSTAT 56
#define SYS_GETPGID 57
#define SYS_SIGSUSPEND 58
#define SYS_REBOOT 59
#define SYS_EXECVE 60
#define SYS_FCNTL 61
#define SYS_SETSID 85
#define SYS_GETSID 86

#define NSYS_KNOWN 62
/* Syscalls numbered past the contiguous range, drawn as well. */
static const long g_extra_sys[] = { SYS_SETSID, SYS_GETSID };
#define NSYS_EXTRA ((long)(sizeof(g_extra_sys) / sizeof(g_extra_sys[0])))

typedef unsigned long u64;

static u64 g_rng;
static int g_nofork; /* -F: run batches in-process (verbose panic triage) */
static u64 rnd(void){ g_rng^=g_rng>>12; g_rng^=g_rng<<25; g_rng^=g_rng>>27; return g_rng*2685821657736338717UL; }
static u64 rn(u64 n){ return n? rnd()%n : 0; }
static long sc(long n,long a0,long a1,long a2,long a3,long a4){ return secos_syscall(n,a0,a1,a2,a3,a4); }

static unsigned char g_buf[8192] __attribute__((aligned(4096)));
static u64 g_scratch, g_fresh, g_hole, g_none, g_edge;

/* interesting pointer values */
static u64 ptrpool(void){
    switch(rn(20)){
    case 0: return 0;
    case 1: return (u64)g_buf;
    case 2: return (u64)g_buf + 1 + rn(7);
    case 3: return 0xFFFFFFFF80000000UL;
    case 4: return 0xFFFF888000000000UL;
    case 5: return 0x0000800000000000UL;
    case 6: return 0x0000800000000000UL - 8;
    case 7: return 0xDEADBEEFUL;
    case 8: return g_fresh + rn(0x4000);
    case 9: return g_hole;
    case 10:return g_none;
    case 11:return g_edge + 0xFF0;
    case 12:return 0x1000;
    case 13:return (u64)&g_buf[8192-4];
    case 14:return 0x7FFFFFFFFFFFFFFFUL;
    /* low addresses that alias the kernel-only low identity map (phys 0..512 MB):
     * a syscall must never read/write these on the caller's behalf. */
    case 15:return 0x10;
    case 16:return 0x100000;        /* ~kernel image load address */
    case 17:return 0x200000;
    case 18:return 0x1FFFFFFF;      /* top of the identity window */
    default:return (u64)g_scratch + rn(0x4000);
    }
}
static u64 lenpool(void){
    switch(rn(10)){
    case 0: return 0; case 1: return 1; case 2: return rn(64);
    case 3: return 4096; case 4: return 0x100000; case 5: return 0x7FFFFFFFFFFFFFFFUL;
    case 6: return (u64)-1; case 7: return 0x800000000000UL; case 8: return rn(0x10000);
    default:return rn(8192);
    }
}
static long intpool(void){
    switch(rn(10)){
    case 0: return 0; case 1: return -1; case 2: return 1; case 3: return 2;
    case 4: return 3; case 5: return 32; case 6: return -4096;
    case 7: return (long)rn(70); case 8: return 0x7FFFFFFF; default:return (long)rnd();
    }
}

static const char* g_fzpaths[] = {
    "/tmp/fz","/tmp/fz/a","/tmp/fz/b","/tmp/fz/d","/tmp/fz/d/x","/tmp/fz/l",
    "/tmp/fz//a","/tmp/fz/","","  ","/tmp/fz/.","/tmp/fz/..",
    "/dev/ptmx","/dev/pts/0","/dev/pts/1","/dev/pts","/dev/tty","/dev/pts/63","/dev/pts/x",
};
static u64 strptr(void){ return ptrpool(); }
static u64 pathpool(void){
    if(rn(3)==0) return strptr();
    return (u64)g_fzpaths[rn(sizeof(g_fzpaths)/sizeof(g_fzpaths[0]))];
}

static int g_verbose;
static int g_ffd = -1; /* per-batch regular file: the only real fd read/write use */
static int g_pm = -1, g_ps = -1; /* per-batch pty master/slave (O_NONBLOCK) */

/* Terminal ioctls (Linux numbers) for the pty ends. */
static const unsigned long g_ttyioc[] = {
    0x5401 /*TCGETS*/, 0x5402 /*TCSETS*/, 0x5403 /*TCSETSW*/, 0x5404 /*TCSETSF*/,
    0x5409 /*TCSBRK*/, 0x540A /*TCXONC*/, 0x540B /*TCFLSH*/, 0x540E /*TIOCSCTTY*/,
    0x540F /*TIOCGPGRP*/, 0x5410 /*TIOCSPGRP*/, 0x5411 /*TIOCOUTQ*/, 0x5413 /*TIOCGWINSZ*/,
    0x5414 /*TIOCSWINSZ*/, 0x541B /*FIONREAD*/, 0x5421 /*FIONBIO*/, 0x5422 /*TIOCNOTTY*/,
    0x5429 /*TIOCGSID*/, 0x80045430 /*TIOCGPTN*/, 0x40045431 /*TIOCSPTLCK*/,
    0x80045439 /*TIOCGPTLCK*/, 0x5420 /*TIOCPKT*/, 0x5441 /*TIOCGPTPEER*/,
};
#define N_TTYIOC (sizeof(g_ttyioc) / sizeof(g_ttyioc[0]))
static int pty_fd(void){ return rn(2) ? g_pm : g_ps; }
static void pty_nonblock(int fd){ int one = 1; if(fd >= 0) sc(SYS_IOCTL, fd, 0x5421, (long)&one, 0, 0); }
/* Signals the batch must never take the default action for (stop/terminate on
 * its own pty): ignored at batch start, never re-defaulted by sigaction. */
static int tty_sig(long s){ return s==1||s==2||s==3||s==20||s==21||s==22; }
static const char* sysname(long n);

static void one_call(void){
    long n;
    if(rn(20)==0)      n = (long)(NSYS_KNOWN + rn(2000));
    else if(rn(15)==0) n = g_extra_sys[rn(NSYS_EXTRA)];
    else               n = (long)rn(NSYS_KNOWN);

    if(n==SYS_EXIT){ n=SYS_GETPID; }
    if(n==SYS_FORK && rn(4)!=0){ n=SYS_GETTICKS; }
    /* SYS_SIGRETURN outside a handler restores a forged context and kills the
     * caller (a ring-3 kill, kernel-safe per M30): keep it, but rare, so most
     * batches run to the end. */
    if(n==SYS_SIGRETURN && rn(8)!=0) n=SYS_GETTICKS;
    /* sigsuspend sleeps until a signal arrives (none will): never. reboot would
     * shut the machine down: never (its argument checks are trivial). */
    if(n==SYS_SIGSUSPEND) n=SYS_GETTICKS;
    if(n==SYS_REBOOT) n=SYS_GETPID;

    long a0,a1,a2,a3,a4; a0=a1=a2=a3=a4=0;
    switch(n){
    case SYS_WRITE: case SYS_READ:
        /* Never the TTY or a fuzzer-made pipe (they block): the batch file,
         * or an invalid fd. Pointer and length stay fully fuzzed. */
        switch(rn(6)){
        case 0: a0=-1-(long)rn(200); break;
        case 1: a0=64+(long)rn(1000); break;
        case 2: case 3: a0=pty_fd(); pty_nonblock((int)a0); break;   /* never blocks */
        default:a0=g_ffd; break;
        }
        a1=(long)strptr(); a2=(long)lenpool();
        break;
    case SYS_OPEN: a0=(long)pathpool(); a1=intpool(); break;
    case SYS_CLOSE: a0=intpool(); if((a0>=0&&a0<=2)||a0==g_ffd) a0=10+(a0&3); break;
    case SYS_STAT: case SYS_LSTAT:
        a0=(long)pathpool(); a1=(long)strptr(); break;
    case SYS_FSTAT: a0=intpool(); a1=(long)strptr(); break;
    case SYS_LSEEK: a0=intpool(); a1=(long)intpool(); a2=(long)rn(4); break;
    case SYS_MMAP: a0=(long)ptrpool(); a1=(long)lenpool(); a2=(long)rn(8); a3=(long)rn(0x40); a4=intpool(); break;
    case SYS_MUNMAP: a0=(long)ptrpool(); a1=(long)lenpool(); break;
    case SYS_MPROTECT: a0=(long)ptrpool(); a1=(long)lenpool(); a2=(long)rn(8); break;
    case SYS_BRK: a0=(long)ptrpool(); break;
    case SYS_MSG_SEND: a0=intpool(); a1=(long)strptr(); a2=(long)lenpool(); break;
    case SYS_MSG_RECV:
        /* M17: recv blocks on an EMPTY valid channel. Queue a byte first, so a
         * valid channel is non-empty (or full) and recv returns at once. */
        a0=(long)rn(6)-1; a1=(long)strptr(); a2=(long)lenpool();
        sc(SYS_MSG_SEND,a0,(long)g_buf,1,0,0);
        break;
    case SYS_SLEEP: a0=(long)rn(2); break;
    case SYS_GETDENTS: a0=(long)pathpool(); a1=(long)strptr(); a2=(long)lenpool(); break;
    case SYS_CREATE: case SYS_MKDIR: case SYS_UNLINK: a0=(long)pathpool(); break;
    case SYS_CHMOD: a0=(long)pathpool(); a1=(long)rn(0x1000); break;
    case SYS_CHOWN: a0=(long)pathpool(); a1=intpool(); a2=intpool(); break;
    case SYS_UTIMES: a0=(long)pathpool(); a1=intpool(); a2=intpool(); break;
    case SYS_READLINK: a0=(long)pathpool(); a1=(long)strptr(); a2=(long)lenpool(); break;
    case SYS_SYMLINK: a0=(long)pathpool(); a1=(long)pathpool(); break;
    case SYS_MOUNT: a0=(long)pathpool(); a1=(long)pathpool(); a2=(long)pathpool(); break;
    case SYS_UMOUNT: a0=(long)pathpool(); break;
    case SYS_CHDIR: a0=(long)pathpool(); break;
    case SYS_GETCWD: a0=(long)strptr(); a1=(long)lenpool(); break;
    case SYS_DUP: a0=intpool(); break;
    case SYS_DUP2: a0=intpool(); a1=10+(long)rn(16); if(a1==g_ffd) a1=26; break;
    case SYS_IOCTL:
        if(rn(2)){                       /* a terminal request on a pty end */
            a0=pty_fd(); a1=(long)g_ttyioc[rn(N_TTYIOC)];
            a2 = rn(3) ? (long)strptr() : (long)rn(3);
        } else { a0=intpool(); a1=(long)intpool(); a2=(long)strptr(); }
        break;
    case SYS_GETSID: a0 = rn(2) ? 0 : intpool(); break;
    case SYS_PIPE: a0=(long)strptr(); a1=rn(3)==0 ? 0x80000 : (long)rn(2); break;
    case SYS_SIGACTION: a0=(long)rn(34); if(tty_sig(a0)) a0=10; a1=(long)strptr(); a2=(long)strptr(); break;
    case SYS_EXECVE: {
        /* Bad/odd path, argv and envp pointers. No pool path is an ELF (the
         * fixtures are empty files / dirs), so a call can only fail: -ENOENT,
         * -EACCES, -ENOEXEC, -EFAULT, -E2BIG. A vector of fuzzed string pointers
         * exercises the per-string copy-in. */
        static u64 vec[2][5];
        for(int v=0; v<2; v++){ for(int k=0;k<4;k++) vec[v][k]=ptrpool(); vec[v][rn(5)]=0; vec[v][4]=0; }
        a0=(long)pathpool();
        a1=rn(2) ? (long)vec[0] : (long)ptrpool();
        a2=rn(2) ? (long)vec[1] : (long)ptrpool();
        break; }
    case SYS_FCNTL:
        a0=rn(2) ? (long)g_ffd : intpool();
        a1=(long)rn(6); if(rn(4)==0) a1=1030 /*F_DUPFD_CLOEXEC*/;
        a2=intpool();
        break;
    case SYS_SIGPROCMASK: a0=intpool(); a1=(long)strptr(); a2=(long)strptr(); break;
    case SYS_KILL: a0=(long)getpid(); a1=0; break;
    case SYS_SETPGID: a0=0; a1=0; break;
    case SYS_WAIT: a0=0x7fffff00+(long)rn(8); break;
    case SYS_WAITANY: a0=(long)strptr(); a1=1; break;
    case SYS_SOCKET: case SYS_CONNECT: case SYS_BIND: case SYS_LISTEN:
    case SYS_ACCEPT: case SYS_SEND: case SYS_RECV: case SYS_SENDTO:
    case SYS_RECVFROM: case SYS_SOCKCLOSE:
        a0=intpool(); a1=(long)strptr(); a2=(long)lenpool(); a3=(long)strptr(); break;
    case SYS_DRIVER: a0=(long)strptr(); break;
    case SYS_FORK: break;
    default:
        a0=(long)strptr(); a1=(long)strptr(); a2=(long)lenpool(); a3=intpool(); a4=intpool();
        break;
    }
    if(g_verbose){
        char line[96]; int p=0; const char* nm=sysname(n);
        p+=snprintf(line+p,sizeof(line)-p,"FZ> n=%ld %s a0=%lx a1=%lx a2=%lx\n",n,nm,(u64)a0,(u64)a1,(u64)a2);
        write(1,line,p);
    }
    long r = sc(n,a0,a1,a2,a3,a4);
    if(n==SYS_FORK && r==0) _exit(0);
    (void)r;
}

static void setup_fixtures(void){
    for(unsigned i=0;i<sizeof(g_buf);i++) g_buf[i]=(unsigned char)i;
    g_scratch=(u64)mmap(0,0x4000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS);
    if((long)g_scratch<0||g_scratch==0) g_scratch=(u64)g_buf;
    else for(int i=0;i<0x4000;i+=0x1000) ((volatile char*)g_scratch)[i]=1;
    g_fresh=(u64)mmap(0,0x4000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS);
    if((long)g_fresh<0) g_fresh=g_scratch;
    g_none=(u64)mmap(0,0x1000,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS);
    if((long)g_none<0) g_none=g_scratch;
    g_edge=(u64)mmap(0,0x1000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS);
    if((long)g_edge<0) g_edge=g_scratch; else ((volatile char*)g_edge)[0]=1;
    u64 h=(u64)mmap(0,0x1000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS);
    if((long)h>0){ munmap((void*)h,0x1000); g_hole=h; } else g_hole=0xDEAD0000UL;
    mkdir("/tmp",0777); mkdir("/tmp/fz",0777); mkdir("/tmp/fz/d",0777);
    creat_file("/tmp/fz/a"); creat_file("/tmp/fz/b");
}

/* (Re)create the batch's private regular file and open it RW: the only real fd
 * the fuzzed read/write target, so neither can block (see header). Recreated per
 * batch so fuzzed lseek+write growth never accumulates across batches. */
static void batch_file(void){
    if(g_ffd>=0) close(g_ffd);
    unlink("/tmp/fz/w");
    creat_file("/tmp/fz/w");
    g_ffd = open("/tmp/fz/w", O_RDWR);
    /* A fresh pty pair per batch (the previous one goes with its child). */
    if(g_pm>=0) close(g_pm);
    if(g_ps>=0) close(g_ps);
    g_pm = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK);
    g_ps = -1;
    if(g_pm >= 0){
        int unlock = 0; unsigned n = 0;
        sc(SYS_IOCTL, g_pm, 0x40045431 /*TIOCSPTLCK*/, (long)&unlock, 0, 0);
        if(sc(SYS_IOCTL, g_pm, 0x80045430 /*TIOCGPTN*/, (long)&n, 0, 0) == 0){
            char nm[32]; snprintf(nm, sizeof(nm), "/dev/pts/%u", n);
            g_ps = open(nm, O_RDWR | O_NOCTTY | O_NONBLOCK);
        }
    }
    for(int s = 1; s < 32; s++) if(tty_sig(s)) signal(s, SIG_IGN);
}

static int g_names_ready;
static const char* g_names[NSYS_KNOWN];
static void init_names(void){
    for(int i=0;i<NSYS_KNOWN;i++) g_names[i]="?";
    g_names[0]="yield";g_names[1]="exit";g_names[2]="write";g_names[3]="read";
    g_names[4]="open";g_names[5]="close";g_names[6]="getpid";g_names[7]="driver";
    g_names[8]="spawn";g_names[9]="wait";g_names[10]="getticks";g_names[11]="msgsend";
    g_names[12]="msgrecv";g_names[13]="sleep";g_names[14]="mmap";g_names[15]="munmap";
    g_names[16]="brk";g_names[17]="mprotect";g_names[18]="fork";g_names[19]="lseek";
    g_names[20]="stat";g_names[21]="socket";g_names[22]="connect";g_names[23]="bind";
    g_names[24]="listen";g_names[25]="accept";g_names[26]="send";g_names[27]="recv";
    g_names[28]="sendto";g_names[29]="recvfrom";g_names[30]="sockclose";g_names[31]="pipe";
    g_names[32]="chmod";g_names[33]="chown";g_names[34]="utimes";g_names[35]="readlink";
    g_names[36]="symlink";g_names[37]="lstat";g_names[38]="mount";g_names[39]="umount";
    g_names[40]="getdents";g_names[41]="create";g_names[42]="mkdir";g_names[43]="unlink";
    g_names[44]="sigaction";g_names[45]="sigreturn";g_names[46]="kill";g_names[47]="sigprocmask";
    g_names[48]="setpgid";g_names[49]="dup2";g_names[50]="dup";g_names[51]="chdir";
    g_names[52]="getcwd";g_names[53]="ioctl";g_names[54]="getppid";g_names[55]="waitany";
    g_names[56]="fstat";g_names[57]="getpgid";g_names[58]="sigsuspend";g_names[59]="reboot";
    g_names[60]="execve";g_names[61]="fcntl";
    g_names_ready=1;
}
static const char* sysname(long n){
    if(!g_names_ready) init_names();
    if(n>=0&&n<NSYS_KNOWN) return g_names[n];
    if(n==SYS_SETSID) return "setsid";
    if(n==SYS_GETSID) return "getsid";
    return "oor";
}

static long atou(const char* s){ long v=0; int neg=0; if(*s=='-'){neg=1;s++;} while(*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;} return neg?-v:v; }

int main(int argc, char** argv){
    u64 seed=0x1234abcd;
    long batches=40, calls=120, replay=-1;
    init_names();
    for(int i=1;i<argc;i++){
        if(argv[i][0]=='-'&&argv[i][1]=='s'&&i+1<argc) seed=(u64)atou(argv[++i]);
        else if(argv[i][0]=='-'&&argv[i][1]=='b'&&i+1<argc) batches=atou(argv[++i]);
        else if(argv[i][0]=='-'&&argv[i][1]=='c'&&i+1<argc) calls=atou(argv[++i]);
        else if(argv[i][0]=='-'&&argv[i][1]=='r'&&i+1<argc) replay=atou(argv[++i]);
        else if(argv[i][0]=='-'&&argv[i][1]=='v') g_verbose=1;
        else if(argv[i][0]=='-'&&argv[i][1]=='F') g_nofork=1;
    }
    printf("[sysfuzz] seed=%lu batches=%ld calls=%ld\n",seed,batches,calls);
    setup_fixtures();

    for(long b=0;b<batches;b++){
        if(replay>=0 && b!=replay) continue;
        u64 batch_seed = seed + (u64)b*0x9E3779B97F4A7C15UL;
        if(g_nofork){
            /* run the batch in this process so a verbose FZ> line prints right
             * before the call that faults — the last line in the log names the
             * offending syscall even when the fault PANICS the kernel. */
            g_rng = batch_seed ? batch_seed : 1;
            batch_file();
            for(long k=0;k<calls;k++) one_call();
            printf("[sysfuzz] batch %ld ok (nofork)\n",b);
            continue;
        }
        int pid = fork();
        if(pid==0){
            g_rng = batch_seed ? batch_seed : 1;
            if(replay>=0) g_verbose=1;
            batch_file();
            for(long k=0;k<calls;k++) one_call();
            _exit((int)(batch_seed%100));
        } else if(pid>0){
            int st = waitpid(pid);
            if(st>=0 && WIFSIGNALED(st)){
                /* A ring-3 kill of the fuzz child is acceptable (deliberate bad
                 * memory access). Report it so it can be reproduced in-process
                 * with:  sysfuzz -s <seed> -r <batch> -v -F  (prints an FZ> line
                 * before each call, so the last one names the offender even if it
                 * then PANICS the kernel). We do NOT replay in the parent — that
                 * would re-run the same fault and kill the fuzzer itself. */
                printf("[sysfuzz] batch %ld (seed=%lu) child KILLED sig=%d (repro: -r %ld -v -F)\n",b,batch_seed,WTERMSIG(st),b);
            } else {
                printf("[sysfuzz] batch %ld ok (status=%d)\n",b,WEXITSTATUS(st));
            }
        } else {
            printf("[sysfuzz] fork failed at batch %ld\n",b);
        }
    }
    printf("[sysfuzz] DONE ok\n");
    return 0;
}
