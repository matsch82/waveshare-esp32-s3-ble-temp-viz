#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_WIDTH  200
#define EPD_HEIGHT 200
#define EPD_BUF_SIZE ((EPD_WIDTH * EPD_HEIGHT) / 8)

/**
 * Initialize the GDEY0154D67 (SSD1680-based) controller. After this the screen
 * is in a clean state and ready for drawing; the framebuffer is not owned here.
 */
bool epd_init(void);

/**
 * Fill the controller's current and previous RAM with `white` (0xFF) and do
 * a full refresh to clear the panel physically. Call once after init.
 */
void epd_clear_white_full(void);

/**
 * Write the 1bpp framebuffer to controller RAM and refresh the panel.
 *
 * - `partial=false`: full refresh. `prev_fb` is ignored.
 * - `partial=true`: partial refresh. `prev_fb` must point to the image that
 *   is currently on the screen; it is loaded into the controller's base RAM
 *   (0x26) so the differential update only refreshes changed pixels.
 */
void epd_refresh_fb(const uint8_t *fb, const uint8_t *prev_fb, bool partial);

/**
 * Power off the panel driving voltages after the last refresh.
 */
void epd_power_off(void);

#ifdef __cplusplus
}
#endif
