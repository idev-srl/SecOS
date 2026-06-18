/*
 * m23_fs.c — [M23] signed user program exercising the POSIX FS personality.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Proves a signed ring-3 program can use the Linux-style filesystem: read
 * /dev/zero, write /dev/null, read /proc/uptime, stat() and lseek() a block
 * device node under /dev. Output goes to fd 1 (console + debugcon, "[m23fs]").
 */
#include "libsecos.h"

static void putnum(unsigned long v) {
    char b[24]; int n = 0;
    if (!v) { write(1, "0", 1); return; }
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n--) write(1, &b[n], 1);
}

int main(void) {
    char buf[64];

    /* /dev/zero: a read must return all-zero bytes. */
    int fd = open("/dev/zero", O_RDONLY);
    if (fd >= 0) {
        for (int i = 0; i < 16; i++) buf[i] = (char)0xAA;
        int r = read(fd, buf, 16);
        int ok = (r == 16);
        for (int i = 0; i < 16; i++) if (buf[i] != 0) ok = 0;
        close(fd);
        puts(ok ? "[m23fs] /dev/zero read OK" : "[m23fs] /dev/zero read FAIL");
    } else puts("[m23fs] open /dev/zero FAIL");

    /* /dev/null: a write is consumed. */
    fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) {
        int r = write(fd, "hello", 5);
        close(fd);
        puts(r == 5 ? "[m23fs] /dev/null write OK" : "[m23fs] /dev/null write FAIL");
    }

    /* /proc/uptime: generated text. */
    fd = open("/proc/uptime", O_RDONLY);
    if (fd >= 0) {
        int r = read(fd, buf, sizeof(buf) - 1);
        if (r > 0) { buf[r] = 0; write(1, "[m23fs] /proc/uptime=", 21); write(1, buf, r); }
        close(fd);
    }

    /* stat() + lseek() on a block-device node, if a disk is present. */
    struct stat st;
    if (stat("/dev/vda", &st) == 0 || stat("/dev/sda", &st) == 0 ||
        stat("/dev/nvme0n1", &st) == 0 || stat("/dev/usb0", &st) == 0) {
        write(1, "[m23fs] stat block dev size=", 28); putnum(st.st_size); write(1, "\n", 1);
        const char* bn = stat("/dev/vda", &st) == 0 ? "/dev/vda" :
                         stat("/dev/sda", &st) == 0 ? "/dev/sda" :
                         stat("/dev/nvme0n1", &st) == 0 ? "/dev/nvme0n1" : "/dev/usb0";
        fd = open(bn, O_RDONLY);
        if (fd >= 0) {
            long end = lseek(fd, 0, SEEK_END);
            write(1, "[m23fs] lseek END=", 18); putnum((unsigned long)end); write(1, "\n", 1);
            close(fd);
        }
    } else puts("[m23fs] no block device node (no disk)");

    puts("[m23fs] DONE");
    return 0;
}
