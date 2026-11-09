/* SecOS - Task Manager, an LVGL client of the secwm compositor (/bin/taskmgr).
 * SPDX-License-Identifier: MIT
 *
 * Modelled on the Windows Task Manager:
 *   Processes   - sortable list (name, PID, user, status, threads, CPU %,
 *                 memory, command line); "End task" sends SIGTERM after a
 *                 confirmation and offers SIGKILL when the process is still
 *                 there 3 s later. Ending another user's process is refused
 *                 by the kernel (cred_may_signal, probed with kill(pid, 0))
 *                 and the UI says so.
 *   Performance - one CPU graph per online CPU (/proc/stat), total CPU,
 *                 memory (/proc/meminfo), receive/send rate per NIC
 *                 (/proc/net/dev), uptime, process count.
 *   Users       - processes, CPU and memory grouped by user.
 * Refreshes once a second. Signed with note_taskmgr.S (CAP_ENFORCE |
 * CAP_FS_READ | CAP_SIGNAL | CAP_TIME): it reads /proc and /etc/passwd,
 * draws its window and sends signals, nothing else.
 *
 * Rendering: LVGL 9.2 in DIRECT mode into the window buffer; the lists and
 * graphs are custom-drawn objects (LV_EVENT_DRAW_MAIN), invalidated only
 * where something changed, so the 1 Hz update sends little through the SWP
 * pipe. Each flushed area goes out as its own PUT.
 *
 *   taskmgr [--max WxH] [--trace]
 * secwm passes --max (the largest window the screen takes) and --trace when
 * it traces itself; --trace prints TASKMGR-* markers for the selftest.
 *
 * Keys: Up/Down/PgUp/PgDn/Home/End select, Delete = End task, typing a name
 * selects the first process that starts with it, Ctrl+Tab next tab, F5
 * refresh now, Enter/Esc answer a dialog.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <linux/input.h>
#include "libsecos.h"
#include "swpc.h"
#include "lvgl.h"

/* ---- configuration ----------------------------------------------------- */
#define DEF_W   880
#define DEF_H   600
#define TAB_H   40
#define STAT_H  26
#define FOOT_H  48
#define HDR_H   40
#define ROW_H   24
#define HIST    60
#define MAXP    64
#define MAXCPU  64
#define MAXNIC  4
#define MAXUSER 32
#define NCOL    8

#define C_BG       0x12171E
#define C_PANEL    0x182029
#define C_HDR      0x1E2731
#define C_LINE     0x2A3542
#define C_ROW2     0x151C24
#define C_HOVER    0x1F2A36
#define C_SEL      0x1D4B73
#define C_TEXT     0xE6EDF3
#define C_DIM      0x8A97A6
#define C_FAINT    0x5C6A79
#define C_ACCENT   0x2BB3A8
#define C_HEAT     0xE0A526
#define C_CPU      0x3FA4F0
#define C_MEM      0xA97CF0
#define C_NET      0xE89A3C
#define C_NET2     0xF3CE8E
#define C_DANGER   0xD9474D

static swpc_t c;
static int WW = DEF_W, WH = DEF_H;
static int trace;
static int ptr_x, ptr_y, ptr_down;
static int my_uid;
static char my_name[32] = "?";

static uint32_t tick_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

#define T(...) do { if (trace) { printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- small helpers --------------------------------------------------------- */
static int read_file(const char* path, char* buf, int max) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = 0;
    while (n < max - 1) {
        int r = (int)read(fd, buf + n, (size_t)(max - 1 - n));
        if (r <= 0) break;
        n += r;
    }
    close(fd);
    buf[n] = 0;
    return n;
}
static long kv(const char* buf, const char* key) {
    const char* p = strstr(buf, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    return strtol(p, 0, 10);
}
static int lower(int ch) { return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch; }
static int cmp_ci(const char* a, const char* b) {
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}
static void fmt_kb(char* out, int n, long kb) {          /* Windows-style memory column */
    if (kb < 1024) snprintf(out, (size_t)n, "%ld K", kb);
    else if (kb < 1024 * 1024) snprintf(out, (size_t)n, "%ld.%ld MB", kb / 1024, (kb % 1024) * 10 / 1024);
    else snprintf(out, (size_t)n, "%ld.%ld GB", kb / (1024 * 1024), (kb % (1024 * 1024)) * 10 / (1024 * 1024));
}
static void fmt_bits(char* out, int n, uint64_t bytes_s) {   /* a rate, in bits per second */
    uint64_t b = bytes_s * 8;
    if (b < 1000000) snprintf(out, (size_t)n, "%lu Kbps", (unsigned long)((b + 999) / 1000));
    else if (b < 1000000000ull) snprintf(out, (size_t)n, "%lu.%lu Mbps", (unsigned long)(b / 1000000), (unsigned long)(b % 1000000 / 100000));
    else snprintf(out, (size_t)n, "%lu.%lu Gbps", (unsigned long)(b / 1000000000ull), (unsigned long)(b % 1000000000ull / 100000000ull));
}
static void fmt_bytes(char* out, int n, uint64_t b) {
    if (b < 1024) snprintf(out, (size_t)n, "%lu B", (unsigned long)b);
    else if (b < 1024 * 1024) snprintf(out, (size_t)n, "%lu.%lu KB", (unsigned long)(b / 1024), (unsigned long)((b % 1024) * 10 / 1024));
    else if (b < 1024ull * 1024 * 1024) snprintf(out, (size_t)n, "%lu.%lu MB", (unsigned long)(b >> 20), (unsigned long)((b & 0xFFFFF) * 10 >> 20));
    else snprintf(out, (size_t)n, "%lu.%lu GB", (unsigned long)(b >> 30), (unsigned long)((b & 0x3FFFFFFF) * 10 >> 30));
}
static void fmt_pct10(char* out, int n, int x10) { snprintf(out, (size_t)n, "%d.%d%%", x10 / 10, x10 % 10); }
static void fmt_uptime(char* out, int n, long s) {
    snprintf(out, (size_t)n, "%ld:%02ld:%02ld:%02ld", s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60);
}

/* ---- users (/etc/passwd) ----------------------------------------------- */
static struct { int uid; char name[24]; } g_users[MAXUSER];
static int g_nusers;
static void load_users(void) {
    static char buf[8192];
    if (read_file("/etc/passwd", buf, sizeof(buf)) <= 0) return;
    g_nusers = 0;
    for (char* line = buf; *line && g_nusers < MAXUSER;) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char* f[4] = { line, 0, 0, 0 };
        int k = 1;
        for (char* p = line; *p && k < 4; p++) if (*p == ':') { *p = 0; f[k++] = p + 1; }
        if (k >= 3 && f[0][0]) {
            g_users[g_nusers].uid = atoi(f[2]);
            snprintf(g_users[g_nusers].name, sizeof(g_users[0].name), "%s", f[0]);
            g_nusers++;
        }
        if (!nl) break;
        line = nl + 1;
    }
}
static const char* user_name(int uid, char* tmp, int n) {
    for (int i = 0; i < g_nusers; i++) if (g_users[i].uid == uid) return g_users[i].name;
    snprintf(tmp, (size_t)n, "%d", uid);
    return tmp;
}

/* ---- sampling ---------------------------------------------------------------- */
typedef struct {
    int pid, ppid, uid;
    int threads;               /* [U9] /proc/<pid>/stat num_threads */
    char state;
    char comm[32];
    char cmd[96];
    uint64_t ticks, start;     /* utime+stime, starttime (USER_HZ) */
    long rss_kb;
    int cpu10;                 /* % of the whole machine x10 */
} proc_t;
static proc_t procs[MAXP]; static int nprocs;
static struct { int pid; uint64_t start, ticks; } prevp[MAXP]; static int nprevp;
static uint32_t prev_ms;

static int ncpu;                                    /* online CPUs (/proc/stat cpuN lines) */
static uint64_t cpu_busy_prev[MAXCPU + 1], cpu_tot_prev[MAXCPU + 1];
static int cpu10[MAXCPU + 1];                       /* [0] = total, [1+i] = cpu i; x10 */
static long mem_total_kb, mem_avail_kb, mem_slab_kb;
static long uptime_s;
static char cpu_model[64] = "Processor";
static int cpu_mhz;

typedef struct { char name[16]; uint64_t rx, tx, rxp, txp, rxe, txe; uint64_t rx_rate, tx_rate; int seen; } nic_t;
static nic_t nics[MAXNIC]; static int nnics;
static uint32_t nic_prev_ms;

static int parse_stat(int pid, proc_t* p) {
    char path[48], buf[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if (read_file(path, buf, sizeof(buf)) <= 0) return -1;
    char* lp = strchr(buf, '(');
    char* rp = strrchr(buf, ')');
    if (!lp || !rp || rp < lp) return -1;
    int cl = (int)(rp - lp - 1);
    if (cl > (int)sizeof(p->comm) - 1) cl = (int)sizeof(p->comm) - 1;
    memcpy(p->comm, lp + 1, (size_t)cl); p->comm[cl] = 0;
    char* t[24]; int nt = 0;
    for (char* q = rp + 1; *q && nt < 24;) {
        while (*q == ' ') q++;
        if (!*q) break;
        t[nt++] = q;
        while (*q && *q != ' ' && *q != '\n') q++;
        if (*q) *q++ = 0;
    }
    if (nt < 22) return -1;
    p->pid = pid;
    p->state = t[0][0];
    p->ppid = atoi(t[1]);
    p->ticks = strtoul(t[11], 0, 10) + strtoul(t[12], 0, 10);   /* utime + stime: all threads */
    p->threads = atoi(t[17]);
    p->start = strtoul(t[19], 0, 10);
    p->rss_kb = (long)strtoul(t[21], 0, 10) * 4;
    return 0;
}

static void sample_procs(uint32_t now) {
    uint32_t dt = now - prev_ms;
    int first = prev_ms == 0;
    prev_ms = now;
    nprocs = 0;
    DIR* d = opendir("/proc");
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) && nprocs < MAXP) {
        int pid = atoi(e->d_name);
        if (pid <= 0) continue;
        proc_t* p = &procs[nprocs];
        memset(p, 0, sizeof(*p));
        if (parse_stat(pid, p) != 0) continue;
        char path[48], buf[256];
        struct stat st;
        snprintf(path, sizeof(path), "/proc/%d", pid);
        p->uid = stat(path, &st) == 0 ? (int)st.st_uid : -1;
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        int n = read_file(path, buf, sizeof(buf));
        if (n > 0) {
            for (int i = 0; i < n; i++) if (buf[i] == 0) buf[i] = ' ';
            while (n > 0 && buf[n - 1] == ' ') buf[--n] = 0;
            snprintf(p->cmd, sizeof(p->cmd), "%s", buf);
        }
        if (!p->cmd[0]) snprintf(p->cmd, sizeof(p->cmd), "[%s]", p->comm);
        p->cpu10 = 0;
        for (int i = 0; i < nprevp; i++)
            if (prevp[i].pid == pid && prevp[i].start == p->start) {
                if (!first && dt > 0 && p->ticks >= prevp[i].ticks) {
                    uint64_t v = (p->ticks - prevp[i].ticks) * 10000ull / ((uint64_t)dt * (uint64_t)(ncpu > 0 ? ncpu : 1));
                    p->cpu10 = v > 1000 ? 1000 : (int)v;
                }
                break;
            }
        nprocs++;
    }
    closedir(d);
    nprevp = 0;
    for (int i = 0; i < nprocs; i++) {
        prevp[nprevp].pid = procs[i].pid; prevp[nprevp].start = procs[i].start; prevp[nprevp].ticks = procs[i].ticks;
        nprevp++;
    }
}

