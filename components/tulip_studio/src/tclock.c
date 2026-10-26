// tclock.c - the Studio master clock (see tclock.h).
#include "studio/tclock.h"

#include <string.h>

#include "studio/midi_parse.h"

#define U_PER_CLOCK (TC_U16 / TC_CLOCKS_PER_16TH)   // 280

static int64_t floordiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

int64_t tclock_tl_pos(const tclock_tl_t *tl, int64_t t) {
    int64_t p = tl->ref_pos + floordiv((t - tl->ref_t) * TC_U16, tl->p16_us);
    return p > tl->max_pos ? tl->max_pos : p;
}

int64_t tclock_tl_time(const tclock_tl_t *tl, int64_t pos) {
    return tl->ref_t + floordiv((pos - tl->ref_pos) * tl->p16_us, TC_U16);
}

// --- small helpers -------------------------------------------------------------

static void send(tclock_t *c, const uint8_t *b, int n) {
    if (c->out) c->out(c->out_ctx, b, n);
}

static void send1(tclock_t *c, uint8_t b) { send(c, &b, 1); }

static void mmc_send(tclock_t *c, uint8_t cmd) {
    const uint8_t m[6] = {0xF0, 0x7F, 0x7F, 0x06, cmd, 0xF7};
    send(c, m, sizeof m);
}

static void mmc_locate_zero(tclock_t *c) {
    const uint8_t m[13] = {0xF0, 0x7F, 0x7F, 0x06, 0x44, 0x06, 0x01, 0, 0, 0, 0, 0, 0xF7};
    send(c, m, sizeof m);
}

int64_t tclock_p16_us(const tclock_t *c) {
    if (c->ext && c->n_iv) return (c->sum_iv * TC_CLOCKS_PER_16TH) / c->n_iv;
    return tclock_p16_from_bpm(c->bpm_x100);
}

int tclock_ext_bpm_x10(const tclock_t *c) {
    if (!c->n_iv) return 0;
    return (int)((600000000LL * c->n_iv) / (c->sum_iv * TC_PPQN));
}

static int64_t pulse_time(const tclock_t *c, uint32_t n) {
    return c->anchor_t + ((int64_t)(n - c->anchor_n) * 1500000000LL) / c->bpm_x100;
}

// MIDI clock k (24 PPQN, counted from the downbeat) on the internal grid
static int64_t out_time(const tclock_t *c, uint32_t k) {
    int64_t rel = (int64_t)k - (int64_t)c->anchor_n * TC_CLOCKS_PER_16TH;
    return c->anchor_t + (rel * 1500000000LL) / ((int64_t)c->bpm_x100 * TC_CLOCKS_PER_16TH);
}

static void publish(tclock_t *c) {
    tclock_tl_t *tl = &c->tl;
    tl->ext = c->ext;
    tl->armed = c->ext && c->armed;
    tl->p16_us = tclock_p16_us(c);
    if (c->ext) {
        int b = tclock_ext_bpm_x10(c);
        tl->bpm_x100 = b ? b * 10 : 0;
    } else {
        tl->bpm_x100 = c->bpm_x100;
    }
    c->tl_ver++;
}

static void tl_new_epoch(tclock_t *c) {
    c->tl.epoch++;
}

static void sinks_tempo(tclock_t *c) {
    int64_t p16 = tclock_p16_us(c);
    for (int i = 0; i < c->nsinks; i++)
        if (c->sinks[i].tempo) c->sinks[i].tempo(c->sinks[i].ctx, p16, c->ext ? 0 : c->bpm_x100);
}

#define FOR_SINKS(call)                                         \
    for (int i_ = 0; i_ < c->nsinks; i_++) {                     \
        const tclock_sink_t *s = &c->sinks[i_];                  \
        call;                                                    \
    }

// --- setup ---------------------------------------------------------------------------

