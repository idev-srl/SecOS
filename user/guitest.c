/* SecOS - GUI kernel interface regression test (selftest M40 gates).
 * SPDX-License-Identifier: MIT
 *
 * One signed binary, several modes (driven by tools/gui_drive.py):
 *   guitest fb     /dev/fb0: screeninfo ioctls, FBIOPUT refuses a mode change,
 *                  mmap round-trips pixels (written through the mapping, read
 *                  back through read()), out-of-range / PROT_EXEC / MAP_PRIVATE
 *                  maps refused, mprotect(PROT_EXEC) refused, a fork child's
 *                  writes are visible to the parent (shared device map), and
 *                  20 map/unmap cycles + a forked child leave MemFree unchanged.
 *   guitest input  opens every /dev/input/eventN, prints names + capability
 *                  bits, then prints every event until KEY_Q is pressed; a
 *                  flood stage checks overflow -> SYN_DROPPED.
 *   guitest gfx [crash|text]  KDSETMODE(KD_GRAPHICS), paints a test pattern,
 *                  then exits without KD_TEXT (or crashes / restores itself).
 *   guitest keys   raw console: prints the bytes of each key until 'q'.
 *   guitest nocap  run from the copy signed WITHOUT CAP_INPUT/CAP_DISPLAY:
 *                  fb0, event nodes and KD_GRAPHICS must all be refused.
 * Every line starts with a G?- marker the harness greps for.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <termios.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include "libsecos.h"

static int fails;
static void ok(const char* tag, int cond) {
    printf("%s-%s\n", tag, cond ? "OK" : "FAIL");
    if (!cond) fails++;
    fflush(stdout);
}

static long meminfo_free_kb(void) {
    char buf[1024];
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return -1;
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    char* p = strstr(buf, "MemFree:");
    if (!p) return -1;
    p += 8;
    while (*p == ' ') p++;
    return atol(p);
}

/* ------------------------------------------------------------------ fb */
static int t_fb(void) {
    int fd = open("/dev/fb0", O_RDWR);
    ok("GF-OPEN", fd >= 0);
    if (fd < 0) return 1;
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    memset(&v, 0, sizeof(v)); memset(&f, 0, sizeof(f));
    int r1 = ioctl(fd, FBIOGET_VSCREENINFO, &v);
    int r2 = ioctl(fd, FBIOGET_FSCREENINFO, &f);
    printf("GF-INFO %ux%u bpp=%u line=%u smem=%u red=%u/%u green=%u/%u blue=%u/%u visual=%u id=%s\n",
           v.xres, v.yres, v.bits_per_pixel, f.line_length, f.smem_len,
           v.red.offset, v.red.length, v.green.offset, v.green.length, v.blue.offset, v.blue.length,
           f.visual, f.id);
    ok("GF-IOCTL", r1 == 0 && r2 == 0 && v.xres > 0 && v.yres > 0 && v.bits_per_pixel == 32 &&
                   f.line_length >= v.xres * 4 && f.smem_len >= f.line_length * v.yres &&
                   f.visual == FB_VISUAL_TRUECOLOR && f.smem_start == 0 &&
                   v.red.offset == 16 && v.red.length == 8 && v.green.offset == 8 &&
                   v.green.length == 8 && v.blue.offset == 0 && v.blue.length == 8);
    /* FBIOPUT: same mode accepted, a different resolution refused cleanly. */
    struct fb_var_screeninfo w = v;
    int same = ioctl(fd, FBIOPUT_VSCREENINFO, &w);
    w = v; w.xres = v.xres / 2; w.xres_virtual = w.xres;
    errno = 0;
    int diff = ioctl(fd, FBIOPUT_VSCREENINFO, &w);
    ok("GF-PUTVAR", same == 0 && diff == -1 && errno == EINVAL);
    ok("GF-NOTTY", ioctl(fd, TCGETS, &w) == -1);

    size_t len = f.smem_len;
    uint8_t* fb = secos_mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ok("GF-MMAP", fb != MAP_FAILED);
    if (fb == MAP_FAILED) return 1;
    /* Write a pattern through the mapping, read it back through read(). */
    uint32_t y0 = v.yres / 2, x0 = 16;
    uint32_t* row = (uint32_t*)(fb + (size_t)y0 * f.line_length);
    for (uint32_t x = 0; x < 64; x++) row[x0 + x] = 0x00A0B0C0u + x;
    uint32_t back[64];
    lseek(fd, (long)y0 * f.line_length + x0 * 4, SEEK_SET);
    int rn = (int)read(fd, back, sizeof(back));
    int same_px = (rn == (int)sizeof(back));
    for (int x = 0; x < 64 && same_px; x++) if (back[x] != 0x00A0B0C0u + (uint32_t)x) same_px = 0;
    ok("GF-MMAP-RW", same_px);
    /* ...and write() is visible through the mapping. */
    uint32_t pat[4] = { 0x00112233u, 0x00445566u, 0x00778899u, 0x00AABBCCu };
    lseek(fd, (long)(y0 + 1) * f.line_length, SEEK_SET);
    int wn = (int)write(fd, pat, sizeof(pat));
    uint32_t* row1 = (uint32_t*)(fb + (size_t)(y0 + 1) * f.line_length);
    ok("GF-WRITE-MMAP", wn == (int)sizeof(pat) && row1[0] == pat[0] && row1[3] == pat[3]);

    /* Refusals: beyond the framebuffer, unaligned offset, PROT_EXEC, MAP_PRIVATE. */
    errno = 0;
    void* a = secos_mmap(NULL, len + 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int e_a = errno;
    void* b = secos_mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, (long)len);
    void* c = secos_mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 100);
    void* d = secos_mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    int e_d = errno;
    void* e = secos_mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    void* g = secos_mmap(NULL, 8192, PROT_READ, MAP_SHARED, fd, (long)len - 4096);
    printf("GF-RANGE big=%d end=%d unaligned=%d exec=%d(errno %d) private=%d straddle=%d\n",
           a == MAP_FAILED, b == MAP_FAILED, c == MAP_FAILED, d == MAP_FAILED, e_d,
           e == MAP_FAILED, g == MAP_FAILED);
    ok("GF-MMAP-RANGE", a == MAP_FAILED && e_a == EINVAL && b == MAP_FAILED && c == MAP_FAILED &&
                        d == MAP_FAILED && e_d == EACCES && e == MAP_FAILED && g == MAP_FAILED);
    /* A valid sub-range at an offset maps the right pixels. */
    uint8_t* sub = secos_mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, (long)(y0 * f.line_length) & ~4095L);
    uint32_t want = row[x0];
    size_t inpage = ((size_t)y0 * f.line_length + x0 * 4) & 4095;
    ok("GF-MMAP-OFFSET", sub != MAP_FAILED && *(uint32_t*)(sub + inpage) == want);
    if (sub != MAP_FAILED) munmap(sub, 4096);
    ok("GF-MPROTECT-EXEC", mprotect(fb, 4096, PROT_READ | PROT_EXEC) != 0);
    { int pr = mprotect(fb + 8192, 4096, PROT_READ); volatile uint8_t rd = fb[8192 + 1]; (void)rd; ok("GF-MPROTECT-RO", pr == 0); }
    mprotect(fb + 8192, 4096, PROT_READ | PROT_WRITE);

    /* Shared across fork: the child's pixel write is seen by the parent. */
    row[x0 + 100] = 0;
    int pid = fork();
    if (pid == 0) { row[x0 + 100] = 0x00C0FFEEu; _exit(0); }
    if (pid > 0) waitpid(pid);
    ok("GF-FORK-SHARED", pid > 0 && row[x0 + 100] == 0x00C0FFEEu);
    munmap(fb, len);

    /* No PMM accounting drift: the framebuffer frames are not PMM-owned, so
     * mapping/unmapping/exiting must leave MemFree exactly where it was. */
    long before = meminfo_free_kb();
    for (int i = 0; i < 20; i++) {
        uint8_t* m = secos_mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) { before = -2; break; }
        m[i * 4] = (uint8_t)i;
        if (i == 10) {                        /* a child inherits the shared map, exits */
            int p2 = fork();
            if (p2 == 0) { m[0] = 1; _exit(0); }
            if (p2 > 0) waitpid(p2);
        }
        if (i % 2 == 0) munmap(m, len);       /* odd ones stay mapped until exit */
    }
    long mid = meminfo_free_kb();
    printf("GF-MEMFREE before=%ld after=%ld\n", before, mid);
    /* Mapping the fb costs page tables only (a few KB); 20 cycles must not
     * free or leak hundreds of frames. */
    long diff_kb = before - mid; if (diff_kb < 0) diff_kb = -diff_kb;
    ok("GF-NOLEAK", before > 0 && mid > 0 && diff_kb < 512);
    close(fd);
    return 0;
}

