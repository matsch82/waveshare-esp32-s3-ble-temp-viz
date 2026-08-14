#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "govee_decode.h"

static int failures = 0;

static void hextobytes(const char *hex, uint8_t *out, size_t *len)
{
    *len = 0;
    for (size_t i = 0; hex[i] && hex[i+1]; i += 2) {
        unsigned int v;
        sscanf(hex + i, "%2x", &v);
        out[(*len)++] = (uint8_t)v;
    }
}

static void check(const char *label, uint16_t mfr, const char *hex_payload,
                  float want_temp, float want_hum, int want_batt)
{
    uint8_t data[64];
    size_t len;
    hextobytes(hex_payload, data, &len);
    reading_t r = {0};
    bool ok = govee_decode(mfr, data, len, &r);
    if (!ok) {
        printf("FAIL %s: decode returned false\n", label);
        failures++;
        return;
    }
    if (r.temp_c != want_temp || r.humidity != want_hum || r.battery != want_batt) {
        printf("FAIL %s: got %.2f C / %.1f %% / %d batt, want %.2f / %.1f / %d\n",
               label, r.temp_c, r.humidity, r.battery, want_temp, want_hum, want_batt);
        failures++;
    } else {
        printf("PASS %s: %.2f C / %.1f %% / %d batt (codec %s)\n",
               label, r.temp_c, r.humidity, r.battery, r.codec);
    }
}

int main(void)
{
    /* From test.csv: company 0x0001, payload starts after 0101. */
    check("test.csv row1", 0x0001,
          "0101044b573c4c000215494e54454c4c495f524f434b535f48575075f2ff0c",
          28.14f, 43.1f, 60);
    check("test.csv row2", 0x0001,
          "0101044f403c4c000215494e54454c4c495f524f434b535f48575075f2ff0c",
          28.24f, 43.2f, 60);
    check("test.csv row3", 0x0001,
          "0101044f403c4c000215494e54454c4c495f524f434b535f48575075f2ff0c",
          28.24f, 43.2f, 60);

    /* Negative temperature example (packed): 0x8053b7 = -2.1431 C, 43.1% */
    check("negative temp", 0x0001, "01018053b7", -2.14f, 43.1f, -1);

    /* H5179 LE16 example: temp=5.39C (0x021B), hum=31.85% (0x0C71) -> rounded 31.9, batt=84 */
    check("h5179 le16", 0x8801, "88ec001b02710c54", 5.39f, 31.9f, 84);

    return failures == 0 ? 0 : 1;
}
