/*
 * SecOS Kernel - User Syscall Test Program
 * Minimal user-mode entry invoking core syscalls through INT 0x80.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

static inline uint64_t do_syscall(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2){
    uint64_t r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(num), "D"(a0), "S"(a1), "d"(a2), "c"(0), "r"( (uint64_t)0 ) : "memory");
    return r;
}

// Simple entry point (placed at USER_CODE_BASE by loader)
void _start(void){
    uint64_t pid = do_syscall(6,0,0,0); // SYS_GETPID
    const char* msg = "Hello from user proc PID=";
    // naive: write each char (fd 1 assumed is some console abstraction later; for now will fail gracefully)
    for(const char* p=msg; *p; ++p){ do_syscall(2,1,(uint64_t)p,1); }
    // Convert pid to hex nibble
    char hx[]="0123456789ABCDEF"; char buf[9]; buf[8]='\0'; for(int i=0;i<8;i++){ buf[7-i]=hx[(pid>>(i*4))&0xF]; }
    for(char* p=buf; *p; ++p){ do_syscall(2,1,(uint64_t)p,1); }
    char nl='\n'; do_syscall(2,1,(uint64_t)&nl,1);
    do_syscall(1,0,0,0); // SYS_EXIT
    while(1){}
}