static void tempo_reset(tclock_t *c) {
    c->n_iv = c->idx_iv = c->outliers = 0;
    c->sum_iv = 0;
    c->last_clock_t = 0;
}

void tclock_init(tclock_t *c, int32_t bpm_x100) {
    memset(c, 0, sizeof *c);
    if (bpm_x100 < TC_MIN_BPM_X100) bpm_x100 = TC_MIN_BPM_X100;
    if (bpm_x100 > TC_MAX_BPM_X100) bpm_x100 = TC_MAX_BPM_X100;
    c->bpm_x100 = bpm_x100;
    c->at_start = true;
    c->next_pulse_t = INT64_MAX;
    c->next_out_t = INT64_MAX;
    c->tl.max_pos = INT64_MAX;
    publish(c);
}

void tclock_add_sink(tclock_t *c, const tclock_sink_t *s) {
    if (c->nsinks < TC_MAX_SINKS) c->sinks[c->nsinks++] = *s;
    if (s->tempo) s->tempo(s->ctx, tclock_p16_us(c), c->ext ? 0 : c->bpm_x100);
}

void tclock_set_out(tclock_t *c, tclock_out_fn fn, void *ctx) {
    c->out = fn;
    c->out_ctx = ctx;
}

// --- transport -------------------------------------------------------------------------

static void do_stop(tclock_t *c, int64_t now) {
    bool was = c->running;
    c->running = false;
    c->start_pending = false;
    c->next_pulse_t = INT64_MAX;
    c->next_out_t = INT64_MAX;
    if (was) {
        FOR_SINKS(if (s->stop) s->stop(s->ctx, now));
        if (!c->ext) {
            if (c->clock_out) send1(c, 0xFC);
            if (c->mmc_out) {
                mmc_send(c, MMC_STOP);
                mmc_locate_zero(c);
            }
        }
    }
    c->tl.running = 0;
    c->tl.max_pos = INT64_MAX;
    tl_new_epoch(c);
    publish(c);
}

static void int_play(tclock_t *c, int64_t now) {
    if (c->running) return;
    c->running = true;
    c->start_pending = true;
    c->start_t = now + TC_START_LEAD_US;
    c->pos16 = 0;
    c->anchor_t = c->start_t;
    c->anchor_n = 0;
    c->next_pulse_t = INT64_MAX;
    if (c->clock_out) {
        // Song Position 0, then Start: the first clock after it (at the
        // downbeat) is the slaves' step 1
        const uint8_t spp[3] = {0xF2, 0, 0};
        send(c, spp, 3);
        send1(c, 0xFA);
        c->out_n = 0;
        c->next_out_t = c->start_t;
    }
    if (c->mmc_out) mmc_send(c, MMC_PLAY);
    tl_new_epoch(c);
    c->tl.running = 1;
    c->tl.ref_pos = 0;
    c->tl.ref_t = c->start_t;
    c->tl.max_pos = INT64_MAX;
    publish(c);
}

void tclock_play(tclock_t *c, int64_t now) {
    if (c->ext) {
        if (!c->running) {
            c->armed = true;
            publish(c);
        }
        return;
    }
    int_play(c, now);
}

void tclock_stop(tclock_t *c, int64_t now) {
    c->armed = false;
    do_stop(c, now);
}

void tclock_toggle(tclock_t *c, int64_t now) {
    if (c->running) {
        tclock_stop(c, now);
    } else if (c->ext) {
        c->armed = !c->armed;   // arm: start on the next clock (or cancel)
        publish(c);
    } else {
        int_play(c, now);
    }
}

void tclock_restart(tclock_t *c, int64_t now) {
    if (c->ext) {
        if (c->running) {
            c->armed = true;   // in phase, on the master's next clock
            publish(c);
        }
        return;
    }
    if (!c->running) return;
    do_stop(c, now);
    int_play(c, now);
}

// --- settings ---------------------------------------------------------------------------

