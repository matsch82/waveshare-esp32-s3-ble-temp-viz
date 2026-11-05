#pragma once

#include "govee_decode.h"
#include "sysclock.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * GATT clock service (setup mode only). Web Bluetooth uses the lowercase
 * string form; NimBLE's BLE_UUID128_INIT takes the bytes reversed.
 *   service   7a6b1000-3c2d-4e5f-9a8b-1c2d3e4f5a6b
 *   time      7a6b1001-3c2d-4e5f-9a8b-1c2d3e4f5a6b  read/write, 8 bytes,
 *             Unix seconds UTC, little endian (uint64)
 *   timezone  7a6b1002-3c2d-4e5f-9a8b-1c2d3e4f5a6b  read/write, 1..48 bytes
 *             POSIX TZ string, ASCII, no NUL
 */

typedef enum {
    /* Clock service + connectable advertising + continuous active scan. */
    BLE_MODE_SETUP,
    /* Active scan only, until the first sensor reading. */
    BLE_MODE_SCAN_ONCE,
} ble_mode_t;

/* A write to the clock service, handed to the app task. */
typedef struct {
    bool has_utc;
    int64_t utc;
    int64_t utc_rx_us;      /* esp_timer_get_time() when the write arrived */
    bool has_tz;
    char tz[SYSCLOCK_TZ_MAX_LEN + 1];
} ble_clock_request_t;

/**
 * Initialize NimBLE and start the given mode. Call from the app task, after
 * nvs_flash_init() and sysclock_init().
 */
bool ble_start(ble_mode_t mode);

/**
 * Wait up to `timeout_ms` for a sensor reading. In setup mode every decoded
 * advertisement is delivered (newest wins); in scan-once mode the first one.
 */
bool ble_wait_reading(reading_t *out, uint32_t timeout_ms);

/**
 * Return pending clock-service writes since the last call (newest values
 * win). Never blocks.
 */
bool ble_take_clock_request(ble_clock_request_t *out);

/** True while a central (the web page) is connected. */
bool ble_is_connected(void);

/** Stop scanning/advertising and deinitialize NimBLE. App task only. */
void ble_stop(void);

#ifdef __cplusplus
}
#endif
