/* SecOS - [M39] POSIX shell-from-source foundation demo (ring 3).
 * Exercises the new syscalls/libc that a real shell (bash/dash) needs:
 * getcwd/chdir, environ (setenv/getenv), dup/dup2 redirection, getppid, termios.
 * Writes [M39] markers to stdout (which the kernel mirrors to debugcon), so the
 * smoke harness can assert each capability. SPDX-License-Identifier: MIT */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/time.h>
#include "libsecos.h"

static void put(const char* s){ write(1, s, strlen(s)); }

int main(void){
    char buf[160];

    /* getcwd / chdir */
    if(getcwd(buf, sizeof(buf)) && buf[0]=='/') put("[M39] getcwd OK\n");
    else put("[M39] getcwd FAIL\n");
    if(chdir("/bin")==0 && getcwd(buf,sizeof(buf)) && strcmp(buf,"/bin")==0)
        put("[M39] chdir+getcwd OK\n");
    else put("[M39] chdir FAIL\n");
    chdir("/");

    /* environment */
    setenv("SHELL", "/bin/bash", 1);
    char* v = getenv("SHELL");
    if(v && strcmp(v,"/bin/bash")==0) put("[M39] setenv/getenv OK\n");
    else put("[M39] env FAIL\n");
    setenv("SHELL", "/bin/sh", 1); v = getenv("SHELL");
    if(v && strcmp(v,"/bin/sh")==0) put("[M39] setenv overwrite OK\n");
    else put("[M39] env-overwrite FAIL\n");
    if(getenv("NOPE")==0) put("[M39] getenv-missing OK\n");
    else put("[M39] getenv-missing FAIL\n");

    /* getppid */
    if(getppid()>=0) put("[M39] getppid OK\n"); else put("[M39] getppid FAIL\n");

    /* dup2 redirection (the bash pattern: dup2(pipe_w, 1)) */
    int pfd[2];
    if(pipe(pfd)==0){
        int saved = dup(1);
        dup2(pfd[1], 1);
        write(1, "PIPED", 5);          /* now goes into the pipe, not the console */
        dup2(saved, 1);                /* restore stdout */
        close(pfd[1]); close(saved);
        char rb[8]; int n = read(pfd[0], rb, sizeof(rb));
        close(pfd[0]);
        if(n==5 && rb[0]=='P' && rb[4]=='D') put("[M39] dup2 redirect OK\n");
        else put("[M39] dup2 redirect FAIL\n");
    } else put("[M39] pipe FAIL\n");

    /* termios get/set (raw mode toggle) */
    struct termios t;
    if(tcgetattr(1, &t)==0){
        cfmakeraw(&t);
        if(tcsetattr(1, TCSANOW, &t)==0) put("[M39] termios get/set OK\n");
        else put("[M39] tcsetattr FAIL\n");
        /* restore canonical+echo so the console stays usable */
        t.c_lflag |= (ICANON | ECHO);
        tcsetattr(1, TCSANOW, &t);
    } else put("[M39] tcgetattr FAIL\n");

    /* single-user identity */
    if(getuid()==0 && geteuid()==0) put("[M39] getuid/geteuid OK\n");
    else put("[M39] getuid FAIL\n");

    /* gettimeofday (uptime-based) */
    struct timeval tv;
    if(gettimeofday(&tv,0)==0 && tv.tv_sec>=0) put("[M39] gettimeofday OK\n");
    else put("[M39] gettimeofday FAIL\n");

    /* fcntl F_DUPFD */
    int fd2 = fcntl(1, F_DUPFD, 3);
    if(fd2>=0){ put("[M39] fcntl F_DUPFD OK\n"); close(fd2); }
    else put("[M39] fcntl FAIL\n");

    put("[M39] POSIX-FOUNDATION DONE\n");
    return 0;
}