void tclock_set_ext(tclock_t *c, bool ext, int64_t now) {
    if (ext == c->ext) return;
    tclock_stop(c, now);
    c->ext = ext;
    c->armed = false;
    c->at_start = true;
    c->clocks = 0;
    c->located = 0;
    tempo_reset(c);
    sinks_tempo(c);
    publish(c);
}

void tclock_set_clock_out(tclock_t *c, bool on, int64_t now) {
    (void)now;
    if (on == c->clock_out) return;
    if (!on && c->running && !c->ext) send1(c, 0xFC);   // tell the slaves
    c->clock_out = on;
    if (on && c->running && !c->ext && !c->start_pending) {
        // join in on the next 1/16 (slaves start on a Continue)
        const uint8_t spp[3] = {0xF2, (uint8_t)((c->pos16 + 1) & 0x7F),
                                (uint8_t)(((c->pos16 + 1) >> 7) & 0x7F)};
        send(c, spp, 3);
        send1(c, 0xFB);
        c->out_n = (c->pos16 + 1) * TC_CLOCKS_PER_16TH;
        c->next_out_t = out_time(c, c->out_n);
    } else if (!on) {
        c->next_out_t = INT64_MAX;
    } else if (c->running && c->start_pending) {
        const uint8_t spp[3] = {0xF2, 0, 0};
        send(c, spp, 3);
        send1(c, 0xFA);
        c->out_n = 0;
        c->next_out_t = c->start_t;
    }
}

void tclock_set_bpm(tclock_t *c, int32_t bpm_x100, int64_t now) {
    if (bpm_x100 < TC_MIN_BPM_X100) bpm_x100 = TC_MIN_BPM_X100;
    if (bpm_x100 > TC_MAX_BPM_X100) bpm_x100 = TC_MAX_BPM_X100;
    if (bpm_x100 == c->bpm_x100) return;
    int32_t old = c->bpm_x100;
    c->bpm_x100 = bpm_x100;
    if (!c->ext && c->running && !c->start_pending) {
        // Keep the song position continuous: wherever we are between two
        // pulses now, we are at the same fraction of the new 1/16.
        int64_t since = now - c->pulse_t;
        if (since < 0) since = 0;
        int64_t frac_new = (since * old) / bpm_x100;   // since * new_p16 / old_p16
        c->anchor_n = c->pos16;
        c->anchor_t = now - frac_new;
        c->next_pulse_t = pulse_time(c, c->pos16 + 1);
        if (c->clock_out) {
            // the next clock out keeps its place in the 1/16 as well
            c->next_out_t = out_time(c, c->out_n);
            if (c->next_out_t < now) c->next_out_t = now;
        }
        c->tl.ref_pos = (int64_t)c->pos16 * TC_U16;
        c->tl.ref_t = c->anchor_t;
    }
    if (!c->ext) sinks_tempo(c);
    publish(c);
}

// --- internal clock -------------------------------------------------------------------------

static void fire_pulse(tclock_t *c, int64_t t) {
    c->pos16++;
    c->pulse_t = t;
    FOR_SINKS(if (s->pulse) s->pulse(s->ctx, t, c->pos16));
    c->tl.ref_pos = (int64_t)c->pos16 * TC_U16;
    c->tl.ref_t = t;
    publish(c);
}

