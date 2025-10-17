/*
 * SecOS Kernel - Driver Syscall Test Harness
 * Kernel-space invocation exercising driver syscall dispatcher & audit trail.
 * For genuine user-space tests, spawn a ring3 process invoking INT 0x80.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
// NOTE: Direct calls to driver_syscall used here to validate dispatcher & audit logging.

#include <stdint.h>
#include "driver_if.h"
#include "terminal.h"

static void tprint(const char* s){ while(*s){ terminal_putchar(*s++); } }
static void tprint_hex(uint64_t v){ char hx[]="0123456789ABCDEF"; for(int i=60;i>=0;i-=4) terminal_putchar(hx[(v>>i)&0xF]); }
static void tprint_dec(uint64_t v){ char buf[32]; int i=0; if(v==0){ buf[i++]='0'; } else { char tmp[32]; int t=0; while(v){ tmp[t++]= (char)('0'+(v%10)); v/=10; } while(t){ buf[i++]=tmp[--t]; } } buf[i]=0; tprint(buf); }

void user_test_driver(void){
    tprint("[drvtest] start\n");
    driver_call_t call; call.device_id=0; call.opcode=DRIVER_OP_READ_REG; call.target=0xF0000010ULL; call.value=0; call.flags=DRV_FLAG_REQUIRE_AUDIT;
    int r = driver_syscall(&call); tprint("READ_REG before bind ret="); tprint_dec(r); tprint("\n");
    // User should run 'drvreg 0' via shell before repeating to obtain permission.
    r = driver_syscall(&call); tprint("READ_REG after bind ret="); tprint_dec(r); tprint(" val="); tprint_hex(call.value); tprint("\n");
    // WRITE_REG permitted (device 0 has WRITE_REG capability)
    call.opcode=DRIVER_OP_WRITE_REG; call.value=0x1122334455667788ULL; r=driver_syscall(&call); tprint("WRITE_REG ret="); tprint_dec(r); tprint("\n");
    call.opcode=DRIVER_OP_READ_REG; call.value=0; r=driver_syscall(&call); tprint("READ_REG confirm ret="); tprint_dec(r); tprint(" val="); tprint_hex(call.value); tprint("\n");
    // RANGE error scenario
    call.target=0xF0001000ULL; call.opcode=DRIVER_OP_READ_REG; r=driver_syscall(&call); tprint("READ_REG out-of-range ret="); tprint_dec(r); tprint("\n"); call.target=0xF0000010ULL;
    // MAP_MEM (not supported by device 0 capabilities; expect PERM)
    call.opcode=DRIVER_OP_MAP_MEM; call.target=0xF1000000ULL; call.value=0; r=driver_syscall(&call); tprint("MAP_MEM ret="); tprint_dec(r); tprint(" (expected PERM)\n");
    // MAP_MEM misaligned (ARGS) still without capability
    call.target=0xF1000004ULL; r=driver_syscall(&call); tprint("MAP_MEM misaligned ret="); tprint_dec(r); tprint("\n");
    // GET_INFO (caps mask returned in value)
    call.opcode=DRIVER_OP_GET_INFO; call.target=0; call.value=0; r=driver_syscall(&call); tprint("GET_INFO ret="); tprint_dec(r); tprint(" caps="); tprint_hex(call.value); tprint("\n");
    // Unknown opcode test
    call.opcode=0x99; r=driver_syscall(&call); tprint("UNKNOWN opcode ret="); tprint_dec(r); tprint("\n");
    tprint("[drvtest] end\n");
}
