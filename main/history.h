#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 24 h of temperatures in 10 minute slots, kept in RAM only. */
#define HISTORY_SLOTS     144
#define HISTORY_SLOT_SEC  600
#define HISTORY_NONE      INT16_MIN

/** Clear the history. */
void history_reset(void);

/**
 * Store a temperature (tenths of a degree C) for the slot containing
 * `now_sec` (seconds since boot). A later value in the same slot replaces
 * the earlier one; slots without a value stay HISTORY_NONE.
 */
void history_add(int16_t temp_deci, int64_t now_sec);

/**
 * Copy the last 24 h into `out`, oldest first; out[HISTORY_SLOTS - 1] is the
 * slot containing `now_sec`.
 */
void history_get(int16_t out[HISTORY_SLOTS], int64_t now_sec);

/**
 * Min and max over the non-empty values of `vals`.
 * @return false if every value is HISTORY_NONE
 */
bool history_min_max(const int16_t vals[HISTORY_SLOTS], int16_t *min, int16_t *max);

#ifdef __cplusplus
}
#endif
