#include "govee_decode.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MFR_GOVEE       0xEC88
#define MFR_GOVEE_ALT   0x0001
#define MFR_GOVEE_H5179 0x8801

static bool plausible(float temp_c, float hum, int batt)
{
    if (temp_c < -40.0f || temp_c > 80.0f) return false;
    if (hum < 0.0f || hum > 100.0f) return false;
    if (batt >= 0 && (batt < 0 || batt > 100)) return false;
    return true;
}

static bool decode_packed(const uint8_t *data, size_t len, size_t offset, reading_t *out)
{
    if (len < offset + 3) return false;

    uint32_t raw = ((uint32_t)data[offset] << 16) |
                   ((uint32_t)data[offset + 1] << 8) |
                   ((uint32_t)data[offset + 2]);
    bool negative = (raw & 0x800000) != 0;
    uint32_t value = raw & 0x7FFFFF;
    float temp_c = value / 10000.0f;
    float hum = (value % 1000) / 10.0f;
    if (negative) temp_c = -temp_c;
    int batt = (len > offset + 3) ? (int)data[offset + 3] : -1;

    if (!plausible(temp_c, hum, batt)) return false;

    out->temp_c = roundf(temp_c * 100.0f) / 100.0f;
    out->humidity = roundf(hum * 10.0f) / 10.0f;
    out->battery = batt;
    snprintf(out->codec, sizeof(out->codec), "packed@%zu", offset);
    return true;
}

static bool decode_le16(const uint8_t *data, size_t len, size_t offset, reading_t *out)
{
    if (len < offset + 4) return false;

    int16_t temp_raw = (int16_t)(data[offset] | (data[offset + 1] << 8));
    uint16_t hum_raw = (uint16_t)(data[offset + 2] | (data[offset + 3] << 8));
    float temp_c = temp_raw / 100.0f;
    float hum = hum_raw / 100.0f;
    int batt = (len > offset + 4) ? (int)data[offset + 4] : -1;

    if (!plausible(temp_c, hum, batt)) return false;

    out->temp_c = roundf(temp_c * 100.0f) / 100.0f;
    out->humidity = roundf(hum * 10.0f) / 10.0f;
    out->battery = batt;
    snprintf(out->codec, sizeof(out->codec), "le16@%zu", offset);
    return true;
}

bool govee_decode(uint16_t mfr, const uint8_t *data, size_t len, reading_t *out)
{
    if (!out || !data || len == 0) return false;

    reading_t candidate = {0};
    bool ok = false;

    switch (mfr) {
    case MFR_GOVEE_ALT:
        ok = decode_packed(data, len, 2, &candidate);
        break;
    case MFR_GOVEE_H5179:
        ok = decode_le16(data, len, 3, &candidate);
        break;
    case MFR_GOVEE:
        ok = decode_packed(data, len, 1, &candidate);
        break;
    default:
        return false;
    }

    if (!ok) return false;

    *out = candidate;
    out->rssi = 0;
    out->seq = 0;

    // hex dump of the raw mfg payload for debugging
    size_t hex_len = len * 2;
    if (hex_len > sizeof(out->mfr_hex) - 1) hex_len = sizeof(out->mfr_hex) - 1;
    for (size_t i = 0; i < hex_len / 2; i++) {
        snprintf(out->mfr_hex + i * 2, 3, "%02x", data[i]);
    }
    out->mfr_hex[hex_len] = '\0';

    return true;
}
