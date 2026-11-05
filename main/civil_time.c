#include "civil_time.h"

#define SECS_PER_DAY 86400

uint8_t civil_bin_to_bcd(uint8_t bin)
{
    return (uint8_t)(((bin / 10) << 4) | (bin % 10));
}

int civil_bcd_to_bin(uint8_t bcd)
{
    int hi = bcd >> 4;
    int lo = bcd & 0x0f;
    if (hi > 9 || lo > 9) return -1;
    return hi * 10 + lo;
}

bool civil_is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int civil_days_in_month(int year, int month)
{
    static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 0;
    if (month == 2 && civil_is_leap_year(year)) return 29;
    return days[month - 1];
}

bool civil_is_valid(const civil_time_t *t)
{
    return t->year >= 0 && t->year <= 9999 &&
           t->month >= 1 && t->month <= 12 &&
           t->day >= 1 && t->day <= civil_days_in_month(t->year, t->month) &&
           t->hour >= 0 && t->hour <= 23 &&
           t->minute >= 0 && t->minute <= 59 &&
           t->second >= 0 && t->second <= 59;
}

/* Days since 1970-01-01, H. Hinnant's days_from_civil: years start in March
   so the leap day is the last day of the year; 400-year eras of 146097 days. */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                                    /* 0..399 */
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;  /* 0..365 */
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            /* 0..146096 */
    return era * 146097 + doe - 719468;
}

int64_t civil_to_unix(const civil_time_t *t)
{
    return days_from_civil(t->year, t->month, t->day) * SECS_PER_DAY +
           t->hour * 3600 + t->minute * 60 + t->second;
}

void civil_from_unix(int64_t unix_sec, civil_time_t *out)
{
    int64_t days = unix_sec / SECS_PER_DAY;
    int64_t secs = unix_sec % SECS_PER_DAY;
    if (secs < 0) {
        secs += SECS_PER_DAY;
        days--;
    }

    /* Inverse of days_from_civil. */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;                                       /* 0..146096 */
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  /* 0..399 */
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                /* 0..365 */
    int64_t mp = (5 * doy + 2) / 153;                                     /* 0..11, March = 0 */
    int month = (int)(mp < 10 ? mp + 3 : mp - 9);

    out->year = (int)(yoe + era * 400 + (month <= 2));
    out->month = month;
    out->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    out->hour = (int)(secs / 3600);
    out->minute = (int)(secs / 60 % 60);
    out->second = (int)(secs % 60);
    /* 1970-01-01 was a Thursday. */
    int wd = (int)((days + 4) % 7);
    out->weekday = wd < 0 ? wd + 7 : wd;
}