/* --------------------------------------------------------------- input */
#define NBITS(x) ((((x) - 1) / (8 * sizeof(long))) + 1)
static int test_bit(int bit, const unsigned long* a) { return (a[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1; }

static int open_inputs(int* fds, char names[][64], int max) {
    int n = 0;
    for (int i = 0; i < 8 && n < max; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        names[n][0] = 0;
        ioctl(fd, EVIOCGNAME(64), names[n]);
        unsigned long ev[NBITS(EV_CNT)] = { 0 }, rel[NBITS(REL_CNT)] = { 0 }, abs_[NBITS(ABS_CNT)] = { 0 };
        unsigned long key[NBITS(KEY_CNT)] = { 0 };
        ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key);
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_)), abs_);
        struct input_id id; memset(&id, 0, sizeof(id));
        ioctl(fd, EVIOCGID, &id);
        printf("GI-DEV event%d \"%s\" bus=0x%x ev=0x%lx key_a=%d btn_left=%d rel_x=%d wheel=%d abs_x=%d\n",
               i, names[n], id.bustype, ev[0], test_bit(KEY_A, key), test_bit(BTN_LEFT, key),
               test_bit(REL_X, rel), test_bit(REL_WHEEL, rel), test_bit(ABS_X, abs_));
        if (test_bit(ABS_X, abs_)) {
            struct input_absinfo ai; memset(&ai, 0, sizeof(ai));
            int r = ioctl(fd, EVIOCGABS(ABS_X), &ai);
            printf("GI-ABS event%d r=%d min=%d max=%d\n", i, r, ai.minimum, ai.maximum);
        }
        fds[n++] = fd;
    }
    fflush(stdout);
    return n;
}

