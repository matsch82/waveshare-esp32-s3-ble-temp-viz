#include <math.h>
#include <stdio.h>
#include "sensor_math.h"

static int failures = 0;

#define EXPECT(cond, label) do { \
    if (cond) { printf("PASS %s\n", label); } \
    else { printf("FAIL %s\n", label); failures++; } \
} while (0)

int main(void)
{
    /* Datasheet example: CRC of 0xBEEF is 0x92. */
    const uint8_t beef[2] = { 0xBE, 0xEF };
    EXPECT(shtc3_crc8(beef, 2) == 0x92, "crc8 datasheet example");
    const uint8_t zero[2] = { 0x00, 0x00 };
    EXPECT(shtc3_crc8(zero, 2) == 0x81, "crc8 of 0x0000");

    EXPECT(fabsf(shtc3_temp_c(0) - -45.0f) < 0.001f, "temp raw 0 = -45 C");
    EXPECT(fabsf(shtc3_temp_c(0x6666) - 25.0f) < 0.01f, "temp raw 0x6666 = 25 C");
    EXPECT(fabsf(shtc3_humidity(0x8000) - 50.0f) < 0.001f, "humidity raw 0x8000 = 50 %");

    EXPECT(battery_percent_from_mv(4300) == 100, "battery above full");
    EXPECT(battery_percent_from_mv(4200) == 100, "battery full");
    EXPECT(battery_percent_from_mv(3840) == 50, "battery curve point 3.84 V");
    EXPECT(battery_percent_from_mv(3860) == 58, "battery interpolated 3.86 V");
    EXPECT(battery_percent_from_mv(3270) == 0, "battery empty");
    EXPECT(battery_percent_from_mv(3000) == 0, "battery below empty");
    int prev = 101, monotonic = 1;
    for (int mv = 4300; mv >= 3000; mv -= 5) {
        int p = battery_percent_from_mv(mv);
        if (p > prev || p < 0 || p > 100) monotonic = 0;
        prev = p;
    }
    EXPECT(monotonic, "battery percent falls monotonically with voltage");

    return failures == 0 ? 0 : 1;
}
