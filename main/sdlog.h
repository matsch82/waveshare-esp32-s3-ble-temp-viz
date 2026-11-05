#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "govee_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CSV log on the micro SD card. The slot is wired as SDMMC 1-bit
 * (CLK 39, CMD 41, D0 40, external 10k pull-ups) and has no card-detect pin,
 * so a missing card shows up as a failed mount. One file per month.
 */

#define SDLOG_MOUNT_POINT "/sdcard"
#define SDLOG_HEADER "local_time,utc_epoch,temperature_c,humidity_pct,sensor_battery_pct,rssi_dbm"

/**
 * Path of the month file for `now` in local time, e.g. "/sdcard/2026-11.csv".
 * @return false if `len` is too small
 */
bool sdlog_month_path(char *buf, size_t len, time_t now);

/**
 * One CSV row (no newline) for reading `r` taken at `now`, e.g.
 * "2026-11-05 14:00:12,1793883612,12.26,63.7,62,-86".
 * Unknown battery (-1) is written as an empty field.
 * @return number of characters written, or -1 if `len` is too small
 */
int sdlog_format_row(char *buf, size_t len, const reading_t *r, time_t now);

/**
 * Mount the card, append one row (plus the header if the file is new),
 * flush, close and unmount. Takes a few hundred ms with a card and fails
 * quickly without one.
 * @return true if the row was written
 */
bool sdlog_append(const reading_t *r, time_t now);

#ifdef __cplusplus
}
#endif