static int t_input(void) {
    int fds[8]; char names[8][64];
    int n = open_inputs(fds, names, 8);
    ok("GI-OPEN", n >= 2);
    /* Short reads are refused: whole events only. */
    char small[8];
    errno = 0;
    ok("GI-SHORTREAD", n > 0 && read(fds[0], small, sizeof(small)) == -1 && errno == EINVAL);
    /* Grab the keyboards for the event phase: the test's keys must not also
     * reach the console (bash) underneath. */
    int kbd[8] = { 0 };
    for (int i = 0; i < n; i++) {
        unsigned long key[NBITS(KEY_CNT)] = { 0 };
        ioctl(fds[i], EVIOCGBIT(EV_KEY, sizeof(key)), key);
        if (test_bit(KEY_Q, key) && ioctl(fds[i], EVIOCGRAB, (void*)1) == 0) kbd[i] = 1;
    }
    printf("GI-READY\n"); fflush(stdout);
    int done = 0, seen_syn = 0;
    while (!done) {
        struct pollfd pf[8];
        for (int i = 0; i < n; i++) { pf[i].fd = fds[i]; pf[i].events = POLLIN; pf[i].revents = 0; }
        int r = poll(pf, (nfds_t)n, 30000);
        if (r <= 0) { printf("GI-TIMEOUT\n"); break; }
        for (int i = 0; i < n; i++) {
            if (!(pf[i].revents & POLLIN)) continue;
            struct input_event evs[32];
            int got = (int)read(fds[i], evs, sizeof(evs));
            for (int k = 0; got > 0 && k < got / (int)sizeof(evs[0]); k++) {
                struct input_event* e = &evs[k];
                if (e->type == EV_SYN) { seen_syn = 1; continue; }
                printf("GI-EV \"%s\" type=%u code=%u value=%d\n", names[i], e->type, e->code, e->value);
                if (e->type == EV_KEY && e->code == KEY_Q && e->value == 1) done = 1;
            }
        }
        fflush(stdout);
    }
    ok("GI-SYN", seen_syn);
    for (int i = 0; i < n; i++) if (kbd[i]) ioctl(fds[i], EVIOCGRAB, (void*)0);
    /* Flood: stop reading while the harness generates > 256 events, then
     * expect SYN_DROPPED at the head of the queue. */
    for (int i = 0; i < n; i++) { struct input_event e[64]; while (read(fds[i], e, sizeof(e)) > 0) { } }
    printf("GI-FLOOD-READY\n"); fflush(stdout);
    sleep(8);
    int dropped = 0, after = 0;
    for (int i = 0; i < n; i++) {
        struct input_event e[64];
        int got;
        while ((got = (int)read(fds[i], e, sizeof(e))) > 0) {
            for (int k = 0; k < got / (int)sizeof(e[0]); k++) {
                if (e[k].type == EV_SYN && e[k].code == SYN_DROPPED) dropped = 1;
                else if (dropped) after++;
            }
        }
    }
    printf("GI-FLOOD dropped=%d after=%d\n", dropped, after);
    ok("GI-SYN-DROPPED", dropped && after > 0);
    /* EVIOCGRAB: exclusive, a second grab elsewhere is busy, release works. */
    int g1 = ioctl(fds[0], EVIOCGRAB, (void*)1);
    char p0[32]; snprintf(p0, sizeof(p0), "/dev/input/event0");
    int other = open(p0, O_RDONLY | O_NONBLOCK);
    errno = 0;
    int g2 = other >= 0 ? ioctl(other, EVIOCGRAB, (void*)1) : 0;
    int e2 = errno;
    int g3 = ioctl(fds[0], EVIOCGRAB, (void*)0);
    ok("GI-GRAB", g1 == 0 && g2 == -1 && e2 == EBUSY && g3 == 0);
    if (other >= 0) close(other);
    for (int i = 0; i < n; i++) close(fds[i]);
    printf("GI-DONE fails=%d\n", fails); fflush(stdout);
    return 0;
}

