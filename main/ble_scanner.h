#pragma once

#include "govee_decode.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start NimBLE and begin a one-shot BLE scan. The scan runs until the first
 * valid sensor reading is received or until ble_scanner_get_one_reading() times
 * out.
 */
bool ble_scanner_start_once(void);

/**
 * Wait for a single sensor reading.
 *
 * @param out        pointer to a reading_t to fill on success
 * @param timeout_ms maximum time to wait for an advertisement
 * @return true if a reading was received, false on timeout
 *
 * This function stops the BLE scanner before returning in both cases.
 */
bool ble_scanner_get_one_reading(reading_t *out, uint32_t timeout_ms);

/**
 * Stop the BLE scanner and release the NimBLE stack.
 */
void ble_scanner_stop(void);

/** Total BLE advertisements received since boot. */
uint32_t ble_scanner_adv_count(void);

#ifdef __cplusplus
}
#endif
