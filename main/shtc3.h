#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * On-board SHTC3 temperature/humidity sensor (I2C 0x70), used as the indoor
 * reading. It idles at ~45 uA, so it is kept in sleep mode between
 * measurements. Being on the board, it reads a little warm while the ESP32
 * is awake for long (setup window).
 */

/** Attach to the I2C bus and put the sensor to sleep. */
esp_err_t shtc3_init(i2c_master_bus_handle_t bus);

/**
 * Wake the sensor, measure (normal mode, ~13 ms), check both CRCs and put
 * it back to sleep.
 * @return true if both values are valid
 */
bool shtc3_measure(float *temp_c, float *humidity);

#ifdef __cplusplus
}
#endif
