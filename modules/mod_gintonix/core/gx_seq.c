// gx_seq.c - GinTonix pattern player (see gx_seq.h).
#include "gx_seq.h"

#include <string.h>

void gx_seq_init(gx_seq_t *q) {
    memset(q, 0, sizeof *q);
    q->cursor = -1;
}

static int64_t swing_units(const gx_state_t *st) {
    int64_t L = gx_step_units(st->rate);
    return (int64_t)(st->swing * 0.5f * (float)L + 0.5f);
}

// Start of step k of a pass (k may be n: the next pass's first step).
// Every other step is late by the swing; with an odd length the pattern
// alternates, so the parity runs on across passes.
static int64_t step_start(const gx_state_t *st, int pi, uint32_t pass_no, int k) {
    int n = st->pats[pi].len;
    int64_t L = gx_step_units(st->rate);
    int64_t par = ((int64_t)pass_no * n + k) & 1;
    return k * L + (par ? swing_units(st) : 0);
}

static int64_t pass_len(const gx_state_t *st, int pi) {
    return (int64_t)st->pats[pi].len * gx_step_units(st->rate);
}

static int take_next(gx_live_t *live) {
    int8_t nx = live->next;
    if (nx < 0) return -1;
    // the UI may change it at the same moment: only take what we saw
    if (__atomic_compare_exchange_n(&live->next, &nx, (int8_t)-1, false, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST))
        return nx;
    return -1;
}

static void restart(gx_seq_t *q, const gx_state_t *st, gx_live_t *live, const tclock_tl_t *tl) {
    int64_t p0 = tl->ref_pos <= 0 ? 0 : ((tl->ref_pos + TC_U16 - 1) / TC_U16) * TC_U16;
    int nx = take_next(live);
    q->pat = nx >= 0 ? nx : st->cur;
    int64_t P = pass_len(st, q->pat);
    // passes are anchored to song position 0
    q->pass_no = (uint32_t)(p0 / P);
    q->pass_start = (int64_t)q->pass_no * P;
    q->cursor = p0 - 1;
    q->epoch = tl->epoch;
    q->active = true;
}

// Hits of one pass with position in (lo, hi].
static int pass_hits(const gx_seq_t *q, const gx_state_t *st, const gx_live_t *live, int64_t lo,
                     int64_t hi, gx_hit_t *out, int n_out, int max) {
    const gx_pat_t *p = &st->pats[q->pat];
    uint8_t off = live->mute | live->hold_mute;
    int fr = GX_FILL_RATES[st->fill_rate % GX_NFILLS];
    for (int k = 0; k < p->len && n_out < max; k++) {
        int64_t s = q->pass_start + step_start(st, q->pat, q->pass_no, k);
        if (s > hi) break;
        int64_t e = q->pass_start + step_start(st, q->pat, q->pass_no, k + 1);
        if (e - 1 <= lo) continue;
        uint16_t b = (uint16_t)(1u << k);
        for (int c = 0; c < GX_NCH && n_out < max; c++) {
            if (!(p->trig[c] & b) || (off & (1u << c))) continue;
            bool acc = p->acc[c] & b;
            float v0 = acc ? 1.0f : 0.5f;
            if (p->fill[c] & b) {
                int64_t ls = e - s, last = -1;
                for (int h = 0; h < fr && n_out < max; h++) {
                    int64_t tt = s + (h * ls + fr / 2) / fr;
                    if (tt <= last) continue;
                    last = tt;
                    if (tt <= lo || tt > hi) continue;
                    float vel = acc ? v0 * (1.0f - 0.5f * h / fr) : v0 * (1.0f - (float)h / fr);
                    out[n_out++] = (gx_hit_t){tt, (uint8_t)c, (uint8_t)k, vel};
                }
            } else if (s > lo && s <= hi) {
                out[n_out++] = (gx_hit_t){s, (uint8_t)c, (uint8_t)k, v0};
            }
        }
    }
    return n_out;
}

int gx_seq_run(gx_seq_t *q, const gx_state_t *st, gx_live_t *live, const tclock_tl_t *tl,
               int64_t pos_end, gx_hit_t *out, int max) {
    if (!tl->running) {
        q->active = false;
        q->epoch = tl->epoch;
        q->pat = st->cur;
        return 0;
    }
    if (!q->active || tl->epoch != q->epoch) restart(q, st, live, tl);
    int n = 0;
    for (int guard = 0; guard < 64 && n < max; guard++) {
        int64_t P = pass_len(st, q->pat);
        int len = st->pats[q->pat].len;
        // the pass's last step (and its fill) runs until the next pass's
        // first step, which may itself be swung late
        int64_t pend = q->pass_start + step_start(st, q->pat, q->pass_no, len);
        if (q->cursor >= pend) {
            // the pattern got shorter (or the rate faster): carry on in the
            // same bar grid, at the right place in the new pass
            int64_t k = (q->cursor - q->pass_start) / P;
            q->pass_start += k * P;
            q->pass_no += (uint32_t)k;
            pend = q->pass_start + step_start(st, q->pat, q->pass_no, len);
            if (q->cursor >= pend) {
                q->pass_start += P;
                q->pass_no++;
                pend = q->pass_start + step_start(st, q->pat, q->pass_no, len);
            }
        }
        int64_t wend = pos_end < pend - 1 ? pos_end : pend - 1;
        if (wend > q->cursor) n = pass_hits(q, st, live, q->cursor, wend, out, n, max);
        if (pos_end < pend) {
            if (pos_end > q->cursor) q->cursor = pos_end;
            break;
        }
        // end of the pass: queued pattern, else the chain, else again
        q->cursor = pend - 1;
        q->pass_start += P;
        q->pass_no++;
        int nx = take_next(live);
        if (nx < 0) nx = gx_chain_next(st->pats, q->pat);
        if (nx >= 0 && nx != q->pat) {
            q->pat = nx;
            q->pass_no = 0;   // a new pattern starts unswung on step 1
        }
    }
    // choke: of the choking channels hitting at one instant, only the
    // lowest plays (the Python's rule)
    if (live->choke) {
        for (int i = 0; i < n; i++) {
            if (!(live->choke & (1u << out[i].ch))) continue;
            for (int j = 0; j < n; j++) {
                if (j != i && out[j].pos == out[i].pos && (live->choke & (1u << out[j].ch)) &&
                    out[j].ch < out[i].ch) {
                    out[i].vel = -1.0f;   // drop
                    break;
                }
            }
        }
        int w = 0;
        for (int i = 0; i < n; i++)
            if (out[i].vel >= 0.0f) out[w++] = out[i];
        n = w;
    }
    // time order (insertion sort: a handful of hits)
    for (int i = 1; i < n; i++) {
        gx_hit_t h = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].pos > h.pos) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = h;
    }
    return n;
}

int gx_seq_step_at(const gx_seq_t *q, const gx_state_t *st, int64_t pos) {
    if (!q->active) return -1;
    int n = st->pats[q->pat].len;
    int64_t rel = pos - q->pass_start;
    if (rel < 0) return n - 1;   // the end of the previous pass
    for (int k = n - 1; k >= 0; k--)
        if (rel >= step_start(st, q->pat, q->pass_no, k)) return k;
    return 0;
}