static void sample_system(uint32_t now) {
    static char buf[8192];
    if (read_file("/proc/stat", buf, sizeof(buf)) > 0) {
        int n = 0;
        for (char* line = buf; *line;) {
            char* nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (!strncmp(line, "cpu", 3)) {
                int idx = 0;
                char* q = line + 3;
                if (*q >= '0' && *q <= '9') { idx = 1 + atoi(q); while (*q >= '0' && *q <= '9') q++; }
                if (idx <= MAXCPU) {
                    uint64_t v[8] = { 0 };
                    for (int k = 0; k < 8; k++) v[k] = strtoul(q, &q, 10);
                    uint64_t busy = v[0] + v[1] + v[2] + v[5] + v[6] + v[7];
                    uint64_t tot = busy + v[3] + v[4];
                    if (cpu_tot_prev[idx] && tot > cpu_tot_prev[idx]) {
                        uint64_t db = busy - cpu_busy_prev[idx], dtt = tot - cpu_tot_prev[idx];
                        if (busy < cpu_busy_prev[idx]) db = 0;
                        int x = (int)(db * 1000 / dtt);
                        cpu10[idx] = x > 1000 ? 1000 : x;
                    }
                    cpu_busy_prev[idx] = busy; cpu_tot_prev[idx] = tot;
                    if (idx > n) n = idx;
                }
            }
            if (!nl) break;
            line = nl + 1;
        }
        if (n > 0) ncpu = n;
    }
    if (read_file("/proc/meminfo", buf, sizeof(buf)) > 0) {
        mem_total_kb = kv(buf, "MemTotal");
        long av = kv(buf, "MemAvailable"), fr = kv(buf, "MemFree");
        if (av < 0 || av > mem_total_kb) av = fr;
        mem_avail_kb = av < 0 ? 0 : av;
        long sl = kv(buf, "Slab");
        mem_slab_kb = sl < 0 ? 0 : sl;
    }
    if (read_file("/proc/uptime", buf, sizeof(buf)) > 0) uptime_s = strtol(buf, 0, 10);
    if (read_file("/proc/net/dev", buf, sizeof(buf)) > 0) {
        uint32_t dt = now - nic_prev_ms;
        int first = nic_prev_ms == 0;
        nic_prev_ms = now;
        for (int i = 0; i < nnics; i++) nics[i].seen = 0;
        for (char* line = buf; *line;) {
            char* nl = strchr(line, '\n');
            if (nl) *nl = 0;
            char* colon = strchr(line, ':');
            if (colon && !strchr(line, '|')) {
                *colon = 0;
                char* nm = line; while (*nm == ' ') nm++;
                char* q = colon + 1;
                uint64_t v[16] = { 0 };
                for (int k = 0; k < 16; k++) v[k] = strtoul(q, &q, 10);
                nic_t* d = 0;
                for (int i = 0; i < nnics; i++) if (!strcmp(nics[i].name, nm)) d = &nics[i];
                if (!d && nnics < MAXNIC && strcmp(nm, "lo") != 0) {
                    d = &nics[nnics++]; memset(d, 0, sizeof(*d));
                    snprintf(d->name, sizeof(d->name), "%s", nm);
                    d->rx = v[0]; d->tx = v[8];
                }
                if (d) {
                    if (!first && dt > 0) {
                        d->rx_rate = v[0] >= d->rx ? (v[0] - d->rx) * 1000 / dt : 0;
                        d->tx_rate = v[8] >= d->tx ? (v[8] - d->tx) * 1000 / dt : 0;
                    }
                    d->rx = v[0]; d->rxp = v[1]; d->rxe = v[2] + v[3];
                    d->tx = v[8]; d->txp = v[9]; d->txe = v[10] + v[11];
                    d->seen = 1;
                }
            }
            if (!nl) break;
            line = nl + 1;
        }
    }
}

static void read_cpuinfo(void) {
    static char buf[4096];
    if (read_file("/proc/cpuinfo", buf, sizeof(buf)) <= 0) return;
    char* p = strstr(buf, "model name");
    if (p && (p = strchr(p, ':'))) {
        p++; while (*p == ' ' || *p == '\t') p++;
        int k = 0;
        while (p[k] && p[k] != '\n' && k < (int)sizeof(cpu_model) - 1) { cpu_model[k] = p[k]; k++; }
        cpu_model[k] = 0;
    }
    p = strstr(buf, "cpu MHz");
    if (p && (p = strchr(p, ':'))) cpu_mhz = atoi(p + 1);
}

/* ---- graphs ------------------------------------------------------------------ */
typedef struct {
    int32_t v[2][HIST];
    int n, nser;
    uint32_t col[2];
    int32_t fixed_max;          /* > 0: fixed scale; 0: auto (rates) */
} gdata_t;

static void g_push(gdata_t* g, int s, int32_t val) {
    memmove(&g->v[s][0], &g->v[s][1], sizeof(int32_t) * (HIST - 1));
    g->v[s][HIST - 1] = val;
}
static void g_tick(gdata_t* g) { if (g->n < HIST) g->n++; }

static int32_t nice_ceil(int32_t v) {        /* 1-2-5 steps, for an auto scale */
    int32_t m = 1;
    while (m < v / 10) m *= 10;
    if (v <= m) return m;
    if (v <= 2 * m) return 2 * m;
    if (v <= 5 * m) return 5 * m;
    return 10 * m;
}
static int32_t g_max(const gdata_t* g) {
    if (g->fixed_max > 0) return g->fixed_max;
    int32_t m = 0;
    for (int s = 0; s < g->nser; s++) for (int i = HIST - g->n; i < HIST; i++) if (g->v[s][i] > m) m = g->v[s][i];
    return nice_ceil(m < 1250 ? 1250 : m);   /* bytes/s; >= 10 Kbps full scale */
}

static void d_rect(lv_layer_t* l, int x1, int y1, int x2, int y2, uint32_t col, lv_opa_t opa, int radius) {
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(col); d.bg_opa = opa; d.radius = radius;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(l, &d, &a);
}
static void d_frame(lv_layer_t* l, int x1, int y1, int x2, int y2, uint32_t col, int radius) {
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP; d.border_color = lv_color_hex(col); d.border_width = 1; d.border_opa = LV_OPA_COVER;
    d.radius = radius;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(l, &d, &a);
}
static void d_line(lv_layer_t* l, int x1, int y1, int x2, int y2, uint32_t col, int w, lv_opa_t opa) {
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.p1.x = x1; d.p1.y = y1; d.p2.x = x2; d.p2.y = y2;
    d.color = lv_color_hex(col); d.width = w; d.opa = opa; d.round_start = d.round_end = 1;
    lv_draw_line(l, &d);
}
static void d_text(lv_layer_t* l, int x1, int y1, int x2, const char* s, const lv_font_t* f, uint32_t col, lv_text_align_t al) {
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.text = s; d.text_local = 1; d.font = f; d.color = lv_color_hex(col); d.align = al;
    lv_area_t a = { x1, y1, x2, y1 + lv_font_get_line_height(f) };
    lv_draw_label(l, &d, &a);
}
/* Copy s into out so that it fits maxw pixels, with "..." when cut. */
static const char* fit(const char* s, int maxw, const lv_font_t* f, char* out, int n) {
    int len = (int)strlen(s);
    if (lv_text_get_width(s, (uint32_t)len, f, 0) <= maxw) return s;
    int dots = lv_text_get_width("...", 3, f, 0);
    while (len > 0 && lv_text_get_width(s, (uint32_t)len, f, 0) + dots > maxw) len--;
    if (len > n - 4) len = n - 4;
    memcpy(out, s, (size_t)len); memcpy(out + len, "...", 4);
    return out;
}
static uint32_t mix(uint32_t a, uint32_t b, int t) {   /* t = 0 (a) .. 255 (b) */
    uint32_t r = (((a >> 16) & 255) * (uint32_t)(255 - t) + ((b >> 16) & 255) * (uint32_t)t) / 255;
    uint32_t g = (((a >> 8) & 255) * (uint32_t)(255 - t) + ((b >> 8) & 255) * (uint32_t)t) / 255;
    uint32_t bl = ((a & 255) * (uint32_t)(255 - t) + (b & 255) * (uint32_t)t) / 255;
    return (r << 16) | (g << 8) | bl;
}

/* A graph: newest sample on the right, translucent area under the line (as
 * opaque pre-mixed triangles: no seams), grid on top, then the line. */
