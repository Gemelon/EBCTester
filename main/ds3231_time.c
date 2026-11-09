/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ds3231_time.h"
#include <stddef.h>

static bool leap(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int month_days(int year, int month)
{
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return days[month - 1] + (month == 2 && leap(year));
}

static int bcd(uint8_t value)
{
    return (value & 15) <= 9 && (value >> 4) <= 9 ?
           (value >> 4) * 10 + (value & 15) : -1;
}

static uint8_t encode(int value)
{
    return (uint8_t)((value / 10) * 16 + value % 10);
}

bool ds3231_decode_time(const uint8_t r[7], int64_t *utc)
{
    if (r == NULL || utc == NULL || (r[2] & 0x80) || (r[5] & 0x60)) return false;
    int second = bcd(r[0]), minute = bcd(r[1]);
    int hour = bcd(r[2] & ((r[2] & 0x40) ? 0x1f : 0x3f));
    if (r[2] & 0x40) {
        if (hour < 1 || hour > 12) return false;
        hour = hour % 12 + ((r[2] & 0x20) ? 12 : 0);
    }
    int day = bcd(r[4]), month = bcd(r[5] & 0x1f), year = bcd(r[6]);
    if (year < 0 || month < 1 || month > 12 || day < 1 ||
        second < 0 || second > 59 || minute < 0 || minute > 59 ||
        hour < 0 || hour > 23 || r[3] < 1 || r[3] > 7) return false;
    year += (r[5] & 0x80) ? 2100 : 2000;
    if (day > month_days(year, month)) return false;
    int64_t days = 10957; /* Days from 1970-01-01 to 2000-01-01. */
    for (int y = 2000; y < year; ++y) days += leap(y) ? 366 : 365;
    for (int m = 1; m < month; ++m) days += month_days(year, m);
    int64_t value = (days + day - 1) * 86400 + hour * 3600 + minute * 60 + second;
    if (value < RTC_VALID_TIME_MIN) return false;
    *utc = value;
    return true;
}

bool ds3231_encode_time(int64_t utc, uint8_t r[7])
{
    if (r == NULL || utc < RTC_VALID_TIME_MIN || utc >= INT64_C(7258118400)) return false;
    int64_t days = utc / 86400;
    int remaining = (int)(days - 10957), year = 2000, month = 1;
    while (remaining >= (leap(year) ? 366 : 365)) {
        remaining -= leap(year) ? 366 : 365;
        ++year;
    }
    while (remaining >= month_days(year, month)) {
        remaining -= month_days(year, month);
        ++month;
    }
    r[0] = encode((int)(utc % 60));
    r[1] = encode((int)(utc / 60 % 60));
    r[2] = encode((int)(utc / 3600 % 24));
    r[3] = (uint8_t)((days + 4) % 7 + 1);
    r[4] = encode(remaining + 1);
    r[5] = encode(month) | (year >= 2100 ? 0x80 : 0);
    r[6] = encode(year % 100);
    return true;
}
