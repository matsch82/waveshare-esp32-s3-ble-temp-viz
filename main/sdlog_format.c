#include <stdio.h>
#include <time.h>
#include "sdlog.h"

/* Plain C, no ESP headers: built into the host test as well. */

bool sdlog_month_path(char *buf, size_t len, time_t now)
{
    struct tm tm;
    if (!localtime_r(&now, &tm)) return false;
    int n = snprintf(buf, len, SDLOG_MOUNT_POINT "/%04d-%02d.csv",
                     tm.tm_year + 1900, tm.tm_mon + 1);
    return n >= 0 && (size_t)n < len;
}

int sdlog_format_row(char *buf, size_t len, const reading_t *r, time_t now)
{
    struct tm tm;
    if (!localtime_r(&now, &tm)) return -1;

    char battery[12] = "";
    if (r->battery >= 0) {
        snprintf(battery, sizeof battery, "%d", r->battery);
    }

    int n = snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d,%lld,%.2f,%.1f,%s,%d",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, (long long)now,
                     r->temp_c, r->humidity, battery, r->rssi);
    if (n < 0 || (size_t)n >= len) return -1;
    return n;
}
