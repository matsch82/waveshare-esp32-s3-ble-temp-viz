#include "sensor_math.h"

uint8_t shtc3_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

float shtc3_temp_c(uint16_t raw)
{
    return -45.0f + 175.0f * (float)raw / 65536.0f;
}

float shtc3_humidity(uint16_t raw)
{
    return 100.0f * (float)raw / 65536.0f;
}

/* Typical single-cell discharge curve, voltage falling with the level. */
static const struct {
    int mv;
    int pct;
} s_curve[] = {
    { 4200, 100 }, { 4150, 95 }, { 4110, 90 }, { 4080, 85 }, { 4020, 80 },
    { 3980, 75 }, { 3950, 70 }, { 3910, 65 }, { 3870, 60 }, { 3850, 55 },
    { 3840, 50 }, { 3820, 45 }, { 3800, 40 }, { 3790, 35 }, { 3770, 30 },
    { 3750, 25 }, { 3730, 20 }, { 3710, 15 }, { 3690, 10 }, { 3610, 5 },
    { 3270, 0 },
};

int battery_percent_from_mv(int mv)
{
    const int n = sizeof(s_curve) / sizeof(s_curve[0]);
    if (mv >= s_curve[0].mv) return 100;
    if (mv <= s_curve[n - 1].mv) return 0;
    for (int i = 1; i < n; i++) {
        if (mv >= s_curve[i].mv) {
            int dv = s_curve[i - 1].mv - s_curve[i].mv;
            int dp = s_curve[i - 1].pct - s_curve[i].pct;
            return s_curve[i].pct + ((mv - s_curve[i].mv) * dp + dv / 2) / dv;
        }
    }
    return 0;
}
