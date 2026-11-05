#include <stdio.h>
#include "civil_time.h"

static int failures = 0;

#define EXPECT(cond, label) do { \
    if (cond) { printf("PASS %s\n", label); } \
    else { printf("FAIL %s\n", label); failures++; } \
} while (0)

static bool same_time(const civil_time_t *a, const civil_time_t *b)
{
    return a->year == b->year && a->month == b->month && a->day == b->day &&
           a->hour == b->hour && a->minute == b->minute && a->second == b->second;
}

/* civil -> unix gives `unix_sec`, and unix -> civil gives the date back with `weekday`. */
static void check_both_ways(civil_time_t t, int64_t unix_sec, int weekday, const char *label)
{
    char buf[96];
    civil_time_t back;

    snprintf(buf, sizeof(buf), "%s: valid", label);
    EXPECT(civil_is_valid(&t), buf);
    snprintf(buf, sizeof(buf), "%s: to unix", label);
    EXPECT(civil_to_unix(&t) == unix_sec, buf);
    civil_from_unix(unix_sec, &back);
    snprintf(buf, sizeof(buf), "%s: from unix", label);
    EXPECT(same_time(&t, &back), buf);
    snprintf(buf, sizeof(buf), "%s: weekday", label);
    EXPECT(back.weekday == weekday, buf);
}

static civil_time_t make(int y, int mo, int d, int h, int mi, int s)
{
    civil_time_t t = { .year = y, .month = mo, .day = d, .hour = h, .minute = mi, .second = s };
    return t;
}

int main(void)
{
    /* Reference values from Python calendar.timegm; weekday 0 = Sunday. */
    check_both_ways(make(1970, 1, 1, 0, 0, 0), 0, 4, "epoch 0");
    check_both_ways(make(2000, 2, 29, 0, 0, 0), 951782400LL, 2, "2000-02-29 (400-year leap)");
    check_both_ways(make(2000, 2, 29, 12, 34, 56), 951827696LL, 2, "2000-02-29 12:34:56");
    check_both_ways(make(2024, 1, 1, 0, 0, 0), 1704067200LL, 1, "2024-01-01 (min valid)");
    check_both_ways(make(2024, 12, 31, 23, 59, 59), 1735689599LL, 2, "2024-12-31 23:59:59");
    check_both_ways(make(2038, 1, 19, 3, 14, 7), 2147483647LL, 2, "2038 last 32-bit second");
    check_both_ways(make(2038, 1, 19, 3, 14, 8), 2147483648LL, 2, "2038 first 33-bit second");
    check_both_ways(make(2040, 2, 29, 6, 7, 8), 2214108428LL, 3, "2040-02-29 06:07:08");
    check_both_ways(make(2099, 12, 31, 23, 59, 59), 4102444799LL, 4, "2099-12-31 23:59:59");
    check_both_ways(make(2100, 3, 1, 0, 0, 0), 4107542400LL, 1, "2100-03-01 (no leap day)");
    check_both_ways(make(1969, 12, 31, 23, 59, 59), -1, 3, "one second before epoch");

    /* Every day from 1999 to 2101 converts back to itself, one day apart. */
    bool days_ok = true;
    int64_t prev = civil_to_unix(&(civil_time_t){ .year = 1998, .month = 12, .day = 31 });
    for (int y = 1999; y <= 2101 && days_ok; y++) {
        for (int m = 1; m <= 12; m++) {
            for (int d = 1; d <= civil_days_in_month(y, m); d++) {
                civil_time_t t = make(y, m, d, 0, 0, 0);
                civil_time_t back;
                int64_t s = civil_to_unix(&t);
                civil_from_unix(s, &back);
                if (s - prev != 86400 || !same_time(&t, &back)) days_ok = false;
                prev = s;
            }
        }
    }
    EXPECT(days_ok, "every day 1999..2101 round trips and is 86400 s long");

    EXPECT(civil_is_leap_year(2000) && civil_is_leap_year(2024) &&
           !civil_is_leap_year(2100) && !civil_is_leap_year(2023), "leap years");
    EXPECT(civil_days_in_month(2024, 2) == 29 && civil_days_in_month(2023, 2) == 28 &&
           civil_days_in_month(2024, 4) == 30 && civil_days_in_month(2024, 12) == 31 &&
           civil_days_in_month(2024, 13) == 0, "days in month");

    /* BCD */
    bool bcd_ok = true;
    for (int v = 0; v <= 99; v++) {
        if (civil_bcd_to_bin(civil_bin_to_bcd((uint8_t)v)) != v) bcd_ok = false;
    }
    EXPECT(bcd_ok, "BCD round trip 0..99");
    EXPECT(civil_bin_to_bcd(59) == 0x59 && civil_bin_to_bcd(7) == 0x07, "bin to BCD");
    EXPECT(civil_bcd_to_bin(0x23) == 23 && civil_bcd_to_bin(0x99) == 99, "BCD to bin");
    EXPECT(civil_bcd_to_bin(0x1A) == -1, "BCD low digit above 9 rejected");
    EXPECT(civil_bcd_to_bin(0xA0) == -1, "BCD high digit above 9 rejected");
    EXPECT(civil_bcd_to_bin(0xFF) == -1, "BCD 0xFF rejected");

    /* Invalid fields */
    civil_time_t t = make(2024, 6, 15, 12, 30, 30);
    EXPECT(civil_is_valid(&t), "ordinary date valid");
    t = make(2024, 0, 15, 12, 30, 30);
    EXPECT(!civil_is_valid(&t), "month 0 invalid");
    t = make(2024, 13, 15, 12, 30, 30);
    EXPECT(!civil_is_valid(&t), "month 13 invalid");
    t = make(2024, 6, 0, 12, 30, 30);
    EXPECT(!civil_is_valid(&t), "day 0 invalid");
    t = make(2024, 4, 31, 12, 30, 30);
    EXPECT(!civil_is_valid(&t), "April 31 invalid");
    t = make(2023, 2, 29, 12, 30, 30);
    EXPECT(!civil_is_valid(&t), "2023-02-29 invalid");
    t = make(2100, 2, 29, 12, 30, 30);
    EXPECT(!civil_is_valid(&t), "2100-02-29 invalid");
    t = make(2024, 2, 29, 12, 30, 30);
    EXPECT(civil_is_valid(&t), "2024-02-29 valid");
    t = make(2024, 6, 15, 24, 0, 0);
    EXPECT(!civil_is_valid(&t), "hour 24 invalid");
    t = make(2024, 6, 15, 12, 60, 0);
    EXPECT(!civil_is_valid(&t), "minute 60 invalid");
    t = make(2024, 6, 15, 12, 30, 60);
    EXPECT(!civil_is_valid(&t), "second 60 invalid");
    t = make(2024, 6, 15, -1, 30, 30);
    EXPECT(!civil_is_valid(&t), "negative hour (bad BCD) invalid");
    t = make(2024, 6, 15, 12, 30, -1);
    EXPECT(!civil_is_valid(&t), "negative second (bad BCD) invalid");
    t = make(10000, 1, 1, 0, 0, 0);
    EXPECT(!civil_is_valid(&t), "year 10000 invalid");

    return failures == 0 ? 0 : 1;
}
