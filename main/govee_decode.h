#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float temp_c;        // Celsius
    float humidity;      // percent relative humidity
    int   battery;       // percent (0..100) or -1 if unknown
    int   rssi;          // dBm
    int   seq;           // packet counter for UI
    time_t timestamp;    // wall-clock time the advertisement was received
    char  codec[16];     // decoder tag, e.g. "packed@2"
    char  mfr_hex[65];   // raw manufacturer data as hex string (for logging)
} reading_t;

/**
 * Decode Govee H5179-style manufacturer data.
 *
 * Supported formats:
 *   - company 0x0001 (H5101/H5102/H5171/H5177/H5179): 01 01 <packed:3> <batt>
 *   - company 0x8801 (H5179 alternate): 88 ec 00 <t:2le> <h:2le> <batt>
 *
 * @param mfr      company ID (little-endian over the air, already parsed)
 * @param data     payload bytes following the company ID
 * @param len      payload length
 * @param out      filled on success
 * @return true if a plausible reading was decoded
 */
bool govee_decode(uint16_t mfr, const uint8_t *data, size_t len, reading_t *out);

#ifdef __cplusplus
}
#endif
