/*
 * kclock.c — kernel wall clock + POSIX clocks. See kclock.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "kclock.h"
#include "config.h"
#include "rtc.h"
#include "spinlock.h"
#include "debugcon.h"

extern uint64_t ktime_ns(void);
extern uint64_t timer_get_ticks(void);
extern uint32_t timer_get_frequency(void);

static int        g_kclock_ready;
static uint64_t   g_real_base_ns;   /* REALTIME = g_real_base_ns + mono */
static spinlock_t g_kclock_lock = SPINLOCK_INIT;

uint64_t kclock_mono_ns(void) {
    uint64_t ns = ktime_ns();
    if (ns) return ns;
    uint32_t hz = timer_get_frequency();
    if (!hz) hz = 1000;
    return timer_get_ticks() * (1000000000ull / hz);
}

/* Days since 1970-01-01 of a proleptic-Gregorian civil date (H. Hinnant). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

uint64_t kclock_civil_to_unix(int year, int mon, int day, int h, int m, int s) {
    int64_t days = days_from_civil(year, (unsigned)mon, (unsigned)day);
    if (days < 0) return 0;
    return (uint64_t)days * 86400ull + (uint64_t)(h * 3600 + m * 60 + s);
}

void kclock_unix_to_civil(uint64_t t, int* year, int* mon, int* day, int* h, int* m, int* s) {
    uint64_t days = t / 86400, rem = t % 86400;
    if (h) *h = (int)(rem / 3600);
    if (m) *m = (int)(rem % 3600 / 60);
    if (s) *s = (int)(rem % 60);
    int64_t z = (int64_t)days + 719468;
    int64_t era = z / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned mo = mp < 10 ? mp + 3 : mp - 9;
    if (year) *year = (int)(y + (mo <= 2));
    if (mon) *mon = (int)mo;
    if (day) *day = (int)d;
}

static void dc_dec(uint64_t v, int width) {
    char b[24]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n < width) b[n++] = '0';
    while (n--) debugcon_putchar(b[n]);
}

void kclock_init(void) {
    if (g_kclock_ready) return;
    uint64_t epoch = 0;
#if ENABLE_RTC
    struct rtc_datetime a, b;
    /* Read until two consecutive reads agree: an update between the register
     * reads of one rtc_read() can otherwise tear minute/hour/day rollovers. */
    for (int tries = 0; tries < 8; tries++) {
        if (!rtc_read(&a) || !rtc_read(&b)) break;
        if (a.second == b.second && a.minute == b.minute && a.hour == b.hour &&
            a.day == b.day && a.month == b.month && a.year == b.year) {
            if (a.month >= 1 && a.month <= 12 && a.day >= 1 && a.day <= 31 &&
                a.hour < 24 && a.minute < 60 && a.second < 60)
                epoch = kclock_civil_to_unix(a.year, a.month, a.day, a.hour, a.minute, a.second);
            break;
        }
    }
#endif
    uint64_t mono = kclock_mono_ns();
    uint64_t fl = spin_lock_irqsave(&g_kclock_lock);
    if (!g_kclock_ready) {
        uint64_t real = epoch * 1000000000ull;
        g_real_base_ns = real > mono ? real - mono : 0;
        g_kclock_ready = 1;
    }
    spin_unlock_irqrestore(&g_kclock_lock, fl);
    int y, mo, d, h, mi, s;
    kclock_unix_to_civil(epoch, &y, &mo, &d, &h, &mi, &s);
    debugcon_writestring("[CLOCK] RTC ");
    dc_dec((uint64_t)y, 4); debugcon_putchar('-'); dc_dec((uint64_t)mo, 2); debugcon_putchar('-');
    dc_dec((uint64_t)d, 2); debugcon_putchar(' '); dc_dec((uint64_t)h, 2); debugcon_putchar(':');
    dc_dec((uint64_t)mi, 2); debugcon_putchar(':'); dc_dec((uint64_t)s, 2);
    debugcon_writestring(" UTC epoch="); dc_dec(epoch, 1); debugcon_putchar('\n');
}

uint64_t kclock_real_ns(void) {
    if (!g_kclock_ready) kclock_init();
    return g_real_base_ns + kclock_mono_ns();
}

uint64_t kclock_real_sec(void) { return kclock_real_ns() / 1000000000ull; }

void kclock_set_real_ns(uint64_t ns) {
    if (!g_kclock_ready) kclock_init();
    uint64_t mono = kclock_mono_ns();
    uint64_t fl = spin_lock_irqsave(&g_kclock_lock);
    g_real_base_ns = ns > mono ? ns - mono : 0;
    spin_unlock_irqrestore(&g_kclock_lock, fl);
}
