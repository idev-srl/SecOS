/* SecOS Driver Space probe — exercises SYS_DRIVER from ring 3 and reports each
 * outcome. Linked into two binaries that differ only in their signed manifest:
 *   - driver_demo  (PROC_TYPE_DRIVER, dev 0, caps READ|WRITE|GET_INFO)
 *   - userprobe    (PROC_TYPE_USER)
 * so the same calls show the capability boundary from both sides.
 *
 * Each report line is assembled in a local buffer and emitted with a single
 * write(): the kernel prints a per-syscall trace on debugcon, so one write per
 * line keeps each line contiguous (and greppable) in the log.
 * SPDX-License-Identifier: MIT */
#include "libsecos.h"
#include "secos_driver.h"

/* device 0 register window base is 0xF0000000 (kernel seed); pick an in-range,
 * 8-byte-aligned register offset and the device memory window base. */
#define DEV0_REG   0xF0000010ULL
#define DEV0_MEM   0xF1000000ULL
#define TESTVAL    0xCAFEF00DD15EA5EDULL

/* --- tiny line builder --- */
static int  put_str(char* b, int i, const char* s) { while (*s) b[i++] = *s++; return i; }
static int  put_dec(char* b, int i, long v) {
    char t[24]; int n = 0; unsigned long u;
    int neg = (v < 0); u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (u == 0) t[n++] = '0';
    while (u) { t[n++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) b[i++] = '-';
    while (n) b[i++] = t[--n];
    return i;
}
static int  put_hex(char* b, int i, unsigned long v) {
    const char* H = "0123456789abcdef";
    b[i++] = '0'; b[i++] = 'x';
    for (int k = 0; k < 16; k++) b[i++] = H[(v >> ((15 - k) * 4)) & 0xF];
    return i;
}

int drvprobe_run(const char* tag) {
    driver_call_t c;
    char line[96]; int i;

    /* 1. GET_INFO (granted for driver_demo) */
    c.opcode = DRIVER_OP_GET_INFO; c.device_id = 0; c.target = 0; c.value = 0;
    c.flags = DRV_FLAG_REQUIRE_AUDIT;
    long r = secos_driver(&c);
    i = 0; i = put_str(line,i,"["); i = put_str(line,i,tag);
    i = put_str(line,i,"] GET_INFO ret="); i = put_dec(line,i,r);
    i = put_str(line,i," caps="); i = put_hex(line,i,c.value); line[i++]='\n';
    write(1, line, i);

    /* 2. WRITE_REG (granted for driver_demo) */
    c.opcode = DRIVER_OP_WRITE_REG; c.device_id = 0; c.target = DEV0_REG;
    c.value = TESTVAL; c.flags = DRV_FLAG_REQUIRE_AUDIT;
    r = secos_driver(&c);
    i = 0; i = put_str(line,i,"["); i = put_str(line,i,tag);
    i = put_str(line,i,"] WRITE_REG ret="); i = put_dec(line,i,r); line[i++]='\n';
    write(1, line, i);

    /* 3. READ_REG (granted) — value should round-trip the write above */
    c.opcode = DRIVER_OP_READ_REG; c.device_id = 0; c.target = DEV0_REG;
    c.value = 0; c.flags = DRV_FLAG_REQUIRE_AUDIT;
    r = secos_driver(&c);
    i = 0; i = put_str(line,i,"["); i = put_str(line,i,tag);
    i = put_str(line,i,"] READ_REG ret="); i = put_dec(line,i,r);
    i = put_str(line,i," val="); i = put_hex(line,i,c.value); line[i++]='\n';
    write(1, line, i);

    /* 4. MAP_MEM — NOT granted to driver_demo (manifest withholds it though the
     * device supports it); for userprobe the whole call is refused (NOTDRV). */
    c.opcode = DRIVER_OP_MAP_MEM; c.device_id = 0; c.target = DEV0_MEM;
    c.value = 0; c.flags = DRV_FLAG_REQUIRE_AUDIT;
    r = secos_driver(&c);
    i = 0; i = put_str(line,i,"["); i = put_str(line,i,tag);
    i = put_str(line,i,"] MAP_MEM ret="); i = put_dec(line,i,r);
    i = put_str(line,i," (want deny)"); line[i++]='\n';
    write(1, line, i);

    i = 0; i = put_str(line,i,"["); i = put_str(line,i,tag);
    i = put_str(line,i,"] done"); line[i++]='\n';
    write(1, line, i);
    return 0;
}
