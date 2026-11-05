/* SecOS - system information panel, an SWP client (/bin/sysinfo).
 * SPDX-License-Identifier: MIT
 *
 * Started from the secwm panel ("System"). Signed with the confined manifest
 * (CAP_ENFORCE | CAP_FS_READ | CAP_TIME): it can read /proc and draw into its
 * own window, nothing else. Shows uptime, memory, load and the process list,
 * refreshed on every compositor tick (1 Hz).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include "libsecos.h"
#include "swpc.h"

#define WW 420
#define WH 360
#define BG    0x141B23u
#define CARD  0x1C2631u
#define TEXT  0xE6EDF3u
#define DIM   0x8A97A6u
#define ACC   0x2BB3A8u
#define ACC2  0xF0B429u

static int read_file(const char* path, char* buf, int max) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = (int)read(fd, buf, (size_t)max - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = 0;
    return n;
}

static long meminfo(const char* buf, const char* key) {
    const char* p = strstr(buf, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    return strtol(p, 0, 10);
}

static void bar(surf_t* s, int x, int y, int w, int h, long used, long total, uint32_t c) {
    gfx_rrect(s, x, y, w, h, 4, 0x0E141A);
    if (total > 0) {
        int f = (int)((long long)w * used / total);
        if (f > 3) gfx_rrect(s, x, y, f, h, 4, c);
    }
}

struct proc { int pid; char comm[24]; char state; long rss; };

static int list_procs(struct proc* out, int max) {
    DIR* d = opendir("/proc");
    if (!d) return 0;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) && n < max) {
        int pid = atoi(e->d_name);
        if (pid <= 0) continue;
        char path[64], buf[512];
        snprintf(path, sizeof(path), "/proc/%d/comm", pid);
        if (read_file(path, buf, sizeof(buf)) <= 0) continue;
        char* nl = strchr(buf, '\n'); if (nl) *nl = 0;
        out[n].pid = pid;
        snprintf(out[n].comm, sizeof(out[n].comm), "%s", buf);
        out[n].state = '?'; out[n].rss = 0;
        snprintf(path, sizeof(path), "/proc/%d/status", pid);
        if (read_file(path, buf, sizeof(buf)) > 0) {
            char* st = strstr(buf, "State:");
            if (st) { st += 6; while (*st == ' ' || *st == '\t') st++; out[n].state = *st; }
            long r = meminfo(buf, "VmRSS");
            if (r >= 0) out[n].rss = r;
        }
        n++;
    }
    closedir(d);
    return n;
}

static void paint(swpc_t* c) {
    surf_t* s = &c->surf;
    char buf[2048], line[96];
    gfx_fill(s, 0, 0, c->w, c->h, BG);
    gfx_text(s, 14, 10, "SecOS system", TEXT, 1, 0);
    if (read_file("/proc/version", buf, sizeof(buf)) > 0) {
        char* nl = strchr(buf, '\n'); if (nl) *nl = 0;
        gfx_text(s, 14, 28, buf, DIM, 0, c->w - 28);
    }
    /* uptime + load */
    gfx_rrect(s, 10, 50, c->w - 20, 44, 6, CARD);
    double up = 0;
    if (read_file("/proc/uptime", buf, sizeof(buf)) > 0) up = strtod(buf, 0);
    long u = (long)up;
    snprintf(line, sizeof(line), "Uptime  %ldd %02ld:%02ld:%02ld", u / 86400, (u / 3600) % 24, (u / 60) % 60, u % 60);
    gfx_text(s, 20, 55, line, TEXT, 0, 0);
    if (read_file("/proc/loadavg", buf, sizeof(buf)) > 0) {
        char* nl = strchr(buf, '\n'); if (nl) *nl = 0;
        snprintf(line, sizeof(line), "Load    %s", buf);
        gfx_text(s, 20, 73, line, DIM, 0, c->w - 40);
    }
    /* memory */
    gfx_rrect(s, 10, 100, c->w - 20, 50, 6, CARD);
    if (read_file("/proc/meminfo", buf, sizeof(buf)) > 0) {
        long tot = meminfo(buf, "MemTotal"), fr = meminfo(buf, "MemFree");
        long av = meminfo(buf, "MemAvailable");
        if (av < 0) av = fr;
        long used = tot - av;
        snprintf(line, sizeof(line), "Memory  %ld / %ld MB used", used / 1024, tot / 1024);
        gfx_text(s, 20, 105, line, TEXT, 0, 0);
        bar(s, 20, 127, c->w - 40, 12, used, tot, ACC);
    }
    /* processes */
    struct proc pr[64];
    int n = list_procs(pr, 64);
    gfx_rrect(s, 10, 156, c->w - 20, c->h - 166, 6, CARD);
    snprintf(line, sizeof(line), "Processes  %d", n);
    gfx_text(s, 20, 161, line, TEXT, 1, 0);
    gfx_text(s, 20, 181, "  PID  S  RSS(kB)  COMMAND", ACC, 0, 0);
    int y = 199;
    for (int i = 0; i < n && y + 16 <= c->h - 12; i++, y += 16) {
        snprintf(line, sizeof(line), "%5d  %c  %7ld  %s", pr[i].pid, pr[i].state, pr[i].rss, pr[i].comm);
        gfx_text(s, 20, y, line, pr[i].state == 'R' ? ACC2 : TEXT, 0, c->w - 40);
    }
    swpc_commit(c, 0, 0, c->w, c->h);
}

int main(void) {
    swpc_t c;
    if (swpc_open(&c, WW, WH, "System information") < 0) {
        printf("sysinfo: not started by secwm (needs the SWP pipes on fds 3/4)\n");
        return 1;
    }
    paint(&c);
    for (;;) {
        struct swpc_event e;
        int r = swpc_wait(&c, &e, 3000);
        if (r < 0) break;
        if (r == 0 || e.type == SWP_EV_FRAME) paint(&c);
        else if (e.type == SWP_EV_CLOSE) break;
        else if (e.type == SWP_EV_KEY && e.u.key.value == 1 && (e.u.key.cp == 'q' || e.u.key.code == 1)) break;
    }
    return 0;
}
