/* SecOS - LVGL system monitor, an SWP client of secwm (/bin/sysmon).
 * SPDX-License-Identifier: MIT
 *
 * LVGL 9.2 (MIT, built from source by tools/port-lvgl.sh) renders straight
 * into the client's local window buffer (DIRECT mode, XRGB8888); the flush
 * callback sends the changed area to secwm with SWP PUT. Pointer events from
 * secwm feed an LVGL pointer input device. Signed with the confined manifest
 * (CAP_ENFORCE | CAP_FS_READ | CAP_TIME): it reads /proc and draws, nothing
 * else.
 *
 * Overview tab: uptime, load, memory gauge + 60 s memory chart.
 * Processes tab: PID / state / RSS / command from /proc/<pid>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include "libsecos.h"
#include "swpc.h"
#include "lvgl.h"

#define WW 500
#define WH 400

static swpc_t c;
static lv_area_t dirty; static int have_dirty;
static int ptr_x, ptr_y, ptr_down;

static uint32_t tick_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

static void flush_cb(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
    (void)px;
    if (!have_dirty) { dirty = *a; have_dirty = 1; }
    else {
        if (a->x1 < dirty.x1) dirty.x1 = a->x1;
        if (a->y1 < dirty.y1) dirty.y1 = a->y1;
        if (a->x2 > dirty.x2) dirty.x2 = a->x2;
        if (a->y2 > dirty.y2) dirty.y2 = a->y2;
    }
    if (lv_display_flush_is_last(d)) {
        swpc_commit(&c, dirty.x1, dirty.y1, dirty.x2 - dirty.x1 + 1, dirty.y2 - dirty.y1 + 1);
        have_dirty = 0;
    }
    lv_display_flush_ready(d);
}

static void read_cb(lv_indev_t* in, lv_indev_data_t* data) {
    (void)in;
    data->point.x = ptr_x; data->point.y = ptr_y;
    data->state = ptr_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ---- /proc ---------------------------------------------------------------- */
static int read_file(const char* path, char* buf, int max) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = (int)read(fd, buf, (size_t)max - 1);
    close(fd);
    if (n < 0) n = 0;
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

/* ---- UI ------------------------------------------------------------------- */
static lv_obj_t *l_uptime, *l_load, *l_procs, *l_mem, *arc_mem, *l_arc, *chart, *table, *l_version;
static lv_chart_series_t* ser;

static void refresh(void) {
    char buf[2048], s[96];
    double up = 0;
    if (read_file("/proc/uptime", buf, sizeof(buf)) > 0) up = strtod(buf, 0);
    long u = (long)up;
    snprintf(s, sizeof(s), "%ldd %02ld:%02ld:%02ld", u / 86400, (u / 3600) % 24, (u / 60) % 60, u % 60);
    lv_label_set_text(l_uptime, s);
    if (read_file("/proc/loadavg", buf, sizeof(buf)) > 0) {
        char* nl = strchr(buf, '\n'); if (nl) *nl = 0;
        snprintf(s, sizeof(s), "Load average  %s", buf);
        lv_label_set_text(l_load, s);
    }
    if (read_file("/proc/meminfo", buf, sizeof(buf)) > 0) {
        long tot = kv(buf, "MemTotal"), fr = kv(buf, "MemFree"), av = kv(buf, "MemAvailable");
        if (av < 0 || av > tot) av = fr;
        long used = tot - av;
        if (used < 0) used = 0;
        int pct = tot > 0 ? (int)(used * 100 / tot) : 0;
        lv_arc_set_value(arc_mem, pct);
        snprintf(s, sizeof(s), "%d%%", pct);
        lv_label_set_text(l_arc, s);
        snprintf(s, sizeof(s), "Memory  %ld / %ld MB", used / 1024, tot / 1024);
        lv_label_set_text(l_mem, s);
        lv_chart_set_next_value(chart, ser, (int32_t)(used / 1024));
        lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, (int32_t)(tot / 1024));
    }
    /* processes */
    DIR* d = opendir("/proc");
    int row = 1;
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) && row < 40) {
            int pid = atoi(e->d_name);
            if (pid <= 0) continue;
            char path[64], comm[32] = "?", st[4] = "?";
            long rss = 0;
            snprintf(path, sizeof(path), "/proc/%d/comm", pid);
            if (read_file(path, buf, sizeof(buf)) > 0) {
                char* nl = strchr(buf, '\n'); if (nl) *nl = 0;
                snprintf(comm, sizeof(comm), "%s", buf);
            }
            snprintf(path, sizeof(path), "/proc/%d/status", pid);
            if (read_file(path, buf, sizeof(buf)) > 0) {
                char* p = strstr(buf, "State:");
                if (p) { p += 6; while (*p == ' ' || *p == '\t') p++; st[0] = *p; st[1] = 0; }
                long r = kv(buf, "VmRSS");
                if (r >= 0) rss = r;
            }
            snprintf(s, sizeof(s), "%d", pid);
            lv_table_set_cell_value(table, row, 0, s);
            lv_table_set_cell_value(table, row, 1, st);
            snprintf(s, sizeof(s), "%ld kB", rss);
            lv_table_set_cell_value(table, row, 2, s);
            lv_table_set_cell_value(table, row, 3, comm);
            row++;
        }
        closedir(d);
    }
    lv_table_set_row_count(table, (uint32_t)row);
    snprintf(s, sizeof(s), "Processes  %d", row - 1);
    lv_label_set_text(l_procs, s);
}

