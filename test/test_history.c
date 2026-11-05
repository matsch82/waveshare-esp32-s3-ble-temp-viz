#include <stdio.h>
#include "history.h"

static int failures = 0;

#define EXPECT(cond, label) do { \
    if (cond) { printf("PASS %s\n", label); } \
    else { printf("FAIL %s\n", label); failures++; } \
} while (0)

int main(void)
{
    int16_t v[HISTORY_SLOTS];
    int16_t lo, hi;

    history_reset();
    history_get(v, 0);
    EXPECT(!history_min_max(v, &lo, &hi), "empty history has no min/max");

    history_add(215, 10);
    history_add(218, 20);   /* same slot: replaces 21.5 */
    history_get(v, 30);
    EXPECT(v[HISTORY_SLOTS - 1] == 218, "latest value in newest slot");
    EXPECT(v[HISTORY_SLOTS - 2] == HISTORY_NONE, "older slot empty");

    history_add(250, 10 * HISTORY_SLOT_SEC);   /* 10 slots later */
    history_get(v, 10 * HISTORY_SLOT_SEC);
    EXPECT(v[HISTORY_SLOTS - 1] == 250 && v[HISTORY_SLOTS - 11] == 218,
           "values keep their slot distance");
    EXPECT(v[HISTORY_SLOTS - 5] == HISTORY_NONE, "skipped slots stay empty");
    EXPECT(history_min_max(v, &lo, &hi) && lo == 218 && hi == 250, "min/max over values");

    history_add(-35, 11 * HISTORY_SLOT_SEC);
    history_get(v, 11 * HISTORY_SLOT_SEC);
    EXPECT(history_min_max(v, &lo, &hi) && lo == -35 && hi == 250, "negative values");

    /* 24 h later the old values have scrolled out. */
    history_get(v, (11 + HISTORY_SLOTS) * HISTORY_SLOT_SEC);
    EXPECT(!history_min_max(v, &lo, &hi), "values older than 24 h dropped");

    /* A clock running backwards starts a fresh history. */
    history_reset();
    history_add(200, 50 * HISTORY_SLOT_SEC);
    history_add(210, 40 * HISTORY_SLOT_SEC);
    history_get(v, 40 * HISTORY_SLOT_SEC);
    EXPECT(v[HISTORY_SLOTS - 1] == 210 && history_min_max(v, &lo, &hi) && lo == 210 && hi == 210,
           "time going backwards resets history");

    /* Slot 0 entry is still visible exactly HISTORY_SLOTS - 1 slots later. */
    history_reset();
    history_add(100, 0);
    history_get(v, (HISTORY_SLOTS - 1) * HISTORY_SLOT_SEC);
    EXPECT(v[0] == 100, "oldest slot still in window");

    return failures == 0 ? 0 : 1;
}
