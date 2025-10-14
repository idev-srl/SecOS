#include "rtc.h"
#include "terminal.h"
#if ENABLE_RTC

static inline void outb(uint16_t port, uint8_t val) { __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

static inline uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg | 0x80); // NMI off
    return inb(0x71);
}

static inline bool cmos_update_in_progress(void) {
    outb(0x70, 0x0A);
    uint8_t statusA = inb(0x71);
    return (statusA & 0x80) != 0;
}

static uint8_t bcd_to_bin(uint8_t v) { return (uint8_t)((v & 0x0F) + ((v >> 4) * 10)); }

static void itoa2(int v, char* b){ b[0] = (char)('0' + (v/10)); b[1] = (char)('0' + (v%10)); }

bool rtc_read(struct rtc_datetime* dt) {
    if (!dt) return false;

    // Attendi fine update
    while (cmos_update_in_progress()) { }

    uint8_t second = cmos_read(0x00);
    uint8_t minute = cmos_read(0x02);
    uint8_t hour   = cmos_read(0x04);
    uint8_t day    = cmos_read(0x07);
    uint8_t month  = cmos_read(0x08);
    uint8_t year   = cmos_read(0x09);
    outb(0x70, 0x0B);
    uint8_t regB = inb(0x71);

    bool is_binary = (regB & 0x04) != 0;
    bool is_24h    = (regB & 0x02) != 0;

    if (!is_binary) {
        second = bcd_to_bin(second);
        minute = bcd_to_bin(minute);
        hour   = bcd_to_bin(hour);
        day    = bcd_to_bin(day);
        month  = bcd_to_bin(month);
        year   = bcd_to_bin(year);
    }

    if (!is_24h) { // converti formato 12h -> 24h
        bool pm = (hour & 0x80) != 0;
        hour &= 0x7F;
        if (pm && hour < 12) hour += 12;
        if (!pm && hour == 12) hour = 0;
    }

    dt->second = second;
    dt->minute = minute;
    dt->hour   = hour;
    dt->day    = day;
    dt->month  = month;
    dt->year   = (uint16_t)(2000 + year); // Assumendo 20xx
    return true;
}

void rtc_format(const struct rtc_datetime* dt, char* buf, uint32_t sz) {
    if (!dt || !buf || sz < 20) return;
    // Formato: YYYY-MM-DD HH:MM:SS
    buf[0] = '0' + ((dt->year / 1000) % 10);
    buf[1] = '0' + ((dt->year / 100 ) % 10);
    buf[2] = '0' + ((dt->year / 10  ) % 10);
    buf[3] = '0' + ((dt->year       ) % 10);
    buf[4] = '-'; itoa2(dt->month, &buf[5]); buf[7]='-'; itoa2(dt->day,&buf[8]); buf[10]=' ';
    itoa2(dt->hour,&buf[11]); buf[13]=':'; itoa2(dt->minute,&buf[14]); buf[16]=':'; itoa2(dt->second,&buf[17]); buf[19]='\0';
}

#endif
