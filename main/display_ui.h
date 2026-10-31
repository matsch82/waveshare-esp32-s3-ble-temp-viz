#pragma once

#include "govee_decode.h"
#include "history.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_UI_LAYOUT_TREND = 0,   /* temperature, min/max and 24 h curve */
    DISPLAY_UI_LAYOUT_FOCUS,       /* one big temperature */
    DISPLAY_UI_LAYOUT_COUNT
} display_ui_layout_t;

/**
 * UI / LVGL layer. Initializes LVGL, creates both 200x200 screen layouts,
 * and owns the 1bpp framebuffer used by the e-paper driver.
 */
bool display_ui_create_layout(void);

/**
 * Render a sensor reading to the framebuffer and refresh the e-paper panel.
 *
 * @param reading    sensor reading to display
 * @param history    last 24 h of temperatures, see history_get()
 * @param prev_fb    previous framebuffer image (used as baseline for partial refresh)
 * @param partial    true = partial refresh, false = full refresh
 * @return false if a partial refresh was skipped because nothing changed
 */
bool display_ui_render(const reading_t *reading, const int16_t history[HISTORY_SLOTS],
                       const uint8_t *prev_fb, bool partial);

/**
 * Return the 1bpp framebuffer so the caller can keep it as the base image
 * for the next partial refresh.
 */
const uint8_t *display_ui_get_fb(void);

/* Original orientation: 90 degrees counter-clockwise. */
#define DISPLAY_UI_ROTATION_DEFAULT 3

/**
 * Set the UI rotation in clockwise quarter turns (0..3). Takes effect on the
 * next display_ui_render(), which should then use a full refresh.
 */
void display_ui_set_rotation(uint8_t quarter_turns);

uint8_t display_ui_get_rotation(void);

/**
 * Show one of the layouts. Takes effect on the next display_ui_render(),
 * which should then use a full refresh.
 */
void display_ui_set_layout(display_ui_layout_t layout);

display_ui_layout_t display_ui_get_layout(void);

#ifdef __cplusplus
}
#endif