void tclock_advance(tclock_t *c, int64_t now) {
    if (c->ext || !c->running) return;
    int64_t horizon = now + TC_LOOKAHEAD_US;
    if (c->start_pending) {
        if (c->start_t > horizon) return;
        c->start_pending = false;
        c->anchor_t = c->start_t;
        c->anchor_n = 0;
        c->pos16 = 0;
        c->pulse_t = c->start_t;
        if (c->clock_out) send1(c, 0xF8);   // the downbeat clock
        c->out_n = 1;
        c->next_out_t = c->clock_out ? out_time(c, 1) : INT64_MAX;
        sinks_tempo(c);
        FOR_SINKS(if (s->start) s->start(s->ctx, c->start_t));
        c->tl.ref_pos = 0;
        c->tl.ref_t = c->start_t;
        publish(c);
        c->next_pulse_t = pulse_time(c, 1);
    }
    int64_t p16 = tclock_p16_from_bpm(c->bpm_x100);
    if (now - c->next_pulse_t > TC_MAX_CATCHUP * p16) {
        // A real stall (should not happen): resume in time from now
        // instead of firing the backlog as a burst.
        c->anchor_n = c->pos16 + 1;
        c->anchor_t = now;
        c->next_pulse_t = now;
        c->resyncs++;
        c->out_n = c->anchor_n * TC_CLOCKS_PER_16TH;
        if (c->clock_out) c->next_out_t = now;
    }
    for (;;) {
        bool pulse_due = c->next_pulse_t <= horizon;
        bool out_due = c->clock_out && c->next_out_t <= horizon;
        if (!pulse_due && !out_due) break;
        if (out_due && (!pulse_due || c->next_out_t <= c->next_pulse_t)) {
            send1(c, 0xF8);
            c->out_n++;
            c->next_out_t = out_time(c, c->out_n);
            if (c->next_out_t < now - p16) {
                // clocks fell behind: skip to the next one ahead of now
                int64_t k = ((now - c->anchor_t) * TC_CLOCKS_PER_16TH * c->bpm_x100) / 1500000000LL;
                c->out_n = (uint32_t)(c->anchor_n * TC_CLOCKS_PER_16TH + k + 1);
                c->next_out_t = out_time(c, c->out_n);
            }
            continue;
        }
        fire_pulse(c, c->next_pulse_t);
        c->next_pulse_t = pulse_time(c, c->pos16 + 1);
    }
}

int64_t tclock_next_due(const tclock_t *c) {
    if (c->ext || !c->running) return INT64_MAX;
    if (c->start_pending) return c->start_t;
    int64_t t = c->next_pulse_t;
    if (c->clock_out && c->next_out_t < t) t = c->next_out_t;
    return t;
}

// --- MIDI clock follower ---------------------------------------------------------------------

static void tempo_add(tclock_t *c, int32_t iv) {
    if (c->n_iv == TC_TEMPO_WINDOW)
        c->sum_iv -= c->iv[c->idx_iv];
    else
        c->n_iv++;
    c->iv[c->idx_iv] = iv;
    c->sum_iv += iv;
    c->idx_iv = (c->idx_iv + 1) % TC_TEMPO_WINDOW;
}

static void tempo_clock(tclock_t *c, int64_t t) {
    if (c->last_clock_t) {
        int64_t iv = t - c->last_clock_t;
        // 2 ms .. 250 ms per clock = 1250 .. 10 BPM
        if (iv >= 2000 && iv <= 250000) {
            int64_t avg = c->n_iv ? c->sum_iv / c->n_iv : iv;
            if (c->n_iv >= 4 && (iv > 2 * avg || 2 * iv < avg)) {
                // a lone hiccup is ignored; three in a row is a new tempo
                if (++c->outliers >= 3) {
                    tempo_reset(c);
                    tempo_add(c, (int32_t)iv);
                }
            } else {
                c->outliers = 0;
                tempo_add(c, (int32_t)iv);
            }
        }
    }
    c->last_clock_t = t;
}

bool tclock_ext_alive(tclock_t *c, int64_t now) {
    if (c->last_clock_t && now - c->last_clock_t > TC_CLOCK_TIMEOUT_US) {
        tempo_reset(c);
        publish(c);
    }
    return c->last_clock_t != 0;
}

static void ext_locate(tclock_t *c, int64_t t, uint32_t pos16) {
    if (c->running) return;   // the spec only allows SPP while stopped
    if (pos16 > 16383) pos16 = 16383;
    if (pos16 == 0) {
        c->at_start = true;
        c->located = 0;
        c->clocks = 0;
    } else {
        // the next clock after CONTINUE plays 1/16 number pos16
        FOR_SINKS(if (s->locate) s->locate(s->ctx, t, pos16));
        c->located = pos16;
        c->clocks = pos16 * TC_CLOCKS_PER_16TH - 1;
        c->at_start = false;
    }
    tl_new_epoch(c);
    publish(c);
}