static void draw_graph(lv_layer_t* l, const lv_area_t* a, const gdata_t* g, int grid, uint32_t bg) {
    int w = lv_area_get_width(a), h = lv_area_get_height(a);
    d_rect(l, a->x1, a->y1, a->x2, a->y2, bg, LV_OPA_COVER, 0);
    int32_t mx = g_max(g);
    if (mx <= 0) mx = 1;
    int x0 = a->x1 + 1, x1 = a->x2 - 1, y0 = a->y1 + 1, y1 = a->y2 - 1;
    int iw = x1 - x0, ih = y1 - y0;
    for (int s = g->nser - 1; s >= 0; s--) {
        uint32_t fillc = mix(bg, g->col[s], 55);
        int px = -1, py = 0;
        for (int i = HIST - g->n; i < HIST; i++) {
            int32_t v = g->v[s][i]; if (v < 0) v = 0; if (v > mx) v = mx;
            int x = x0 + (int)((int64_t)iw * i / (HIST - 1));
            int y = y1 - (int)((int64_t)ih * v / mx);
            if (px >= 0 && s == 0) {
                int low = py > y ? py : y;              /* the lower end of the segment */
                if (low < y1) d_rect(l, px, low, x, y1, fillc, LV_OPA_COVER, 0);
                if (py != y) {
                    lv_draw_triangle_dsc_t t; lv_draw_triangle_dsc_init(&t);
                    t.bg_color = lv_color_hex(fillc); t.bg_opa = LV_OPA_COVER;
                    t.p[0].x = px; t.p[0].y = py; t.p[1].x = x; t.p[1].y = y;
                    t.p[2].x = py > y ? x : px; t.p[2].y = low + 1;
                    lv_draw_triangle(l, &t);
                }
            }
            px = x; py = y;
        }
    }
    if (grid) {
        uint32_t gc = mix(bg, g->col[0], 70);
        for (int k = 1; k < 10; k++) { int x = a->x1 + w * k / 10; d_rect(l, x, y0, x, y1, gc, 90, 0); }
        for (int k = 1; k < 5; k++) { int y = a->y1 + h * k / 5; d_rect(l, x0, y, x1, y, gc, 90, 0); }
    }
    for (int s = g->nser - 1; s >= 0; s--) {
        int px = -1, py = 0;
        for (int i = HIST - g->n; i < HIST; i++) {
            int32_t v = g->v[s][i]; if (v < 0) v = 0; if (v > mx) v = mx;
            int x = x0 + (int)((int64_t)iw * i / (HIST - 1));
            int y = y1 - (int)((int64_t)ih * v / mx);
            if (px >= 0) d_line(l, px, py, x, y, g->col[s], s == 0 ? 2 : 1, LV_OPA_COVER);
            px = x; py = y;
        }
    }
    d_frame(l, a->x1, a->y1, a->x2, a->y2, mix(bg, g->col[0], 150), 0);
}

static lv_obj_t* plain_obj(lv_obj_t* parent) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}
static lv_obj_t* label(lv_obj_t* parent, const lv_font_t* f, uint32_t col, const char* txt) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_label_set_text(l, txt);
    return l;
}
static void trace_obj(const char* name, lv_obj_t* o) {
    if (!trace || !o) return;
    lv_obj_update_layout(o);
    lv_area_t a; lv_obj_get_coords(o, &a);
    printf("TASKMGR-OBJ %s %d,%d,%d,%d\n", name, (int)a.x1, (int)a.y1, (int)lv_area_get_width(&a), (int)lv_area_get_height(&a));
}

/* ---- list view (Processes, Users) ---------------------------------------------- */
typedef struct { const char* name; int w; int right; int numeric; int desc_first; } lcol_t;
typedef struct {
    char txt[NCOL][96];
    int64_t num[NCOL];
    int heat[NCOL];             /* 0..1000: cell shading (CPU, memory) */
    int key;                    /* pid / uid */
} lrow_t;
typedef struct lview {
    lv_obj_t* obj;
    const lcol_t* cols; int ncol;
    int x[NCOL + 1];
    lrow_t rows[MAXP]; int n;
    char tot[NCOL][16];
    int sort, desc;
    int sel, has_sel;           /* selected key */
    int scroll, hover;
    int press_mode;             /* 0 none, 1 body, 2 header, 3 scrollbar */
    int press_y, press_scroll, press_col, dragging;
    uint32_t sig[64];           /* per visible slot: what was drawn */
    int last_scroll, last_n;
    void (*on_select)(struct lview*);
    const char* tname;
} lview_t;

static lview_t lv_procs, lv_users;
static const lview_t* g_sort_view;

static int body_h(const lview_t* v) { return lv_obj_get_height(v->obj) - HDR_H; }
static int max_scroll(const lview_t* v) { int m = v->n * ROW_H - body_h(v); return m > 0 ? m : 0; }
static int sel_index(const lview_t* v) {
    if (!v->has_sel) return -1;
    for (int i = 0; i < v->n; i++) if (v->rows[i].key == v->sel) return i;
    return -1;
}
static uint32_t row_sig(const lview_t* v, int i) {
    uint32_t h = 2166136261u;
    if (i >= v->n) return 1;
    const lrow_t* r = &v->rows[i];
    for (int c2 = 0; c2 < v->ncol; c2++) {
        for (const char* s = r->txt[c2]; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
        h = (h ^ (uint32_t)(r->heat[c2] / 25)) * 16777619u;
    }
    h = (h ^ (uint32_t)(v->has_sel && r->key == v->sel ? 7 : 0)) * 16777619u;
    h = (h ^ (uint32_t)(v->hover == i ? 3 : 0)) * 16777619u;
    return h | 2;
}
static void row_area(const lview_t* v, int i, lv_area_t* out) {
    lv_area_t a; lv_obj_get_coords(v->obj, &a);
    int y = a.y1 + HDR_H + i * ROW_H - v->scroll;
    out->x1 = a.x1; out->x2 = a.x2; out->y1 = y; out->y2 = y + ROW_H - 1;
    if (out->y1 < a.y1 + HDR_H) out->y1 = a.y1 + HDR_H;
    if (out->y2 > a.y2) out->y2 = a.y2;
}
/* Invalidate only the rows whose content changed (whole body on scroll). */
static void lview_damage(lview_t* v, int all) {
    if (lv_obj_has_flag(lv_obj_get_parent(v->obj), LV_OBJ_FLAG_HIDDEN)) { v->last_scroll = -1; return; }
    if (all || v->scroll != v->last_scroll || v->n != v->last_n) {
        lv_obj_invalidate(v->obj);
        v->last_scroll = v->scroll; v->last_n = v->n;
        int first = v->scroll / ROW_H;
        for (int k = 0; k < 64; k++) v->sig[k] = row_sig(v, first + k);
        return;
    }
    int first = v->scroll / ROW_H, vis = body_h(v) / ROW_H + 2;
    if (vis > 64) vis = 64;
    for (int k = 0; k < vis; k++) {
        uint32_t s = row_sig(v, first + k);
        if (s != v->sig[k]) {
            v->sig[k] = s;
            lv_area_t ra; row_area(v, first + k, &ra);
            if (ra.y2 >= ra.y1) lv_obj_invalidate_area(v->obj, &ra);
        }
    }
}
static void lview_damage_header(lview_t* v) {
    lv_area_t a; lv_obj_get_coords(v->obj, &a);
    a.y2 = a.y1 + HDR_H - 1;
    lv_obj_invalidate_area(v->obj, &a);
}

static int row_cmp(const void* pa, const void* pb) {
    const lview_t* v = g_sort_view;
    const lrow_t* a = (const lrow_t*)pa; const lrow_t* b = (const lrow_t*)pb;
    int c2 = v->sort, r;
    if (v->cols[c2].numeric) r = a->num[c2] < b->num[c2] ? -1 : a->num[c2] > b->num[c2] ? 1 : 0;
    else r = cmp_ci(a->txt[c2], b->txt[c2]);
    if (v->desc) r = -r;
    if (!r) r = a->key - b->key;
    return r;
}
static void lview_sort(lview_t* v) { g_sort_view = v; qsort(v->rows, (size_t)v->n, sizeof(lrow_t), row_cmp); }

static void lview_clamp(lview_t* v) {
    int m = max_scroll(v);
    if (v->scroll > m) v->scroll = m;
    if (v->scroll < 0) v->scroll = 0;
}
static void lview_show_index(lview_t* v, int i) {
    if (i < 0) return;
    int top = i * ROW_H, bh = body_h(v);
    if (top < v->scroll) v->scroll = top;
    else if (top + ROW_H > v->scroll + bh) v->scroll = top + ROW_H - bh;
    lview_clamp(v);
}
static void trace_rows(lview_t* v) {
    if (!trace || v != &lv_procs) return;
    static char line[1024], last[1024];
    lv_area_t a; lv_obj_get_coords(v->obj, &a);
    int o = snprintf(line, sizeof(line), "TASKMGR-ROWS y0=%d rh=%d pids=", (int)a.y1 + HDR_H - v->scroll, ROW_H);
    for (int i = 0; i < v->n && o < (int)sizeof(line) - 12; i++) o += snprintf(line + o, sizeof(line) - (size_t)o, "%s%d", i ? "," : "", v->rows[i].key);
    if (strcmp(line, last)) { strcpy(last, line); printf("%s\n", line); }
}
static void lview_select(lview_t* v, int i) {
    if (i < 0 || i >= v->n) return;
    if (v->has_sel && v->sel == v->rows[i].key) return;
    v->has_sel = 1; v->sel = v->rows[i].key;
    lview_show_index(v, i);
    lview_damage(v, 0);
    trace_rows(v);
    if (v->on_select) v->on_select(v);
}

static void lview_draw(lv_event_t* e) {
    lview_t* v = (lview_t*)lv_event_get_user_data(e);
    lv_layer_t* l = lv_event_get_layer(e);
    lv_area_t a; lv_obj_get_coords(v->obj, &a);
    const lv_font_t* f = &lv_font_montserrat_14;
    const lv_font_t* fs = &lv_font_montserrat_12;
    int lh = lv_font_get_line_height(f);
    char tmp[100];
    d_rect(l, a.x1, a.y1 + HDR_H, a.x2, a.y2, C_BG, LV_OPA_COVER, 0);
    int first = v->scroll / ROW_H;
    for (int i = first; i < v->n; i++) {
        int y = a.y1 + HDR_H + i * ROW_H - v->scroll;
        if (y > a.y2) break;
        const lrow_t* r = &v->rows[i];
        int sel = v->has_sel && r->key == v->sel;
        uint32_t bg = sel ? C_SEL : (v->hover == i ? C_HOVER : (i & 1) ? C_ROW2 : C_BG);
        d_rect(l, a.x1, y, a.x2, y + ROW_H - 1, bg, LV_OPA_COVER, 0);
        for (int k = 0; k < v->ncol; k++) {
            int cx1 = a.x1 + v->x[k], cx2 = a.x1 + v->x[k + 1] - 1;
            if (r->heat[k] > 0 && !sel) {
                int t = 28 + r->heat[k] * 170 / 1000;
                if (t > 200) t = 200;
                d_rect(l, cx1, y, cx2, y + ROW_H - 1, mix(bg, C_HEAT, t), LV_OPA_COVER, 0);
            }
            const char* s = fit(r->txt[k], cx2 - cx1 - 14, f, tmp, sizeof(tmp));
            uint32_t col = sel ? 0xFFFFFF : (k == 0 ? C_TEXT : (r->heat[k] > 300 ? 0xFFFFFF : 0xC9D3DD));
            d_text(l, cx1 + 7, y + (ROW_H - lh) / 2, cx2 - 7, s, f, col, v->cols[k].right ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT);
        }
        if (sel) d_rect(l, a.x1, y, a.x1 + 2, y + ROW_H - 1, C_ACCENT, LV_OPA_COVER, 0);
    }
    if (v->n == 0) d_text(l, a.x1, a.y1 + HDR_H + 20, a.x2, "Nothing to show", f, C_DIM, LV_TEXT_ALIGN_CENTER);
    /* scroll bar */
    int bh = body_h(v), tot = v->n * ROW_H;
    if (tot > bh && bh > 20) {
        int th = bh * bh / tot; if (th < 24) th = 24;
        int ty = a.y1 + HDR_H + (int)((int64_t)(bh - th) * v->scroll / max_scroll(v));
        d_rect(l, a.x2 - 7, ty + 2, a.x2 - 3, ty + th - 3, 0x55657A, LV_OPA_COVER, 2);
    }
    /* header, drawn last so a row scrolled under it is covered */
    d_rect(l, a.x1, a.y1, a.x2, a.y1 + HDR_H - 1, C_HDR, LV_OPA_COVER, 0);
    d_rect(l, a.x1, a.y1 + HDR_H - 1, a.x2, a.y1 + HDR_H - 1, C_LINE, LV_OPA_COVER, 0);
    for (int k = 0; k < v->ncol; k++) {
        int cx1 = a.x1 + v->x[k], cx2 = a.x1 + v->x[k + 1] - 1;
        if (k < v->ncol - 1) d_rect(l, cx2, a.y1 + 6, cx2, a.y1 + HDR_H - 7, C_LINE, LV_OPA_COVER, 0);
        int sorted = v->sort == k;
        char nm[48];
        snprintf(nm, sizeof(nm), "%s%s", v->cols[k].name, sorted ? (v->desc ? " " LV_SYMBOL_DOWN : " " LV_SYMBOL_UP) : "");
        lv_text_align_t al = v->cols[k].right ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT;
        d_text(l, cx1 + 7, a.y1 + HDR_H - 18, cx2 - 7, nm, fs, sorted ? C_TEXT : C_DIM, al);
        if (v->tot[k][0]) d_text(l, cx1 + 7, a.y1 + 3, cx2 - 7, v->tot[k], f, C_TEXT, al);
    }
}

static int col_at(const lview_t* v, int rx) {
    for (int k = 0; k < v->ncol; k++) if (rx >= v->x[k] && rx < v->x[k + 1]) return k;
    return v->ncol - 1;
}
static void lview_event(lv_event_t* e) {
    lview_t* v = (lview_t*)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DRAW_MAIN) { lview_draw(e); return; }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    lv_point_t p; lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a; lv_obj_get_coords(v->obj, &a);
    int rx = p.x - a.x1, ry = p.y - a.y1;
    if (code == LV_EVENT_PRESSED) {
        v->dragging = 0; v->press_y = ry; v->press_scroll = v->scroll;
        if (ry < HDR_H) { v->press_mode = 2; v->press_col = col_at(v, rx); return; }
        if (rx >= lv_area_get_width(&a) - 12 && max_scroll(v) > 0) { v->press_mode = 3; }
        else {
            v->press_mode = 1;
            int i = (ry - HDR_H + v->scroll) / ROW_H;
            if (i >= 0 && i < v->n) lview_select(v, i);
            return;
        }
    }
    if (code == LV_EVENT_PRESSING || (code == LV_EVENT_PRESSED && v->press_mode == 3)) {
        if (v->press_mode == 1) {
            int dy = ry - v->press_y;
            if (!v->dragging && (dy > 8 || dy < -8)) v->dragging = 1;
            if (v->dragging) { v->scroll = v->press_scroll - dy; lview_clamp(v); lview_damage(v, 0); trace_rows(v); }
        } else if (v->press_mode == 3) {
            int bh = body_h(v), tot = v->n * ROW_H;
            int th = bh * bh / tot; if (th < 24) th = 24;
            int pos = ry - HDR_H - th / 2;
            v->scroll = (int)((int64_t)pos * max_scroll(v) / (bh - th > 0 ? bh - th : 1));
            lview_clamp(v); lview_damage(v, 0); trace_rows(v);
        }
        return;
    }
    if (code == LV_EVENT_RELEASED && v->press_mode == 2 && ry < HDR_H && col_at(v, rx) == v->press_col) {
        int k = v->press_col;
        if (v->sort == k) v->desc = !v->desc;
        else { v->sort = k; v->desc = v->cols[k].desc_first; }
        T("TASKMGR-SORT %s col=%s desc=%d", v->tname, v->cols[k].name, v->desc);
        lview_sort(v);
        lview_show_index(v, sel_index(v));
        lview_damage(v, 1);
        trace_rows(v);
    }
    v->press_mode = 0;
}

