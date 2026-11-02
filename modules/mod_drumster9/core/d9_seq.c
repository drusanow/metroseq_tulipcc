// d9_seq.c - Drumster9's pattern player (see d9_seq.h).
#include "d9_seq.h"

#include <string.h>

void d9_seq_init(d9_seq_t *q) {
    memset(q, 0, sizeof *q);
    q->cursor = -1;
}

int d9_seq_run(d9_seq_t *q, d9_state_t *st, const tclock_tl_t *tl, int64_t pos_end,
               volatile int8_t *bank_next, uint32_t *switched, d9_hit_t *out, int max) {
    if (!tl->running) {
        q->active = false;
        q->epoch = tl->epoch;
        return 0;
    }
    if (!q->active || tl->epoch != q->epoch) {
        // from the first step at or after where the clock (re)started
        int64_t p0 = tl->ref_pos <= 0 ? 0 : ((tl->ref_pos + D9_STEP_UNITS - 1) / D9_STEP_UNITS) * D9_STEP_UNITS;
        q->cursor = p0 - 1;
        q->epoch = tl->epoch;
        q->active = true;
    }
    int n = 0;
    // the first step boundary after the cursor
    int64_t s = (q->cursor / D9_STEP_UNITS + 1) * D9_STEP_UNITS;
    if (q->cursor < 0) s = 0;
    for (; s <= pos_end && n < max; s += D9_STEP_UNITS) {
        int k = (int)((s / D9_STEP_UNITS) % D9_STEPS);
        if (k == 0 && bank_next && *bank_next >= 0) {
            d9_slot_switch(st, *bank_next);
            *bank_next = -1;
            if (switched) (*switched)++;
        }
        for (int l = 0; l < D9_LANES && n < max; l++) {
            if (!(st->mask[l] & (1u << k)) || !d9_audible(st, l)) continue;
            if (d9_lane_preset(st, l) == D9_NO_SAMPLE) continue;
            out[n++] = (d9_hit_t){s, (uint8_t)l, (uint8_t)k};
        }
        q->cursor = s;
    }
    if (pos_end > q->cursor && n < max) q->cursor = pos_end;
    return n;
}

int d9_seq_step_at(const d9_seq_t *q, int64_t pos) {
    if (!q->active || pos < 0) return -1;
    return (int)((pos / D9_STEP_UNITS) % D9_STEPS);
}
