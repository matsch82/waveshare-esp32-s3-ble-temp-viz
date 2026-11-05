#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GDEY0154D67 panel, SSD1681 controller (register-compatible with SSD1680). */
#define EPD_WIDTH  200
#define EPD_HEIGHT 200
#define EPD_BUF_SIZE ((EPD_WIDTH * EPD_HEIGHT) / 8)

/**
 * Write the 1bpp framebuffer to controller RAM and refresh the panel.
 *
 * Works the same after power-on, after deep sleep (panel rail was off) and in
 * the always-on setup window: it powers the rail if needed, runs the complete
 * controller init (hardware reset, SW reset 0x12, driver output, data entry
 * mode, RAM window, temperature sensor, LUT), refreshes, and finally puts the
 * controller into its own deep sleep (0x10). The rail stays on.
 *
 * - `partial=false`: full refresh. `prev_fb` is ignored.
 * - `partial=true`: partial refresh. `prev_fb` must point to the image that
 *   is currently on the glass; it is loaded into RAM 0x26 so the
 *   differential waveform only drives changed pixels.
 */
void epd_refresh_fb(const uint8_t *fb, const uint8_t *prev_fb, bool partial);

/**
 * Put the controller into deep sleep (if it is not already) and switch the
 * panel rail off. Call before the ESP32 enters deep sleep.
 */
void epd_power_off(void);

#ifdef __cplusplus
}
#endif
