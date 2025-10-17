/*
 * SecOS Kernel - RTC Driver (Header)
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef RTC_H
#define RTC_H
#include <stdint.h>
#include <stdbool.h>
#include "config.h"

#if ENABLE_RTC
struct rtc_datetime {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year; // full year (e.g., 2025)
};

bool rtc_read(struct rtc_datetime* dt);
void rtc_format(const struct rtc_datetime* dt, char* buf, uint32_t bufsize);
#endif // ENABLE_RTC

#endif // RTC_H
