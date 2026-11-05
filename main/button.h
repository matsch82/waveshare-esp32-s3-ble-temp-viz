#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Holding the button at least this long counts as a long press. */
#define BUTTON_LONG_PRESS_MS 1000

typedef struct {
    uint32_t short_presses;
    uint32_t long_presses;
} button_events_t;

/**
 * Start debounced polling of the BOOT button (BOARD_BOOT_BUTTON_GPIO).
 * Presses are counted in the background until button_take_events() is called.
 */
bool button_init(void);

/**
 * Wait up to `timeout_ms` for a press, then return all presses since the last
 * call. A short press is reported on release; a long press as soon as the
 * hold time is reached, and its release is not reported again.
 * `timeout_ms = 0` only polls.
 *
 * @return true if `out` holds at least one press
 */
bool button_take_events(uint32_t timeout_ms, button_events_t *out);

/**
 * After a deep-sleep wake by the BOOT button: wait until the button is
 * released (or `max_ms` passed) and return how long it was held since this
 * call. Works without button_init().
 */
uint32_t button_measure_hold_ms(uint32_t max_ms);

/** True while the BOOT button is pressed (raw level, no debouncing). */
bool button_is_pressed(void);

#ifdef __cplusplus
}
#endif