/* ----------------------------------------------------------------- gfx */
static int t_gfx(const char* how) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { printf("GG-NOFB\n"); return 1; }
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    ioctl(fd, FBIOGET_VSCREENINFO, &v); ioctl(fd, FBIOGET_FSCREENINFO, &f);
    uint8_t* fb = secos_mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { printf("GG-NOMAP\n"); return 1; }
    int r = ioctl(0, KDSETMODE, (void*)KD_GRAPHICS);
    int mode = -1; ioctl(0, KDGETMODE, &mode);
    ok("GG-KDGRAPHICS", r == 0 && mode == KD_GRAPHICS);
    /* Four colour quadrants; a magenta square in the middle. */
    for (uint32_t y = 0; y < v.yres; y++) {
        uint32_t* row = (uint32_t*)(fb + (size_t)y * f.line_length);
        for (uint32_t x = 0; x < v.xres; x++) {
            uint32_t c = (y < v.yres / 2) ? (x < v.xres / 2 ? 0x00FF0000u : 0x0000FF00u)
                                          : (x < v.xres / 2 ? 0x000000FFu : 0x00FFFF00u);
            if (x > v.xres / 2 - 50 && x < v.xres / 2 + 50 && y > v.yres / 2 - 50 && y < v.yres / 2 + 50)
                c = 0x00FF00FFu;
            row[x] = c;
        }
    }
    /* Kernel console output while in graphics mode must NOT reach the screen. */
    printf("GG-DRAWN %ux%u\n", v.xres, v.yres); fflush(stdout);
    for (int i = 0; i < 20; i++) printf("GG-CONSOLE-NOISE %d\n", i);
    fflush(stdout);
    sleep(6);                                      /* harness: screendump now */
    if (how && strcmp(how, "crash") == 0) {
        printf("GG-CRASHING\n"); fflush(stdout);
        volatile int* np = (volatile int*)0;
        *np = 42;                                  /* SIGSEGV: the kernel must restore text */
    }
    if (how && strcmp(how, "text") == 0) {
        ioctl(0, KDSETMODE, (void*)KD_TEXT);
        printf("GG-TEXT\n");
    }
    printf("GG-EXIT\n"); fflush(stdout);
    return 0;                                      /* exit still in KD_GRAPHICS */
}

/* ---------------------------------------------------------------- keys */
static int t_keys(void) {
    struct termios old, raw;
    tcgetattr(0, &old);
    raw = old; cfmakeraw(&raw);
    tcsetattr(0, TCSANOW, &raw);
    char line[512]; int lp = 0;
    char lay[16] = { 0 };
    ioctl(0, KDGKBLAYOUT, lay);
    printf("GK-READY layout=%s\r\n", lay); fflush(stdout);
    for (;;) {
        unsigned char c;
        if (read(0, &c, 1) != 1) break;
        if (c == 'q') break;
        if (lp < (int)sizeof(line) - 4) lp += snprintf(line + lp, sizeof(line) - lp, "%02x", c);
    }
    tcsetattr(0, TCSANOW, &old);
    line[lp] = 0;
    printf("GK-BYTES %s\n", line); fflush(stdout);
    return 0;
}

/* --------------------------------------------------------------- nocap */
static int t_nocap(void) {
    errno = 0;
    int fd = open("/dev/fb0", O_RDWR);
    int e1 = errno;
    errno = 0;
    int in = open("/dev/input/event0", O_RDONLY);
    int e2 = errno;
    errno = 0;
    int kd = ioctl(0, KDSETMODE, (void*)KD_GRAPHICS);
    int e3 = errno;
    printf("GN-RESULT fb=%d/%d input=%d/%d kd=%d/%d\n", fd, e1, in, e2, kd, e3);
    ok("GN-FB-DENIED", fd == -1 && e1 == EACCES);
    ok("GN-INPUT-DENIED", in == -1 && e2 == EACCES);
    ok("GN-KD-DENIED", kd == -1 && e3 == EPERM);
    return 0;
}

int main(int argc, char** argv) {
    const char* m = argc > 1 ? argv[1] : "fb";
    int r = 1;
    if (!strcmp(m, "fb")) r = t_fb();
    else if (!strcmp(m, "input")) r = t_input();
    else if (!strcmp(m, "gfx")) r = t_gfx(argc > 2 ? argv[2] : NULL);
    else if (!strcmp(m, "keys")) r = t_keys();
    else if (!strcmp(m, "nocap")) r = t_nocap();
    printf("G-END %s fails=%d\n", m, fails); fflush(stdout);
    return r ? r : (fails ? 1 : 0);
}
