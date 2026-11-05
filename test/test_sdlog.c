#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sdlog.h"

static int failures = 0;

#define EXPECT(cond, label) do { \
    if (cond) { printf("PASS %s\n", label); } \
    else { printf("FAIL %s\n", label); failures++; } \
} while (0)

static reading_t make_reading(float temp, float hum, int batt, int rssi)
{
    reading_t r = {0};
    r.temp_c = temp;
    r.humidity = hum;
    r.battery = batt;
    r.rssi = rssi;
    return r;
}

static void check_row(const char *label, const reading_t *r, time_t now, const char *want)
{
    char buf[128];
    int n = sdlog_format_row(buf, sizeof buf, r, now);
    if (n == (int)strlen(want) && strcmp(buf, want) == 0) {
        printf("PASS %s\n", label);
    } else {
        printf("FAIL %s: got \"%s\" (%d), want \"%s\"\n", label, n < 0 ? "" : buf, n, want);
        failures++;
    }
}

static void check_path(const char *label, time_t now, const char *want)
{
    char buf[32];
    bool ok = sdlog_month_path(buf, sizeof buf, now);
    if (ok && strcmp(buf, want) == 0) {
        printf("PASS %s\n", label);
    } else {
        printf("FAIL %s: got \"%s\", want \"%s\"\n", label, ok ? buf : "(false)", want);
        failures++;
    }
}

int main(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    reading_t r = make_reading(12.26f, 63.7f, 62, -86);

    /* 2026-11-05 13:00:12 UTC = 14:00:12 CET (UTC+1) */
    check_row("winter row", &r, 1793883612, "2026-11-05 14:00:12,1793883612,12.26,63.7,62,-86");
    /* 2026-07-15 12:00:12 UTC = 14:00:12 CEST (UTC+2) */
    check_row("summer row", &r, 1784116812, "2026-07-15 14:00:12,1784116812,12.26,63.7,62,-86");

    /* DST switches: 2026-03-29 01:00 UTC and 2026-10-25 01:00 UTC */
    check_row("last second of CET", &r, 1774745999, "2026-03-29 01:59:59,1774745999,12.26,63.7,62,-86");
    check_row("first second of CEST", &r, 1774746000, "2026-03-29 03:00:00,1774746000,12.26,63.7,62,-86");
    check_row("last second of CEST", &r, 1792889999, "2026-10-25 02:59:59,1792889999,12.26,63.7,62,-86");
    check_row("repeated hour in CET", &r, 1792890000, "2026-10-25 02:00:00,1792890000,12.26,63.7,62,-86");

    reading_t no_batt = make_reading(21.5f, 40.0f, -1, -70);
    check_row("unknown battery is empty field", &no_batt, 1793883612,
              "2026-11-05 14:00:12,1793883612,21.50,40.0,,-70");

    reading_t cold = make_reading(-5.25f, 88.4f, 100, -101);
    check_row("negative temperature", &cold, 1793883612,
              "2026-11-05 14:00:12,1793883612,-5.25,88.4,100,-101");
    reading_t just_below = make_reading(-0.5f, 99.9f, 0, -40);
    check_row("temperature just below zero", &just_below, 1793883612,
              "2026-11-05 14:00:12,1793883612,-0.50,99.9,0,-40");

    check_path("month path", 1793883612, "/sdcard/2026-11.csv");
    check_path("summer month path", 1784116812, "/sdcard/2026-07.csv");
    /* 2026-10-31 23:30 UTC is already 2026-11-01 00:30 CET */
    check_path("month boundary in local time", 1793489400, "/sdcard/2026-11.csv");
    /* 2026-12-31 23:30 UTC is already 2027-01-01 00:30 CET */
    check_path("year boundary in local time", 1798759800, "/sdcard/2027-01.csv");

    /* Buffers one byte short of the terminating NUL. */
    const char *want = "2026-11-05 14:00:12,1793883612,12.26,63.7,62,-86";
    size_t want_len = strlen(want);
    char buf[128];
    EXPECT(sdlog_format_row(buf, want_len, &r, 1793883612) == -1, "row buffer too small");
    EXPECT(sdlog_format_row(buf, want_len + 1, &r, 1793883612) == (int)want_len, "row buffer exact fit");
    EXPECT(sdlog_format_row(buf, 0, &r, 1793883612) == -1, "row buffer of zero length");

    size_t path_len = strlen("/sdcard/2026-11.csv");
    EXPECT(!sdlog_month_path(buf, path_len, 1793883612), "path buffer too small");
    EXPECT(sdlog_month_path(buf, path_len + 1, 1793883612), "path buffer exact fit");
    EXPECT(!sdlog_month_path(buf, 0, 1793883612), "path buffer of zero length");

    return failures == 0 ? 0 : 1;
}