static void ext_continue(tclock_t *c, int64_t t) {
    if (c->running) return;
    if (c->at_start) {
        c->armed = true;   // continuing from the top is a start
        publish(c);
        return;
    }
    c->running = true;
    c->armed = false;
    FOR_SINKS(if (s->cont) s->cont(s->ctx, t));
    // nothing plays until the master's next clock says where we are
    tl_new_epoch(c);
    c->tl.running = 1;
    c->tl.ref_pos = (int64_t)c->clocks * U_PER_CLOCK;
    c->tl.ref_t = t;
    c->tl.max_pos = c->tl.ref_pos + U_PER_CLOCK - 1;
    publish(c);
}

static void ext_clock(tclock_t *c, int64_t t) {
    tempo_clock(c, t);
    sinks_tempo(c);
    if (c->armed) {
        // the first clock after START is the downbeat
        c->armed = false;
        c->at_start = false;
        c->running = true;
        c->clocks = 0;
        c->pos16 = 0;
        c->pulse_t = t;
        FOR_SINKS(if (s->start) s->start(s->ctx, t));
        tl_new_epoch(c);
        c->tl.running = 1;
        c->tl.ref_pos = 0;
        c->tl.ref_t = t;
        c->tl.max_pos = 2 * TC_U16;
        publish(c);
        return;
    }
    if (!c->running) {
        publish(c);   // (tempo display)
        return;
    }
    c->clocks++;
    c->tl.ref_pos = (int64_t)c->clocks * U_PER_CLOCK;
    c->tl.ref_t = t;
    c->tl.max_pos = c->tl.ref_pos + 2 * TC_U16;
    if (c->clocks % TC_CLOCKS_PER_16TH == 0) {
        c->pos16 = c->clocks / TC_CLOCKS_PER_16TH;
        c->pulse_t = t;
        FOR_SINKS(if (s->pulse) s->pulse(s->ctx, t, c->pos16));
    }
    publish(c);
}

void tclock_midi(tclock_t *c, int type, uint16_t arg, const uint8_t *mmc, int64_t t) {
    if (!c->ext) return;   // internal clock: MIDI IN transport is ignored
    switch (type) {
    case MP_CLOCK:
        ext_clock(c, t);
        break;
    case MP_START:
        if (c->running) do_stop(c, t);
        c->armed = true;
        c->clocks = 0;
        c->located = 0;
        publish(c);
        break;
    case MP_CONTINUE:
        ext_continue(c, t);
        break;
    case MP_STOP:
        c->armed = false;
        if (c->running) do_stop(c, t);
        publish(c);
        break;
    case MP_SPP:
        ext_locate(c, t, arg);
        break;
    case MP_MMC: {
        if (!mmc) break;
        switch (mmc[0]) {
        case MMC_PLAY:
        case MMC_DEFERRED_PLAY:
            ext_continue(c, t);
            break;
        case MMC_STOP:
        case MMC_PAUSE:
            c->armed = false;
            if (c->running) do_stop(c, t);
            publish(c);
            break;
        case MMC_REWIND:
            ext_locate(c, t, 0);
            break;
        case MMC_LOCATE: {
            // time code -> 1/16s at the current tempo (30 fps frames)
            int64_t ms = ((int64_t)mmc[1] * 3600 + mmc[2] * 60 + mmc[3]) * 1000 +
                         (int64_t)mmc[4] * 1000 / 30;
            int64_t p16 = tclock_p16_us(c);
            ext_locate(c, t, (uint32_t)((ms * 1000 + p16 / 2) / p16));
            break;
        }
        }
        break;
    }
    }
}
