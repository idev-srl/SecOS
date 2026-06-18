/* SecOS M20: file-backed mmap through the unified page cache. Opens a VFS file,
 * maps it read-only (MAP_PRIVATE), checks the mapped bytes match the file, then
 * read()s the same file and confirms read() and mmap see identical bytes (they
 * share cache pages -> coherent). SPDX-License-Identifier: MIT */
#include "libsecos.h"

static const char EXP[] = "M20-PAGE-CACHE-OK";

int main(void) {
    puts("[m20] file-backed mmap + page cache");
    int fd = open("/m20.txt", 0 /*O_RDONLY*/);
    if (fd < 0) { puts("[m20] open FAILED"); return 1; }
    int n = (int)sizeof(EXP) - 1;

    char* m = (char*)mmap_file(0, 4096, PROT_READ, MAP_PRIVATE, fd);
    if ((long)m == -1L) { puts("[m20] mmap FAILED"); return 1; }
    int mok = 1; for (int i = 0; i < n; i++) if (m[i] != EXP[i]) mok = 0;
    puts(mok ? "[m20] mmap content OK" : "[m20] mmap content FAIL");

    char buf[64]; for (int i = 0; i < 64; i++) buf[i] = 0;
    int r = (int)read(fd, buf, n);
    int rok = (r == n); for (int i = 0; i < n; i++) if (buf[i] != EXP[i]) rok = 0;
    puts(rok ? "[m20] read content OK" : "[m20] read content FAIL");

    int coh = 1; for (int i = 0; i < n; i++) if (m[i] != buf[i]) coh = 0;
    puts(coh ? "[m20] read/mmap coherent OK" : "[m20] coherent FAIL");

    close(fd);
    puts("[m20] DONE-USER");
    return 0;
}