static void lview_init(lview_t* v, lv_obj_t* parent, const lcol_t* cols, int ncol, int w, int h, const char* tname) {
    memset(v, 0, sizeof(*v));
    v->cols = cols; v->ncol = ncol; v->hover = -1; v->last_scroll = -1; v->tname = tname;
    v->obj = plain_obj(parent);
    lv_obj_add_flag(v->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(v->obj, w, h);
    int x = 0, fixed = 0;
    for (int k = 0; k < ncol - 1; k++) fixed += cols[k].w;
    for (int k = 0; k < ncol; k++) { v->x[k] = x; x += k < ncol - 1 ? cols[k].w : (w - fixed); }
    v->x[ncol] = w;
    lv_obj_add_event_cb(v->obj, lview_event, LV_EVENT_ALL, v);
}

/* ---- UI state -------------------------------------------------------------- */
enum { TAB_PROC, TAB_PERF, TAB_USERS, NTAB };
static lv_obj_t *tabbtn[NTAB], *page[NTAB];
static int cur_tab;
static lv_obj_t *btn_end, *l_foot, *l_status, *l_status_r;
static uint32_t foot_until;

static const lcol_t proc_cols[] = {
    { "Name", 168, 0, 0, 0 }, { "PID", 64, 1, 1, 0 }, { "User", 96, 0, 0, 0 }, { "Status", 92, 0, 0, 0 },
    { "Threads", 72, 1, 1, 1 }, { "CPU", 72, 1, 1, 1 }, { "Memory", 96, 1, 1, 1 }, { "Command line", 0, 0, 0, 0 },
};
static const lcol_t user_cols[] = {
    { "User", 220, 0, 0, 0 }, { "UID", 80, 1, 1, 0 }, { "Processes", 110, 1, 1, 1 },
    { "CPU", 90, 1, 1, 1 }, { "Memory", 120, 1, 1, 1 }, { "", 0, 0, 0, 0 },
};
enum { PC_NAME, PC_PID, PC_USER, PC_STATUS, PC_THREADS, PC_CPU, PC_MEM, PC_CMD };

/* Performance tab */
enum { PK_CPU, PK_MEM, PK_NIC };
typedef struct { lv_obj_t* obj; int kind, idx; gdata_t* g; char title[32], sub[48]; } side_t;
static side_t side[2 + MAXNIC]; static int nside, cur_side;
static gdata_t g_cpu_tot, g_cpu[MAXCPU], g_mem, g_nic[MAXNIC];
static lv_obj_t *perf_right, *l_ptitle, *l_psub, *l_cap_l, *l_cap_r, *l_cap_b, *l_leg1, *l_leg2;
static lv_obj_t *pane_cpu, *pane_mem, *pane_net;
static lv_obj_t* cpu_graph[MAXCPU]; static int ncpu_graphs;
static lv_obj_t *big_graph_mem, *big_graph_net;
typedef struct { gdata_t* g; int idx; } gview_t;
static gview_t gv_cpu[MAXCPU], gv_mem, gv_net;
#define NSTAT 6
static lv_obj_t *st_val[NSTAT], *st_cap[NSTAT]; static lv_obj_t* stats_box;
static int perf_built_cpus;

/* dialogs */
enum { DLG_NONE, DLG_CONFIRM, DLG_KILL, DLG_INFO };
static lv_obj_t* dlg; static int dlg_kind;
static struct { int pid; uint64_t start; char name[32]; } dlg_t;
static struct { int active, pid; uint64_t start; uint32_t deadline; char name[32]; } pending;

static char typeahead[32]; static int nta; static uint32_t ta_ms;

static void set_foot(const char* msg, uint32_t col) {
    lv_label_set_text(l_foot, msg);
    lv_obj_set_style_text_color(l_foot, lv_color_hex(col), 0);
    foot_until = tick_ms() + 6000;
}

static const proc_t* proc_by_pid(int pid) {
    for (int i = 0; i < nprocs; i++) if (procs[i].pid == pid) return &procs[i];
    return 0;
}
static const char* status_name(char s) {
    switch (s) {
    case 'R': return "Running";
    case 'S': return "Sleeping";
    case 'T': return "Stopped";
    case 'Z': return "Zombie";
    case 'D': return "Starting";
    default: return "?";
    }
}

static void update_end_button(void) {
    int ok = lv_procs.has_sel && proc_by_pid(lv_procs.sel) != 0 && !dlg;
    if (ok) lv_obj_remove_state(btn_end, LV_STATE_DISABLED);
    else lv_obj_add_state(btn_end, LV_STATE_DISABLED);
}
static void on_proc_select(lview_t* v) {
    const proc_t* p = proc_by_pid(v->sel);
    if (p) T("TASKMGR-SELECT pid=%d name=%s", p->pid, p->comm);
    update_end_button();
}

/* ---- refresh ---------------------------------------------------------------- */
static void fill_proc_rows(void) {
    lview_t* v = &lv_procs;
    v->n = 0;
    long tot_rss = 0; int tot_cpu = 0;
    char tmp[16];
    for (int i = 0; i < nprocs && v->n < MAXP; i++) {
        const proc_t* p = &procs[i];
        lrow_t* r = &v->rows[v->n++];
        memset(r, 0, sizeof(*r));
        r->key = p->pid;
        snprintf(r->txt[PC_NAME], sizeof(r->txt[0]), "%s", p->comm);
        snprintf(r->txt[PC_PID], sizeof(r->txt[0]), "%d", p->pid); r->num[PC_PID] = p->pid;
        snprintf(r->txt[PC_USER], sizeof(r->txt[0]), "%s", user_name(p->uid, tmp, sizeof(tmp)));
        snprintf(r->txt[PC_STATUS], sizeof(r->txt[0]), "%s", status_name(p->state));
        snprintf(r->txt[PC_THREADS], sizeof(r->txt[0]), "%d", p->threads); r->num[PC_THREADS] = p->threads;
        fmt_pct10(r->txt[PC_CPU], sizeof(r->txt[0]), p->cpu10); r->num[PC_CPU] = p->cpu10; r->heat[PC_CPU] = p->cpu10;
        fmt_kb(r->txt[PC_MEM], sizeof(r->txt[0]), p->rss_kb); r->num[PC_MEM] = p->rss_kb;
        r->heat[PC_MEM] = mem_total_kb > 0 ? (int)(p->rss_kb * 4000 / mem_total_kb) : 0;   /* 25% of RAM = full */
        if (r->heat[PC_MEM] > 1000) r->heat[PC_MEM] = 1000;
        snprintf(r->txt[PC_CMD], sizeof(r->txt[0]), "%s", p->cmd);
        tot_rss += p->rss_kb; tot_cpu += p->cpu10;
    }
    (void)tot_rss; (void)tot_cpu;
    lview_sort(v);
    if (v->has_sel && sel_index(v) < 0) v->has_sel = 0;   /* the process is gone */
    lview_clamp(v);
    snprintf(v->tot[PC_CPU], sizeof(v->tot[0]), "%d%%", (cpu10[0] + 5) / 10);
    long used = mem_total_kb - mem_avail_kb;
    snprintf(v->tot[PC_MEM], sizeof(v->tot[0]), "%d%%", mem_total_kb > 0 ? (int)(used * 100 / mem_total_kb) : 0);
}

static void fill_user_rows(void) {
    lview_t* v = &lv_users;
    v->n = 0;
    char tmp[16];
    for (int i = 0; i < nprocs; i++) {
        const proc_t* p = &procs[i];
        lrow_t* r = 0;
        for (int k = 0; k < v->n; k++) if (v->rows[k].key == p->uid) r = &v->rows[k];
        if (!r) {
            if (v->n >= MAXP) continue;
            r = &v->rows[v->n++];
            memset(r, 0, sizeof(*r));
            r->key = p->uid;
            snprintf(r->txt[0], sizeof(r->txt[0]), "%s%s", user_name(p->uid, tmp, sizeof(tmp)), p->uid == my_uid ? "  (you)" : "");
            r->num[1] = p->uid; snprintf(r->txt[1], sizeof(r->txt[0]), "%d", p->uid);
        }
        r->num[2]++; r->num[3] += p->cpu10; r->num[4] += p->rss_kb;
    }
    for (int k = 0; k < v->n; k++) {
        lrow_t* r = &v->rows[k];
        snprintf(r->txt[2], sizeof(r->txt[0]), "%ld", (long)r->num[2]);
        if (r->num[3] > 1000) r->num[3] = 1000;
        fmt_pct10(r->txt[3], sizeof(r->txt[0]), (int)r->num[3]); r->heat[3] = (int)r->num[3];
        fmt_kb(r->txt[4], sizeof(r->txt[0]), (long)r->num[4]);
        r->heat[4] = mem_total_kb > 0 ? (int)(r->num[4] * 2000 / mem_total_kb) : 0;
        if (r->heat[4] > 1000) r->heat[4] = 1000;
    }
    lview_sort(v);
    if (v->has_sel && sel_index(v) < 0) v->has_sel = 0;
    lview_clamp(v);
}

static void trace_list(void) {
    if (!trace) return;
    static int last[MAXP], nlast = -1;
    int same = nlast == nprocs;
    for (int i = 0; same && i < nprocs; i++) if (last[i] != procs[i].pid) same = 0;
    if (same) return;
    nlast = nprocs;
    for (int i = 0; i < nprocs; i++) last[i] = procs[i].pid;
    char tmp[16];
    printf("TASKMGR-LIST n=%d\n", nprocs);
    for (int i = 0; i < nprocs; i++)
        printf("TASKMGR-P %d %s %s %c %d\n", procs[i].pid, procs[i].comm, user_name(procs[i].uid, tmp, sizeof(tmp)),
               procs[i].state, procs[i].threads);
    printf("TASKMGR-LIST-END\n");
}

static lv_obj_t* stat_cell(lv_obj_t* parent, int i, const char* cap) {
    lv_obj_t* box = plain_obj(parent);
    lv_obj_set_size(box, LV_SIZE_CONTENT, 46);
    lv_obj_set_style_min_width(box, 84, 0);
    st_cap[i] = label(box, &lv_font_montserrat_12, C_DIM, cap);
    st_val[i] = label(box, &lv_font_montserrat_20, C_TEXT, "-");
    lv_obj_set_pos(st_val[i], 0, 16);
    return box;
}

static void side_set(int i) {
    if (i < 0 || i >= nside) return;
    cur_side = i;
    for (int k = 0; k < nside; k++) lv_obj_invalidate(side[k].obj);
    side_t* s = &side[i];
    lv_obj_add_flag(pane_cpu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(pane_mem, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(pane_net, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(l_leg1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(l_leg2, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(l_ptitle, s->title);
    if (s->kind == PK_CPU) {
        lv_obj_remove_flag(pane_cpu, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(l_psub, cpu_model);
        lv_label_set_text(l_cap_l, "% Utilization over 60 seconds, per logical processor");
        lv_label_set_text(l_cap_r, "100%");
    } else if (s->kind == PK_MEM) {
        lv_obj_remove_flag(pane_mem, LV_OBJ_FLAG_HIDDEN);
        char b[48]; fmt_kb(b, sizeof(b), mem_total_kb);
        lv_label_set_text_fmt(l_psub, "%s RAM", b);
        lv_label_set_text(l_cap_l, "Memory usage");
        lv_label_set_text(l_cap_r, b);
    } else {
        lv_obj_remove_flag(pane_net, LV_OBJ_FLAG_HIDDEN);
        gv_net.g = &g_nic[s->idx];
        lv_obj_invalidate(big_graph_net);
        lv_label_set_text_fmt(l_psub, "%s", nics[s->idx].name);
        lv_label_set_text(l_cap_l, "Throughput");
        lv_obj_remove_flag(l_leg1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(l_leg2, LV_OBJ_FLAG_HIDDEN);
    }
    T("TASKMGR-PERF %s", s->title);
}

static void update_perf_texts(void) {
    char b[64], b2[48];
    side_t* s = &side[cur_side];
    int k = 0;
#define STAT(capt, ...) do { if (k < NSTAT) { snprintf(b, sizeof(b), __VA_ARGS__); \
        if (strcmp(lv_label_get_text(st_cap[k]), capt)) lv_label_set_text(st_cap[k], capt); \
        if (strcmp(lv_label_get_text(st_val[k]), b)) lv_label_set_text(st_val[k], b); k++; } } while (0)
    long used = mem_total_kb - mem_avail_kb;
    if (s->kind == PK_CPU) {
        STAT("Utilization", "%d%%", (cpu10[0] + 5) / 10);
        if (cpu_mhz > 0) STAT("Speed", "%d.%02d GHz", cpu_mhz / 1000, (cpu_mhz % 1000) / 10);
        STAT("Processes", "%d", nprocs);
        fmt_uptime(b2, sizeof(b2), uptime_s); STAT("Up time", "%s", b2);
        STAT("Logical processors", "%d", ncpu);
    } else if (s->kind == PK_MEM) {
        fmt_kb(b2, sizeof(b2), used); STAT("In use", "%s", b2);
        fmt_kb(b2, sizeof(b2), mem_avail_kb); STAT("Available", "%s", b2);
        fmt_kb(b2, sizeof(b2), mem_total_kb); STAT("Total", "%s", b2);
        fmt_kb(b2, sizeof(b2), mem_slab_kb); STAT("Kernel heap", "%s", b2);
        STAT("Used", "%d%%", mem_total_kb > 0 ? (int)(used * 100 / mem_total_kb) : 0);
    } else {
        nic_t* d = &nics[s->idx];
        fmt_bits(b2, sizeof(b2), d->rx_rate); STAT("Receive", "%s", b2);
        fmt_bits(b2, sizeof(b2), d->tx_rate); STAT("Send", "%s", b2);
        fmt_bytes(b2, sizeof(b2), d->rx); STAT("Received", "%s", b2);
        fmt_bytes(b2, sizeof(b2), d->tx); STAT("Sent", "%s", b2);
        STAT("Packets in / out", "%lu / %lu", (unsigned long)d->rxp, (unsigned long)d->txp);
        STAT("Errors", "%lu", (unsigned long)(d->rxe + d->txe));
        fmt_bits(b2, sizeof(b2), (uint64_t)g_max(&g_nic[s->idx])); lv_label_set_text(l_cap_r, b2);
    }
#undef STAT
    for (int i = 0; i < NSTAT; i++) {
        lv_obj_t* box = lv_obj_get_parent(st_val[i]);
        int hid = lv_obj_has_flag(box, LV_OBJ_FLAG_HIDDEN);
        if (i < k && hid) lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
        if (i >= k && !hid) lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    }
    /* sidebar subtitles */
    for (int i = 0; i < nside; i++) {
        side_t* sd = &side[i];
        char old[48]; snprintf(old, sizeof(old), "%s", sd->sub);
        if (sd->kind == PK_CPU) snprintf(sd->sub, sizeof(sd->sub), "%d%%", (cpu10[0] + 5) / 10);
        else if (sd->kind == PK_MEM) {
            snprintf(sd->sub, sizeof(sd->sub), "%ld / %ld MB (%d%%)", used / 1024, mem_total_kb / 1024,
                     mem_total_kb > 0 ? (int)(used * 100 / mem_total_kb) : 0);
        } else {
            char r1[16], t1[16]; fmt_bits(r1, sizeof(r1), nics[sd->idx].rx_rate); fmt_bits(t1, sizeof(t1), nics[sd->idx].tx_rate);
            snprintf(sd->sub, sizeof(sd->sub), "%s   R: %s", nics[sd->idx].name, r1);
            (void)t1;
        }
        lv_obj_invalidate(sd->obj);          /* the mini graph moves every second anyway */
    }
}

static void refresh(void) {
    uint32_t now = tick_ms();
    sample_system(now);
    sample_procs(now);
    /* graphs */
    g_push(&g_cpu_tot, 0, cpu10[0]); g_tick(&g_cpu_tot);
    for (int i = 0; i < ncpu && i < MAXCPU; i++) { g_push(&g_cpu[i], 0, cpu10[1 + i]); g_tick(&g_cpu[i]); }
    long used = mem_total_kb - mem_avail_kb;
    g_push(&g_mem, 0, mem_total_kb > 0 ? (int32_t)(used * 1000 / mem_total_kb) : 0); g_tick(&g_mem);
    for (int i = 0; i < nnics; i++) {
        g_push(&g_nic[i], 0, (int32_t)(nics[i].rx_rate > 0x7fffffff ? 0x7fffffff : nics[i].rx_rate));
        g_push(&g_nic[i], 1, (int32_t)(nics[i].tx_rate > 0x7fffffff ? 0x7fffffff : nics[i].tx_rate));
        g_tick(&g_nic[i]);
    }
    fill_proc_rows();
    fill_user_rows();
    trace_list();
    if (cur_tab == TAB_PROC) { lview_damage(&lv_procs, 0); lview_damage_header(&lv_procs); trace_rows(&lv_procs); }
    if (cur_tab == TAB_USERS) { lview_damage(&lv_users, 0); lview_damage_header(&lv_users); }
    if (cur_tab == TAB_PERF) {
        update_perf_texts();
        if (!lv_obj_has_flag(pane_cpu, LV_OBJ_FLAG_HIDDEN)) for (int i = 0; i < ncpu_graphs; i++) lv_obj_invalidate(cpu_graph[i]);
        if (!lv_obj_has_flag(pane_mem, LV_OBJ_FLAG_HIDDEN)) lv_obj_invalidate(big_graph_mem);
        if (!lv_obj_has_flag(pane_net, LV_OBJ_FLAG_HIDDEN)) lv_obj_invalidate(big_graph_net);
    }
    update_end_button();
    char b[160], up[32];
    fmt_uptime(up, sizeof(up), uptime_s);
    snprintf(b, sizeof(b), "Processes: %d     CPU: %d%%     Memory: %d%%     Up time: %s", nprocs, (cpu10[0] + 5) / 10,
             mem_total_kb > 0 ? (int)(used * 100 / mem_total_kb) : 0, up);
    lv_label_set_text(l_status, b);
    if (foot_until && (int32_t)(now - foot_until) > 0) { lv_label_set_text(l_foot, ""); foot_until = 0; }
}

/* ---- End task ------------------------------------------------------------- */
static void dlg_close(void) {
    if (dlg) lv_msgbox_close_async(dlg);     /* may run inside one of its buttons' events */
    dlg = 0; dlg_kind = DLG_NONE;
    update_end_button();
}
/* Still the same process? (pid reuse: compare the start time) */
static int same_proc(int pid, uint64_t start, char* state_out) {
    proc_t p;
    if (parse_stat(pid, &p) != 0) return 0;
    if (state_out) *state_out = p.state;
    return p.start == start;
}
static void do_term(void);
static void do_kill(void);
static void dlg_btn_cb(lv_event_t* e) {
    int action = (int)(intptr_t)lv_event_get_user_data(e);
    int kind = dlg_kind;
    dlg_close();
    if (action == 1 && kind == DLG_CONFIRM) do_term();
    else if (action == 1 && kind == DLG_KILL) do_kill();
    else if (action == 0 && kind == DLG_KILL && pending.active) {
        pending.deadline = tick_ms() + 3000;            /* "Wait": ask again later */
        T("TASKMGR-WAIT pid=%d", pending.pid);
    } else if (action == 0 && kind == DLG_CONFIRM) T("TASKMGR-CANCEL pid=%d", dlg_t.pid);
}
static void dlg_open(int kind, const char* title, const char* text, const char* ok, const char* cancel, int danger) {
    if (dlg) dlg_close();
    dlg_kind = kind;
    dlg = lv_msgbox_create(NULL);
    lv_obj_set_width(dlg, 440);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x1C2530), 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(danger ? C_DANGER : C_ACCENT), 0);
    lv_obj_set_style_border_width(dlg, 1, 0);
    lv_obj_set_style_radius(dlg, 10, 0);
    lv_obj_set_style_shadow_width(dlg, 30, 0);
    lv_obj_set_style_shadow_opa(dlg, LV_OPA_50, 0);
    lv_obj_t* t = lv_msgbox_add_title(dlg, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_t* tx = lv_msgbox_add_text(dlg, text);
    lv_obj_set_style_text_color(tx, lv_color_hex(0xC9D3DD), 0);
    lv_obj_t* bok = lv_msgbox_add_footer_button(dlg, ok);
    lv_obj_add_event_cb(bok, dlg_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)1);
    lv_obj_set_style_bg_color(bok, lv_color_hex(danger ? C_DANGER : 0x2A7FBF), 0);
    lv_obj_t* bc = 0;
    if (cancel) {
        bc = lv_msgbox_add_footer_button(dlg, cancel);
        lv_obj_add_event_cb(bc, dlg_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)0);
        lv_obj_set_style_bg_color(bc, lv_color_hex(0x33404E), 0);
    }
    update_end_button();
    trace_obj("dlg-ok", bok);
    if (bc) trace_obj("dlg-cancel", bc);
}

static void end_task(void) {
    if (dlg || !lv_procs.has_sel) return;
    const proc_t* p = proc_by_pid(lv_procs.sel);
    if (!p) return;
    char tmp[16], msg[320];
    dlg_t.pid = p->pid; dlg_t.start = p->start;
    snprintf(dlg_t.name, sizeof(dlg_t.name), "%s", p->comm);
    /* Ask the kernel first: signal 0 checks the permission (cred_may_signal). */
    if (kill(p->pid, 0) != 0) {
        int er = errno;
        if (er == EPERM) {
            const char* owner = user_name(p->uid, tmp, sizeof(tmp));
            T("TASKMGR-REFUSED pid=%d name=%s owner=%s errno=%d", p->pid, p->comm, owner, er);
            snprintf(msg, sizeof(msg), "\"%s\" (PID %d) belongs to %s.\n\n"
                     "You are signed in as %s and can end only your own processes.",
                     p->comm, p->pid, owner, my_name);
            dlg_open(DLG_INFO, "Access denied", msg, "OK", 0, 1);
            set_foot("Access denied: not your process", 0xFF9AA0);
        } else {
            set_foot("The process has already exited", C_DIM);
        }
        return;
    }
    const char* warn = "";
    if (p->pid == 1) warn = "\n\nThis is init, the first process: the system will not work without it.";
    else if (p->pid == getppid()) warn = "\n\nThis is the desktop (secwm): every window will close.";
    else if (p->pid == getpid()) warn = "\n\nThis is the Task Manager itself.";
    snprintf(msg, sizeof(msg), "Do you want to end \"%s\" (PID %d)?\n\n"
             "If it has unsaved data, that data will be lost.%s", p->comm, p->pid, warn);
    T("TASKMGR-CONFIRM pid=%d name=%s", p->pid, p->comm);
    dlg_open(DLG_CONFIRM, "End task", msg, "End process", "Cancel", 1);
}

static void do_term(void) {
    if (!same_proc(dlg_t.pid, dlg_t.start, 0)) { set_foot("The process has already exited", C_DIM); return; }
    int r = kill(dlg_t.pid, SIGTERM);
    int er = r ? errno : 0;
    T("TASKMGR-TERM pid=%d rc=%d errno=%d", dlg_t.pid, r, er);
    char msg[200];
    if (r != 0) {
        snprintf(msg, sizeof(msg), "Could not end \"%s\": %s", dlg_t.name, strerror(er));
        set_foot(msg, 0xFF9AA0);
        return;
    }
    pending.active = 1; pending.pid = dlg_t.pid; pending.start = dlg_t.start;
    pending.deadline = tick_ms() + 3000;
    snprintf(pending.name, sizeof(pending.name), "%s", dlg_t.name);
    snprintf(msg, sizeof(msg), "Ending \"%s\" (PID %d)...", dlg_t.name, dlg_t.pid);
    set_foot(msg, C_DIM);
}
static void do_kill(void) {
    if (!pending.active) return;
    if (!same_proc(pending.pid, pending.start, 0)) return;
    int r = kill(pending.pid, SIGKILL);
    T("TASKMGR-KILL pid=%d rc=%d errno=%d", pending.pid, r, r ? errno : 0);
    pending.deadline = tick_ms() + 3000;
}
static void check_pending(void) {
    if (!pending.active) return;
    char st = 0;
    char msg[160];
    if (!same_proc(pending.pid, pending.start, &st) || st == 'Z') {
        T("TASKMGR-GONE pid=%d", pending.pid);
        snprintf(msg, sizeof(msg), "Ended \"%s\" (PID %d)", pending.name, pending.pid);
        set_foot(msg, 0x7FD9A8);
        pending.active = 0;
        if (dlg_kind == DLG_KILL) dlg_close();
        return;
    }
    if (!dlg && (int32_t)(tick_ms() - pending.deadline) > 0) {
        T("TASKMGR-KILLASK pid=%d", pending.pid);
        snprintf(msg, sizeof(msg), "\"%s\" (PID %d) is still running 3 seconds after the request to end.\n\n"
                 "End it immediately (SIGKILL)? It cannot save anything.", pending.name, pending.pid);
        dlg_open(DLG_KILL, "Not responding", msg, "End now", "Wait", 1);
    }
}

static void end_btn_cb(lv_event_t* e) { (void)e; end_task(); }

/* ---- tabs ------------------------------------------------------------------- */
static void tab_set(int t) {
    if (t < 0 || t >= NTAB) return;
    cur_tab = t;
    for (int i = 0; i < NTAB; i++) {
        int on = i == t;
        if (on) lv_obj_remove_flag(page[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(page[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(lv_obj_get_child(tabbtn[i], 0), lv_color_hex(on ? C_TEXT : C_DIM), 0);
        lv_obj_set_style_border_width(tabbtn[i], on ? 3 : 0, 0);
    }
    static const char* const nm[] = { "Processes", "Performance", "Users" };
    T("TASKMGR-TAB %d %s", t, nm[t]);
    if (t == TAB_PROC) { lview_damage(&lv_procs, 1); trace_rows(&lv_procs); }
    if (t == TAB_USERS) lview_damage(&lv_users, 1);
    if (t == TAB_PERF) { update_perf_texts(); side_set(cur_side); }
}
static void tab_cb(lv_event_t* e) { tab_set((int)(intptr_t)lv_event_get_user_data(e)); }

/* ---- performance tab widgets -------------------------------------------------- */
static void side_draw(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        side_t* s = (side_t*)lv_event_get_user_data(e);
        side_set((int)(s - side));
        update_perf_texts();
        return;
    }
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
    side_t* s = (side_t*)lv_event_get_user_data(e);
    lv_layer_t* l = lv_event_get_layer(e);
    lv_area_t a; lv_obj_get_coords(s->obj, &a);
    int on = (int)(s - side) == cur_side;
    d_rect(l, a.x1, a.y1, a.x2, a.y2, on ? 0x223142 : C_PANEL, LV_OPA_COVER, 6);
    if (on) d_rect(l, a.x1, a.y1 + 8, a.x1 + 2, a.y2 - 8, C_ACCENT, LV_OPA_COVER, 1);
    lv_area_t ga = { a.x1 + 10, a.y1 + 10, a.x1 + 66, a.y2 - 10 };
    draw_graph(l, &ga, s->g, 0, 0x10161C);
    char tmp[64];
    d_text(l, a.x1 + 78, a.y1 + 12, a.x2 - 6, fit(s->title, a.x2 - a.x1 - 84, &lv_font_montserrat_14, tmp, sizeof(tmp)),
           &lv_font_montserrat_14, C_TEXT, LV_TEXT_ALIGN_LEFT);
    d_text(l, a.x1 + 78, a.y1 + 32, a.x2 - 6, fit(s->sub, a.x2 - a.x1 - 84, &lv_font_montserrat_12, tmp, sizeof(tmp)),
           &lv_font_montserrat_12, C_DIM, LV_TEXT_ALIGN_LEFT);
}
static void graph_draw(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
    gview_t* gv = (gview_t*)lv_event_get_user_data(e);
    lv_layer_t* l = lv_event_get_layer(e);
    lv_area_t a; lv_obj_get_coords(lv_event_get_target(e), &a);
    draw_graph(l, &a, gv->g, 1, 0x0F151B);
    if (gv->idx >= 0) {
        char b[24]; snprintf(b, sizeof(b), "CPU %d   %d%%", gv->idx, (cpu10[1 + gv->idx] + 5) / 10);
        d_text(l, a.x1 + 6, a.y1 + 4, a.x2 - 6, b, &lv_font_montserrat_12, 0xA9C7E3, LV_TEXT_ALIGN_LEFT);
    }
}
static lv_obj_t* graph_obj(lv_obj_t* parent, gview_t* gv) {
    lv_obj_t* o = plain_obj(parent);
    lv_obj_add_event_cb(o, graph_draw, LV_EVENT_DRAW_MAIN, gv);
    return o;
}

static void build_cpu_grid(int pw, int ph) {
    int n = ncpu < 1 ? 1 : (ncpu > MAXCPU ? MAXCPU : ncpu);
    int gap = 6, cols = 1, best = -1;
    for (int k = 1; k <= n; k++) {             /* the layout with the largest cells, about 2:1 */
        int rws = (n + k - 1) / k;
        int cw = (pw - gap * (k - 1)) / k, chh = (ph - gap * (rws - 1)) / rws;
        int score = cw < chh * 2 ? cw : chh * 2;
        if (score > best) { best = score; cols = k; }
    }
    int rows = (n + cols - 1) / cols;
    int gw = (pw - gap * (cols - 1)) / cols, gh = (ph - gap * (rows - 1)) / rows;
    for (int i = 0; i < n; i++) {
        gv_cpu[i].g = &g_cpu[i]; gv_cpu[i].idx = i;
        g_cpu[i].nser = 1; g_cpu[i].col[0] = C_CPU; g_cpu[i].fixed_max = 1000;
        cpu_graph[i] = graph_obj(pane_cpu, &gv_cpu[i]);
        lv_obj_set_pos(cpu_graph[i], (i % cols) * (gw + gap), (i / cols) * (gh + gap));
        lv_obj_set_size(cpu_graph[i], gw, gh);
    }
    ncpu_graphs = n;
    perf_built_cpus = n;
    T("TASKMGR-CPUGRAPHS %d", n);
}

static void build_perf(lv_obj_t* pg, int pw, int ph) {
    int sw = 236;
    lv_obj_t* sbar = plain_obj(pg);
    lv_obj_set_size(sbar, sw, ph);
    lv_obj_set_style_bg_color(sbar, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(sbar, LV_OPA_COVER, 0);
    g_cpu_tot.nser = 1; g_cpu_tot.col[0] = C_CPU; g_cpu_tot.fixed_max = 1000;
    g_mem.nser = 1; g_mem.col[0] = C_MEM; g_mem.fixed_max = 1000;
    nside = 0;
    side[nside].kind = PK_CPU; side[nside].g = &g_cpu_tot; snprintf(side[nside].title, 32, "CPU"); nside++;
    side[nside].kind = PK_MEM; side[nside].g = &g_mem; snprintf(side[nside].title, 32, "Memory"); nside++;
    for (int i = 0; i < nnics && nside < 2 + MAXNIC; i++) {
        g_nic[i].nser = 2; g_nic[i].col[0] = C_NET; g_nic[i].col[1] = C_NET2; g_nic[i].fixed_max = 0;
        side[nside].kind = PK_NIC; side[nside].idx = i; side[nside].g = &g_nic[i];
        snprintf(side[nside].title, 32, "Ethernet"); nside++;
        T("TASKMGR-NIC %s", nics[i].name);
    }
    for (int i = 0; i < nside; i++) {
        side[i].obj = plain_obj(sbar);
        lv_obj_add_flag(side[i].obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(side[i].obj, 8, 8 + i * 66);
        lv_obj_set_size(side[i].obj, sw - 16, 60);
        lv_obj_add_event_cb(side[i].obj, side_draw, LV_EVENT_ALL, &side[i]);
    }
    int rx = sw + 18, rw = pw - rx - 18;
    perf_right = plain_obj(pg);
    lv_obj_set_pos(perf_right, rx, 0);
    lv_obj_set_size(perf_right, rw, ph);
    l_ptitle = label(perf_right, &lv_font_montserrat_28, C_TEXT, "CPU");
    lv_obj_set_pos(l_ptitle, 0, 10);
    l_psub = label(perf_right, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_set_width(l_psub, rw - 160);
    lv_label_set_long_mode(l_psub, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l_psub, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(l_psub, 160, 22);
    l_cap_l = label(perf_right, &lv_font_montserrat_12, C_DIM, "");
    lv_obj_set_pos(l_cap_l, 0, 54);
    l_leg1 = label(perf_right, &lv_font_montserrat_12, C_NET, LV_SYMBOL_DOWN " Receive");
    lv_obj_set_pos(l_leg1, 90, 54);
    l_leg2 = label(perf_right, &lv_font_montserrat_12, C_NET2, LV_SYMBOL_UP " Send");
    lv_obj_set_pos(l_leg2, 175, 54);
    l_cap_r = label(perf_right, &lv_font_montserrat_12, C_DIM, "");
    lv_obj_set_width(l_cap_r, 120);
    lv_obj_set_style_text_align(l_cap_r, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(l_cap_r, rw - 120, 54);
    int gy = 72, gh = ph - gy - 20 - 64;
    pane_cpu = plain_obj(perf_right); lv_obj_set_pos(pane_cpu, 0, gy); lv_obj_set_size(pane_cpu, rw, gh);
    pane_mem = plain_obj(perf_right); lv_obj_set_pos(pane_mem, 0, gy); lv_obj_set_size(pane_mem, rw, gh);
    pane_net = plain_obj(perf_right); lv_obj_set_pos(pane_net, 0, gy); lv_obj_set_size(pane_net, rw, gh);
    build_cpu_grid(rw, gh);
    gv_mem.g = &g_mem; gv_mem.idx = -1;
    big_graph_mem = graph_obj(pane_mem, &gv_mem); lv_obj_set_size(big_graph_mem, rw, gh);
    gv_net.g = &g_nic[0]; gv_net.idx = -1;
    big_graph_net = graph_obj(pane_net, &gv_net); lv_obj_set_size(big_graph_net, rw, gh);
    l_cap_b = label(perf_right, &lv_font_montserrat_12, C_DIM, "60 seconds");
    lv_obj_set_pos(l_cap_b, 0, gy + gh + 3);
    stats_box = plain_obj(perf_right);
    lv_obj_set_pos(stats_box, 0, ph - 60);
    lv_obj_set_size(stats_box, rw, 56);
    lv_obj_set_flex_flow(stats_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(stats_box, 22, 0);
    for (int i = 0; i < NSTAT; i++) stat_cell(stats_box, i, "");
}

/* ---- keyboard ---------------------------------------------------------------- */
static lview_t* cur_view(void) { return cur_tab == TAB_PROC ? &lv_procs : cur_tab == TAB_USERS ? &lv_users : 0; }
static void on_key(const struct swp_key* k) {
    if (k->value == 0) return;
    if (dlg) {
        if (k->code == KEY_ENTER || k->code == KEY_KPENTER) {
            int kind = dlg_kind; dlg_close();
            if (kind == DLG_CONFIRM) do_term(); else if (kind == DLG_KILL) do_kill();
        } else if (k->code == KEY_ESC) {
            int kind = dlg_kind; dlg_close();
            if (kind == DLG_KILL && pending.active) pending.deadline = tick_ms() + 3000;
        }
        return;
    }
    if ((k->mods & SWP_MOD_CTRL) && (k->code == KEY_TAB || k->code == KEY_PAGEDOWN)) {
        tab_set((cur_tab + ((k->mods & SWP_MOD_SHIFT) || k->code == KEY_PAGEUP ? NTAB - 1 : 1)) % NTAB); return;
    }
    if ((k->mods & SWP_MOD_CTRL) && k->code == KEY_PAGEUP) { tab_set((cur_tab + NTAB - 1) % NTAB); return; }
    if (k->code == KEY_F5) { refresh(); return; }
    if (cur_tab == TAB_PERF) {
        if (k->code == KEY_UP) { side_set(cur_side > 0 ? cur_side - 1 : 0); update_perf_texts(); }
        if (k->code == KEY_DOWN) { side_set(cur_side + 1 < nside ? cur_side + 1 : cur_side); update_perf_texts(); }
        return;
    }
    lview_t* v = cur_view();
    if (!v) return;
    int i = sel_index(v), page_rows = body_h(v) / ROW_H - 1;
    if (page_rows < 1) page_rows = 1;
    switch (k->code) {
    case KEY_UP: lview_select(v, i < 0 ? 0 : (i > 0 ? i - 1 : 0)); return;
    case KEY_DOWN: lview_select(v, i < 0 ? 0 : (i + 1 < v->n ? i + 1 : i)); return;
    case KEY_PAGEUP: lview_select(v, i - page_rows < 0 ? 0 : i - page_rows); return;
    case KEY_PAGEDOWN: lview_select(v, i + page_rows >= v->n ? v->n - 1 : i + page_rows); return;
    case KEY_HOME: lview_select(v, 0); return;
    case KEY_END: lview_select(v, v->n - 1); return;
    case KEY_DELETE: if (v == &lv_procs) end_task(); return;
    default: break;
    }
    /* type-ahead: the first row whose name starts with what was typed */
    if (k->cp >= 0x20 && k->cp < 0x7F && !(k->mods & (SWP_MOD_CTRL | SWP_MOD_ALT))) {
        uint32_t now = tick_ms();
        if (now - ta_ms > 1200) nta = 0;
        ta_ms = now;
        if (nta < (int)sizeof(typeahead) - 1) { typeahead[nta++] = (char)k->cp; typeahead[nta] = 0; }
        for (int r = 0; r < v->n; r++) {
            const char* s = v->rows[r].txt[0];
            int ok = 1;
            for (int q = 0; q < nta && ok; q++) if (lower((unsigned char)s[q]) != lower((unsigned char)typeahead[q])) ok = 0;
            if (ok) { lview_select(v, r); break; }
        }
    }
}

/* ---- build ---------------------------------------------------------------------- */
static lv_obj_t* tab_button(lv_obj_t* bar, int i, const char* txt, int x) {
    lv_obj_t* b = lv_button_create(bar);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, 40, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_set_height(b, TAB_H);
    lv_obj_set_pos(b, x, 0);
    lv_obj_t* l = label(b, &lv_font_montserrat_14, C_DIM, txt);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, tab_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    return b;
}

static void build_ui(void) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    /* tab bar */
    lv_obj_t* bar = plain_obj(scr);
    lv_obj_set_size(bar, WW, TAB_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    static const char* const tn[] = { "Processes", "Performance", "Users" };
    int x = 8;
    for (int i = 0; i < NTAB; i++) {
        tabbtn[i] = tab_button(bar, i, tn[i], x);
        lv_obj_update_layout(tabbtn[i]);
        x += lv_obj_get_width(tabbtn[i]) + 2;
    }
    char who[48];
    snprintf(who, sizeof(who), LV_SYMBOL_HOME "  %s", my_name);
    lv_obj_t* lw = label(bar, &lv_font_montserrat_12, C_DIM, who);
    lv_obj_align(lw, LV_ALIGN_RIGHT_MID, -14, 0);
    /* pages */
    int ph = WH - TAB_H - STAT_H;
    for (int i = 0; i < NTAB; i++) {
        page[i] = plain_obj(scr);
        lv_obj_set_pos(page[i], 0, TAB_H);
        lv_obj_set_size(page[i], WW, ph);
    }
    /* Processes */
    lview_init(&lv_procs, page[TAB_PROC], proc_cols, NCOL, WW, ph - FOOT_H, "processes");
    lv_procs.on_select = on_proc_select;
    lv_obj_t* foot = plain_obj(page[TAB_PROC]);
    lv_obj_set_pos(foot, 0, ph - FOOT_H);
    lv_obj_set_size(foot, WW, FOOT_H);
    lv_obj_set_style_bg_color(foot, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(foot, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_width(foot, 1, 0);
    l_foot = label(foot, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_align(l_foot, LV_ALIGN_LEFT_MID, 14, 0);
    btn_end = lv_button_create(foot);
    lv_obj_set_size(btn_end, 132, 34);
    lv_obj_align(btn_end, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_bg_color(btn_end, lv_color_hex(0x2A7FBF), 0);
    lv_obj_set_style_bg_color(btn_end, lv_color_hex(0x2A3440), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(btn_end, lv_color_hex(0x6A7785), LV_STATE_DISABLED);
    lv_obj_set_style_radius(btn_end, 6, 0);
    lv_obj_set_style_shadow_width(btn_end, 0, 0);
    lv_obj_t* le = label(btn_end, &lv_font_montserrat_14, 0xFFFFFF, LV_SYMBOL_CLOSE "  End task");
    lv_obj_set_style_text_color(le, lv_color_hex(0x6A7785), LV_STATE_DISABLED);
    lv_obj_center(le);
    lv_obj_add_event_cb(btn_end, end_btn_cb, LV_EVENT_CLICKED, 0);
    lv_obj_add_state(btn_end, LV_STATE_DISABLED);
    /* Performance */
    build_perf(page[TAB_PERF], WW, ph);
    /* Users */
    lview_init(&lv_users, page[TAB_USERS], user_cols, 6, WW, ph, "users");
    /* status bar */
    lv_obj_t* sb = plain_obj(scr);
    lv_obj_set_pos(sb, 0, WH - STAT_H);
    lv_obj_set_size(sb, WW, STAT_H);
    lv_obj_set_style_bg_color(sb, lv_color_hex(0x0E1318), 0);
    lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, 0);
    l_status = label(sb, &lv_font_montserrat_12, C_DIM, "");
    lv_obj_align(l_status, LV_ALIGN_LEFT_MID, 12, 0);
    l_status_r = label(sb, &lv_font_montserrat_12, C_FAINT, "SecOS Task Manager");
    lv_obj_align(l_status_r, LV_ALIGN_RIGHT_MID, -12, 0);
}

/* ---- SWP glue (see sysmon.c) ---------------------------------------------------- */
static void flush_cb(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
    (void)px;
    swpc_commit(&c, a->x1, a->y1, a->x2 - a->x1 + 1, a->y2 - a->y1 + 1);   /* each area its own PUT */
    lv_display_flush_ready(d);
}
static void read_cb(lv_indev_t* in, lv_indev_data_t* data) {
    (void)in;
    data->point.x = ptr_x; data->point.y = ptr_y;
    data->state = ptr_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void update_hover(void) {
    lview_t* v = cur_view();
    if (!v || dlg) return;
    lv_area_t a; lv_obj_get_coords(v->obj, &a);
    int h = -1;
    if (ptr_x >= a.x1 && ptr_x <= a.x2 && ptr_y >= a.y1 + HDR_H && ptr_y <= a.y2) {
        h = (ptr_y - a.y1 - HDR_H + v->scroll) / ROW_H;
        if (h >= v->n) h = -1;
    }
    if (h != v->hover) { v->hover = h; lview_damage(v, 0); }
}

int main(int argc, char** argv) {
    int maxw = 0, maxh = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) trace = 1;
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) {
            char* x = 0; maxw = (int)strtol(argv[++i], &x, 10);
            if (x && *x == 'x') maxh = (int)strtol(x + 1, 0, 10);
        }
    }
    if (maxw >= 400 && WW > maxw) WW = maxw;
    if (maxh >= 300 && WH > maxh) WH = maxh;
    if (swpc_open(&c, WW, WH, "Task Manager") < 0) {
        printf("taskmgr: not started by secwm (needs the SWP pipes on fds 3/4)\n");
        return 1;
    }
    WW = c.w; WH = c.h;
    my_uid = getuid();
    load_users();
    { char tmp[16]; snprintf(my_name, sizeof(my_name), "%s", user_name(my_uid, tmp, sizeof(tmp))); }
    read_cpuinfo();
    uint32_t t0 = tick_ms();
    sample_system(t0);           /* first sample: the next one gives the rates */
    sample_procs(t0);

    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t* disp = lv_display_create(c.w, c.h);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, c.px, NULL, (uint32_t)(c.w * c.h * 4), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_indev_t* in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, read_cb);
    build_ui();
    fill_proc_rows();
    fill_user_rows();
    tab_set(TAB_PROC);
    side_set(0);
    lv_timer_handler();
    printf("TASKMGR-READY %dx%d cpus=%d user=%s uid=%d\n", c.w, c.h, ncpu, my_name, my_uid);
    trace_obj("tab-processes", tabbtn[TAB_PROC]);
    trace_obj("tab-performance", tabbtn[TAB_PERF]);
    trace_obj("tab-users", tabbtn[TAB_USERS]);
    trace_obj("end-task", btn_end);
    trace_obj("proc-list", lv_procs.obj);
    uint32_t next = tick_ms() + 600, next_pending = 0;
    for (;;) {
        uint32_t now = tick_ms();
        if ((int32_t)(now - next) >= 0) { refresh(); next = now + 1000; }
        if (pending.active && (int32_t)(now - next_pending) >= 0) { check_pending(); next_pending = now + 250; }
        uint32_t wait = lv_timer_handler();
        if (wait == LV_NO_TIMER_READY || wait > 50) wait = 50;
        uint32_t until = next - tick_ms();
        if ((int32_t)until < 0) until = 0;
        if (wait > until) wait = until;
        struct swpc_event e;
        int r = swpc_wait(&c, &e, (int)wait);
        if (r < 0) break;
        while (r > 0) {
            if (e.type == SWP_EV_POINTER) {
                ptr_x = e.u.ptr.x; ptr_y = e.u.ptr.y; ptr_down = (e.u.ptr.buttons & 1) != 0;
                update_hover();
            } else if (e.type == SWP_EV_BUTTON && e.u.btn.button == 0x110) {
                ptr_x = e.u.btn.x; ptr_y = e.u.btn.y; ptr_down = e.u.btn.value != 0;
                lv_indev_read(in);          /* do not lose a quick click between two timer runs */
            } else if (e.type == SWP_EV_KEY) on_key(&e.u.key);
            else if (e.type == SWP_EV_CLOSE) return 0;
            r = swpc_wait(&c, &e, 0);
            if (r < 0) return 0;
        }
    }
    return 0;
}
