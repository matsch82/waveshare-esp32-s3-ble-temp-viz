#include "history.h"

/* Ring buffer indexed by absolute slot number (now_sec / HISTORY_SLOT_SEC). */
static int16_t s_vals[HISTORY_SLOTS];
static int64_t s_head = -1;    /* slot number of the newest entry, -1 = empty */

static int ring_index(int64_t slot)
{
    return (int)(((slot % HISTORY_SLOTS) + HISTORY_SLOTS) % HISTORY_SLOTS);
}

/* Move the head forward to the slot of `now_sec`, clearing skipped slots. */
static void advance(int64_t now_sec)
{
    int64_t slot = now_sec / HISTORY_SLOT_SEC;
    if (s_head < 0 || slot - s_head >= HISTORY_SLOTS) {
        history_reset();
        s_head = slot;
        return;
    }
    while (s_head < slot) {
        s_head++;
        s_vals[ring_index(s_head)] = HISTORY_NONE;
    }
}

void history_reset(void)
{
    for (int i = 0; i < HISTORY_SLOTS; i++) {
        s_vals[i] = HISTORY_NONE;
    }
    s_head = -1;
}

void history_add(int16_t temp_deci, int64_t now_sec)
{
    advance(now_sec);
    s_vals[ring_index(s_head)] = temp_deci;
}

void history_get(int16_t out[HISTORY_SLOTS], int64_t now_sec)
{
    advance(now_sec);
    for (int i = 0; i < HISTORY_SLOTS; i++) {
        out[i] = s_vals[ring_index(s_head - (HISTORY_SLOTS - 1) + i)];
    }
}

bool history_min_max(const int16_t vals[HISTORY_SLOTS], int16_t *min, int16_t *max)
{
    bool any = false;
    for (int i = 0; i < HISTORY_SLOTS; i++) {
        if (vals[i] == HISTORY_NONE) continue;
        if (!any || vals[i] < *min) *min = vals[i];
        if (!any || vals[i] > *max) *max = vals[i];
        any = true;
    }
    return any;
}
