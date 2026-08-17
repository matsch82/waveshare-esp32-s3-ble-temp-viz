#pragma once

#include "govee_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * UI / LVGL layer. Initializes LVGL, creates the 200x200 screen layout,
 * and owns the 1bpp framebuffer used by the e-paper driver.
 */
bool display_ui_create_layout(void);

/**
 * Render a sensor reading to the framebuffer and refresh the e-paper panel.
 *
 * @param reading    sensor reading to display
 * @param prev_fb    previous framebuffer image (used as baseline for partial refresh)
 * @param partial    true = partial refresh, false = full refresh
 * @param adv_count  number of BLE advertisements received so far
 */
void display_ui_render(const reading_t *reading, const uint8_t *prev_fb, bool partial, uint32_t adv_count);

/**
 * Return the 1bpp framebuffer so the caller can save it across deep sleep.
 */
const uint8_t *display_ui_get_fb(void);

#ifdef __cplusplus
}
#endif