static void build_ui(void) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x121820), 0);
    lv_obj_t* tv = lv_tabview_create(scr);
    lv_tabview_set_tab_bar_size(tv, 40);
    lv_obj_t* t1 = lv_tabview_add_tab(tv, "Overview");
    lv_obj_t* t2 = lv_tabview_add_tab(tv, "Processes");
    lv_obj_t* t3 = lv_tabview_add_tab(tv, "About");

    /* Overview */
    lv_obj_set_flex_flow(t1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t1, 8, 0);
    lv_obj_t* top = lv_obj_create(t1);
    lv_obj_set_size(top, lv_pct(100), 150);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    arc_mem = lv_arc_create(top);
    lv_obj_set_size(arc_mem, 120, 120);
    lv_arc_set_range(arc_mem, 0, 100);
    lv_obj_remove_flag(arc_mem, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(arc_mem, lv_color_hex(0x2BB3A8), LV_PART_INDICATOR);
    lv_obj_remove_style(arc_mem, NULL, LV_PART_KNOB);
    l_arc = lv_label_create(arc_mem);
    lv_obj_set_style_text_font(l_arc, &lv_font_montserrat_20, 0);
    lv_obj_center(l_arc);
    lv_obj_t* col = lv_obj_create(top);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, lv_pct(100));
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_bg_opa(col, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* cap = lv_label_create(col);
    lv_label_set_text(cap, "Uptime");
    lv_obj_set_style_text_color(cap, lv_color_hex(0x8A97A6), 0);
    l_uptime = lv_label_create(col);
    lv_obj_set_style_text_font(l_uptime, &lv_font_montserrat_28, 0);
    l_load = lv_label_create(col);
    l_mem = lv_label_create(col);
    l_procs = lv_label_create(col);

    chart = lv_chart_create(t1);
    lv_obj_set_size(chart, lv_pct(100), 120);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, 60);
    lv_chart_set_div_line_count(chart, 3, 6);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
    ser = lv_chart_add_series(chart, lv_color_hex(0xF0B429), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);
    lv_obj_t* cl = lv_label_create(t1);
    lv_label_set_text(cl, "Memory used (MB), last 60 s");
    lv_obj_set_style_text_color(cl, lv_color_hex(0x8A97A6), 0);

    /* Processes */
    table = lv_table_create(t2);
    lv_obj_set_width(table, lv_pct(100));
    lv_table_set_column_count(table, 4);
    lv_table_set_column_width(table, 0, 70);
    lv_table_set_column_width(table, 1, 60);
    lv_table_set_column_width(table, 2, 110);
    lv_table_set_column_width(table, 3, 200);
    lv_table_set_cell_value(table, 0, 0, "PID");
    lv_table_set_cell_value(table, 0, 1, "S");
    lv_table_set_cell_value(table, 0, 2, "RSS");
    lv_table_set_cell_value(table, 0, 3, "Command");
    lv_obj_set_style_pad_ver(table, 4, LV_PART_ITEMS);

    /* About */
    lv_obj_set_flex_flow(t3, LV_FLEX_FLOW_COLUMN);
    lv_obj_t* h = lv_label_create(t3);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_20, 0);
    lv_label_set_text(h, "SecOS system monitor");
    l_version = lv_label_create(t3);
    lv_label_set_long_mode(l_version, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l_version, lv_pct(100));
    char buf[256];
    if (read_file("/proc/version", buf, sizeof(buf)) <= 0) strcpy(buf, "?");
    lv_label_set_text(l_version, buf);
    lv_obj_t* a = lv_label_create(t3);
    lv_label_set_long_mode(a, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a, lv_pct(100));
    lv_label_set_text_fmt(a, "LVGL %d.%d.%d (MIT), an SWP client of secwm.\n"
                             "Confined: reads /proc, draws its window, nothing else.",
                          LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
}

int main(void) {
    if (swpc_open(&c, WW, WH, "System monitor (LVGL)") < 0) {
        printf("sysmon: not started by secwm (needs the SWP pipes on fds 3/4)\n");
        return 1;
    }
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
    refresh();
    printf("SYSMON-READY %dx%d\n", c.w, c.h);
    for (;;) {
        uint32_t wait = lv_timer_handler();
        if (wait == LV_NO_TIMER_READY || wait > 50) wait = 50;
        struct swpc_event e;
        int r = swpc_wait(&c, &e, (int)wait);
        if (r < 0) break;
        while (r > 0) {
            if (e.type == SWP_EV_POINTER) { ptr_x = e.u.ptr.x; ptr_y = e.u.ptr.y; ptr_down = (e.u.ptr.buttons & 1) != 0; }
            else if (e.type == SWP_EV_BUTTON && e.u.btn.button == 0x110) {
                ptr_x = e.u.btn.x; ptr_y = e.u.btn.y; ptr_down = e.u.btn.value != 0;
                lv_indev_read(in);          /* do not lose a quick click between two timer runs */
            }
            else if (e.type == SWP_EV_FRAME) refresh();
            else if (e.type == SWP_EV_CLOSE) return 0;
            else if (e.type == SWP_EV_KEY && e.u.key.value == 1 && (e.u.key.cp == 'q' || e.u.key.code == 1)) return 0;
            r = swpc_wait(&c, &e, 0);
            if (r < 0) return 0;
        }
    }
    return 0;
}
