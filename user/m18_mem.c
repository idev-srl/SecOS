/* SecOS M18: exercises dynamic memory — malloc/free/reuse over sbrk, anonymous
 * mmap, mprotect (read-only enforcement + W^X rejection). Finishes by writing to
 * a read-only page, which must terminate the process (proving mprotect enforces).
 * SPDX-License-Identifier: MIT */
#include "libsecos.h"

int main(void) {
    puts("[m18] dynamic memory test");

    /* 1. malloc a multi-page buffer (forces sbrk growth + demand faults). */
    const int n = 4096 * 3;
    char* a = (char*)malloc((size_t)n);
    if (!a) { puts("[m18] malloc FAILED"); return 1; }
    for (int i = 0; i < n; i++) a[i] = (char)(i & 0xff);
    int ok = 1;
    for (int i = 0; i < n; i++) if (a[i] != (char)(i & 0xff)) ok = 0;
    puts(ok ? "[m18] malloc rw OK" : "[m18] malloc rw FAIL");

    /* 2. free + malloc again -> the free list reuses the same block. */
    free(a);
    char* b = (char*)malloc((size_t)n);
    puts(b == a ? "[m18] free+reuse OK" : "[m18] free+reuse FAIL");
    free(b);

    /* 3. anonymous mmap, write across two pages, read back. */
    char* m = (char*)mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE);
    if ((long)m == -1L) { puts("[m18] mmap FAILED"); return 1; }
    m[0] = 42; m[4096] = 7;
    puts((m[0] == 42 && m[4096] == 7) ? "[m18] mmap rw OK" : "[m18] mmap rw FAIL");

    /* 4. W^X must be rejected by mmap and mprotect. */
    char* wx = (char*)mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS);
    puts((long)wx == -1L ? "[m18] mmap W+X refused OK" : "[m18] mmap W+X FAIL");
    puts(mprotect(m, 8192, PROT_READ | PROT_WRITE | PROT_EXEC) < 0
         ? "[m18] mprotect W+X refused OK" : "[m18] mprotect W+X FAIL");

    /* 5. mprotect to read-only; reads still work. */
    puts(mprotect(m, 8192, PROT_READ) == 0 ? "[m18] mprotect RO OK" : "[m18] mprotect FAIL");
    puts(m[0] == 42 ? "[m18] read-after-RO OK" : "[m18] read-after-RO FAIL");

    puts("[m18] DONE-USER");

    /* 6. Writing to the now read-only page must terminate this process. */
    puts("[m18] writing to RO page (should be killed)");
    m[0] = 99;
    puts("[m18] STILL ALIVE after RO write — FAIL");
    return 0;
}
