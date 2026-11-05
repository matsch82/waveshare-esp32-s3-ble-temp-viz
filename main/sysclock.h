#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Wall-clock time for the display and the SD log.
 *
 * The PCF85063A RTC (I2C 0x51 on SDA 47 / SCL 48) keeps UTC and runs from the
 * battery even while the board is switched off. The local time zone is a
 * POSIX TZ string stored in NVS (default CONFIG_APP_DEFAULT_TZ). The ESP32's
 * own system time is set from the RTC on every boot and deep-sleep wake,
 * because the internal RC oscillator drifts.
 */

/* Unix time before 2024-01-01 means the clock was never set. */
#define SYSCLOCK_MIN_VALID_UTC 1704067200LL

/* Longest accepted POSIX TZ string, without the terminating NUL. */
#define SYSCLOCK_TZ_MAX_LEN 48

/**
 * Create the I2C bus, disable the RTC's CLKOUT, load the TZ from NVS
 * (setenv + tzset) and set the system time from the RTC. Call once per boot
 * after nvs_flash_init().
 * @return true if the wall-clock time is valid afterwards
 */
bool sysclock_init(void);

/** The board's I2C bus (also used by the SHTC3), NULL if init failed. */
i2c_master_bus_handle_t sysclock_i2c_bus(void);

/** True if time(NULL) is at or after SYSCLOCK_MIN_VALID_UTC. */
bool sysclock_is_valid(void);

/**
 * Set system time and RTC to `utc_us` (Unix time in microseconds). Rejects
 * times before SYSCLOCK_MIN_VALID_UTC and after 2099 (the RTC's two-digit
 * year). Blocks up to 1 s to write the RTC at a second boundary. If the RTC
 * write fails, the next deep-sleep wake retries it from the system time.
 * Call from the app task.
 */
esp_err_t sysclock_set_utc_us(int64_t utc_us);

/**
 * Validate (1..SYSCLOCK_TZ_MAX_LEN printable ASCII chars), store in NVS and
 * apply a POSIX TZ string such as "CET-1CEST,M3.5.0,M10.5.0/3".
 * Call from the app task.
 */
esp_err_t sysclock_set_tz(const char *posix_tz);

/** Copy the active TZ string into `buf` (always NUL-terminated). Thread-safe. */
void sysclock_get_tz(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
