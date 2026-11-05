#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pure date math for the RTC: BCD helpers and conversion between a
 * broken-down UTC date/time (proleptic Gregorian calendar) and Unix seconds.
 * No ESP-IDF dependencies, so it is unit-tested on the host.
 */

typedef struct {
    int year;       /* e.g. 2024 */
    int month;      /* 1..12 */
    int day;        /* 1..31 */
    int hour;       /* 0..23 */
    int minute;     /* 0..59 */
    int second;     /* 0..59 */
    int weekday;    /* 0..6, 0 = Sunday; set by civil_from_unix, ignored otherwise */
} civil_time_t;

/** Binary 0..99 to packed BCD. */
uint8_t civil_bin_to_bcd(uint8_t bin);

/** Packed BCD to binary, or -1 if a digit is above 9. */
int civil_bcd_to_bin(uint8_t bcd);

bool civil_is_leap_year(int year);

/** Days in `month` (1..12) of `year`, 0 for an invalid month. */
int civil_days_in_month(int year, int month);

/** True if all fields except weekday are in range (years 0..9999). */
bool civil_is_valid(const civil_time_t *t);

/** Unix seconds of a valid broken-down UTC time. */
int64_t civil_to_unix(const civil_time_t *t);

/** Broken-down UTC time (with weekday) of Unix seconds `unix_sec`. */
void civil_from_unix(int64_t unix_sec, civil_time_t *out);

#ifdef __cplusplus
}
#endif
