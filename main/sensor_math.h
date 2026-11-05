#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pure conversions for the on-board sensors, host-tested. */

/** SHTC3 checksum: CRC-8, polynomial 0x31, init 0xFF. */
uint8_t shtc3_crc8(const uint8_t *data, size_t len);

/** SHTC3 raw values to degrees Celsius and percent relative humidity. */
float shtc3_temp_c(uint16_t raw);
float shtc3_humidity(uint16_t raw);

/**
 * Charge level of a single Li-ion/LiPo cell from its voltage, 0..100,
 * interpolated over a typical discharge curve (lightly loaded).
 */
int battery_percent_from_mv(int mv);

#ifdef __cplusplus
}
#endif
