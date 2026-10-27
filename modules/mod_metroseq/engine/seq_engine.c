// seq_engine.c - the sequencer engine (see seq_engine.h for the design).
//
// Function-for-function port of the Python SEQUENCER ENGINE and the CV
// gate/merge helpers. Differences from the Python are deliberate and
// marked "FIX:" - each one removes a way the original could glitch.
#include "seq_engine.h"

#include <string.h>

enum {
    EV_GATE_PULSE = 0,  // deferred (swung / ratcheted) gate pulse; arg = len
    EV_HOLD_ON,         // deferred (swung) HOLD gate open
    EV_GATE_LOW,        // gate low for phys, if token still current
    EV_AUDIO_OFF,       // preview note off, if token still current
    EV_GLIDE,           // glide pitch update
};

// Ordering of simultaneous events: lows first, then pitch, then the pulse
// (which may write pitch and open gates), then deferred gate-ons.
enum { PRIO_LOW = 0, PRIO_GLIDE = 1, PRIO_PULSE = 2, PRIO_GATE_ON = 3 };

// --- small utilities -------------------------------------------------------

static uint32_t rng_next(seq_engine_t *e) {
    uint32_t x = e->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    e->rng = x;
    return x;
}

static int rand_range(seq_engine_t *e, int n) {   // 0..n-1
    return (int)(rng_next(e) % (uint32_t)n);
}

static void emit(seq_actions_t *out, int64_t due, uint8_t type, uint8_t o, int32_t v) {
    if (out->n >= SEQ_MAX_ACTIONS) {
        out->dropped++;
        return;
    }
    seq_action_t *a = &out->a[out->n++];
    a->due_us = due;
    a->type = type;
    a->out = o;
    a->value = v;
    a->arg = 0;
}

// An audio note-on: carries the slide time of the stage that set its pitch
// (once - a ratchet's repeats don't slide again).
static void emit_audio_on(seq_engine_t *e, int t, int64_t now, seq_actions_t *out) {
    if (e->pat->tracks[t].mute_audio) return;
    emit(out, now, ACT_AUDIO_ON, t, e->tr[t].cur_cents / 100);
    if (out->n > 0 && out->a[out->n - 1].type == ACT_AUDIO_ON)
        out->a[out->n - 1].arg = e->tr[t].slide_us;
    e->tr[t].slide_us = 0;
}

static int64_t pulse_time(const seq_engine_t *e, uint32_t n) {
    return e->anchor_us + ((int64_t)(n - e->anchor_n) * 15000000LL) / e->bpm;
}

// --- event heap ------------------------------------------------------------

static bool ev_before(const seq_event_t *a, const seq_event_t *b) {
    if (a->t != b->t) return a->t < b->t;
    if (a->prio != b->prio) return a->prio < b->prio;
    return (int32_t)(a->seq - b->seq) < 0;
}

static void ev_push(seq_engine_t *e, seq_event_t ev) {
    if (e->nev >= SEQ_MAX_EVENTS) {
        e->stats.queue_overflows++;
        return;
    }
    ev.seq = e->ev_seq++;
    int i = e->nev++;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (!ev_before(&ev, &e->ev[p])) break;
        e->ev[i] = e->ev[p];
        i = p;
    }
    e->ev[i] = ev;
}

static seq_event_t ev_pop(seq_engine_t *e) {
    seq_event_t top = e->ev[0];
    seq_event_t last = e->ev[--e->nev];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        const seq_event_t *mv = &last;
        if (l < e->nev && ev_before(&e->ev[l], mv)) { m = l; mv = &e->ev[l]; }
        if (r < e->nev && ev_before(&e->ev[r], mv)) { m = r; }
        if (m == i) break;
        e->ev[i] = e->ev[m];
        i = m;
    }
    if (e->nev > 0) e->ev[i] = last;
    return top;
}

static void schedule(seq_engine_t *e, int64_t t, uint8_t kind, uint8_t prio,
                     uint8_t track, uint8_t phys, uint32_t token, int32_t arg) {
    seq_event_t ev = {.t = t, .prio = prio, .kind = kind, .track = track,
                      .phys = phys, .token = token, .arg = arg};
    ev_push(e, ev);
}

// --- modulation bus ----------------------------------------------------------
//
// Python kept app.mod_bus as a dict recomputed when lanes advanced. Here the
// sum is computed on demand from each lane's current stage value, which is
// equivalent (a live slider edit on the playing stage is heard at once, as
// in the Python) and needs no shared derived state.

static int32_t mod_sum(const seq_engine_t *e, int t, int param) {
    int32_t s = 0;
    for (int l = 0; l < NUM_MOD_LANES; l++) {
        const mod_lane_cfg_t *m = &e->pat->mods[l];
        if (m->dest == 0 || m->depth == 0) continue;
        if (dest_track(m->dest) != t || dest_param(m->dest) != param) continue;
        int st = clampi(e->ln[l].cur_stage, 0, NUM_STAGES - 1);
        s += (int32_t)m->values[st] * m->depth;   // +-10000 == +-1.0 full scale
    }
    return s;
}

int seq_mod_offset(const seq_engine_t *e, int t, int param, int32_t scale_num) {
    return rdiv(mod_sum(e, t, param) * scale_num, 10000);
}

static int eff_transpose(const seq_engine_t *e, int t) {
    return e->pat->tracks[t].transpose + seq_mod_offset(e, t, MP_TRANSPOSE, 12);
}

static int eff_octave(const seq_engine_t *e, int t) {
    return e->pat->tracks[t].octave + seq_mod_offset(e, t, MP_OCTAVE, 2);
}

// gate fraction / swing in units of 1/10000
static int32_t eff_gate_e4(const seq_engine_t *e, int t) {
    return clampi(e->pat->tracks[t].gate_pct * 100 + mod_sum(e, t, MP_GATE) / 2, 500, 10000);
}

static int32_t eff_swing_e4(const seq_engine_t *e, int t) {
    return clampi(e->pat->tracks[t].swing * 100 + mod_sum(e, t, MP_SWING) * 2 / 5, 0, 6000);
}

// --- CV gate / merge logic (Python _cv_raise, cv_gate_* etc.) -------------

static bool t2_blocked(const seq_engine_t *e, int t) {
    return e->pat->merge_cv && t == 1 && e->shared_high && e->shared_owner == 0;
}

static void audio_pulse(seq_engine_t *e, int t, int64_t now, int32_t len, seq_actions_t *out) {
    uint32_t tok = ++e->audio_tok[t];
    emit_audio_on(e, t, now, out);
    schedule(e, now + len, EV_AUDIO_OFF, PRIO_LOW, t, t, tok, 0);
}

static void cv_raise(seq_engine_t *e, int t, int64_t now, int32_t len, seq_actions_t *out) {
    int ph;
    if (e->pat->merge_cv) {
        if (t2_blocked(e, t)) return;   // TRK 1 has the shared out
        e->shared_owner = t;
        e->shared_high = true;
        emit(out, now, ACT_PITCH, 0, e->tr[t].cur_cents);   // winner's pitch first
        emit(out, now, ACT_GATE, 0, 1);
        ph = 0;
    } else {
        emit(out, now, ACT_GATE, t, 1);
        ph = t;
    }
    uint32_t tok = ++e->gate_tok[ph];
    schedule(e, now + len, EV_GATE_LOW, PRIO_LOW, t, ph, tok, 0);
}

static void gate_pulse(seq_engine_t *e, int t, int64_t now, int32_t len, seq_actions_t *out) {
    audio_pulse(e, t, now, len, out);
    cv_raise(e, t, now, len, out);
}

static void gate_hold_on(seq_engine_t *e, int t, int64_t now, seq_actions_t *out) {
    e->audio_tok[t]++;
    emit_audio_on(e, t, now, out);
    e->tr[t].holding = true;
    if (e->pat->merge_cv) {
        if (t2_blocked(e, t)) return;
        e->shared_owner = t;
        e->shared_high = true;
        e->gate_tok[0]++;
        emit(out, now, ACT_PITCH, 0, e->tr[t].cur_cents);
        emit(out, now, ACT_GATE, 0, 1);
    } else {
        e->gate_tok[t]++;
        emit(out, now, ACT_GATE, t, 1);
    }
}

static void gate_off(seq_engine_t *e, int t, int64_t now, seq_actions_t *out) {
    e->audio_tok[t]++;
    emit(out, now, ACT_AUDIO_OFF, t, 0);
    e->tr[t].holding = false;
    if (e->pat->merge_cv) {
        e->gate_tok[0]++;
        if (e->shared_owner == t || e->shared_owner < 0) {
            emit(out, now, ACT_GATE, 0, 0);
            e->shared_high = false;
            e->shared_owner = -1;
        }
    } else {
        e->gate_tok[t]++;
        emit(out, now, ACT_GATE, t, 0);
    }
}

static void gate_low_event(seq_engine_t *e, const seq_event_t *ev, seq_actions_t *out) {
    int ph = ev->phys;
    if (e->gate_tok[ph] != ev->token) return;   // a newer raise owns this out
    if (e->pat->merge_cv && ph == 0) {
        if (e->shared_owner == ev->track) {
            emit(out, ev->t, ACT_GATE, 0, 0);
            e->shared_high = false;
            e->shared_owner = -1;
        }
    } else {
        emit(out, ev->t, ACT_GATE, ph, 0);
    }
}

static void set_pitch(seq_engine_t *e, int t, int midi, bool slide, int32_t slide_us,
                      int64_t now, seq_actions_t *out) {
    track_rt_t *tr = &e->tr[t];
    int32_t start = tr->cur_cents;
    tr->cur_cents = midi * 100;
    tr->glide_tok++;   // FIX: a new note always cancels a glide still running
    tr->slide_us = (slide && slide_us > 0) ? slide_us : 0;
    if (e->pat->merge_cv) return;   // shared out takes the pitch at gate time
    if (!slide || slide_us <= 0) {
        emit(out, now, ACT_PITCH, t, tr->cur_cents);
        return;
    }
    tr->glide_from = start;
    tr->glide_to = tr->cur_cents;
    tr->glide_t0 = now;
    tr->glide_dur = slide_us;
    emit(out, now, ACT_PITCH, t, start);
    int64_t nt = now + (SEQ_GLIDE_STEP_US < slide_us ? SEQ_GLIDE_STEP_US : slide_us);
    schedule(e, nt, EV_GLIDE, PRIO_GLIDE, t, t, tr->glide_tok, 0);
}

static void glide_event(seq_engine_t *e, const seq_event_t *ev, int64_t now,
                        seq_actions_t *out) {
    track_rt_t *tr = &e->tr[ev->track];
    if (tr->glide_tok != ev->token || e->pat->merge_cv) return;
    // If we're behind (after a stall), jump to where the glide is NOW
    // instead of replaying every missed step as a burst of stale writes.
    int64_t t = ev->t > now ? ev->t : now;
    int64_t el = t - tr->glide_t0;
    if (el >= tr->glide_dur) {
        emit(out, ev->t, ACT_PITCH, ev->track, tr->glide_to);
        return;
    }
    int32_t v = tr->glide_from +
                (int32_t)(((int64_t)(tr->glide_to - tr->glide_from) * el) / tr->glide_dur);
    emit(out, ev->t, ACT_PITCH, ev->track, v);
    int64_t nt = t + SEQ_GLIDE_STEP_US;
    int64_t end = tr->glide_t0 + tr->glide_dur;
    schedule(e, nt < end ? nt : end, EV_GLIDE, PRIO_GLIDE, ev->track, ev->track, ev->token, 0);
}

// --- stage selection -------------------------------------------------------

static int next_order_index(seq_engine_t *e, int order, int L, int cur, int8_t *pdir) {
    switch (order) {
    case ORD_FWD:
        return pmod(cur + 1, L);
    case ORD_REV:
        return pmod(cur - 1, L);
    case ORD_PEND: {
        int nxt = cur + *pdir;
        if (nxt >= L) {
            *pdir = -1;
            return L > 1 ? L - 2 : 0;
        }
        if (nxt < 0) {
            *pdir = 1;
            return L > 1 ? 1 : 0;
        }
        return nxt;
    }
    case ORD_RAND:
        return rand_range(e, L);
    default:   // ORD_BROWN
        return clampi(cur + ((rng_next(e) & 1) ? 1 : -1), 0, L - 1);
    }
}

// Next playable stage honouring ORDER, LENgth and SKIP; -1 if all skipped.
static int next_stage_index(seq_engine_t *e, int t) {
    const track_cfg_t *cfg = &e->pat->tracks[t];
    track_rt_t *tr = &e->tr[t];
    int L = cfg->length;
    int cur = tr->cur_stage;
    for (int guard = 0; guard < L + 1; guard++) {
        int nxt;
        if (cur < 0)
            nxt = (cfg->order == ORD_REV) ? L - 1 : 0;
        else
            nxt = next_order_index(e, cfg->order, L, cur, &tr->pdir);
        cur = nxt;
        if (!e->pat->stages[nxt].skip) return nxt;
    }
    return -1;
}

// The stage track t enters next from (cur, pdir), for the orders that
// don't roll dice (so the engine isn't touched).
static int peek_stage(const seq_engine_t *e, int t, int cur, int8_t pdir) {
    const track_cfg_t *cfg = &e->pat->tracks[t];
    int L = cfg->length;
    for (int guard = 0; guard < L + 1; guard++) {
        int nxt = cur < 0 ? (cfg->order == ORD_REV ? L - 1 : 0)
                          : next_order_index((seq_engine_t *)e, cfg->order, L, cur, &pdir);
        cur = nxt;
        if (!e->pat->stages[nxt].skip) return nxt;
    }
    return -1;
}

bool seq_engine_top_next(const seq_engine_t *e) {
    if (!e->playing) return true;
    uint32_t n = e->clk + 1;   // the next master pulse
    bool bar = n % MASTER_DIV == 0;
    const track_cfg_t *cfg = &e->pat->tracks[0];
    const track_rt_t *tr = &e->tr[0];
    if (cfg->order == ORD_RAND || cfg->order == ORD_BROWN) return bar;
    int start = peek_stage(e, 0, -1, 1);
    if (start < 0) return bar;
    // TRK 1 doesn't move on that pulse, or is still inside a stage
    if (n < tr->next16 || tr->pulse_left > 0) return false;
    return peek_stage(e, 0, tr->cur_stage, tr->pdir) == start;
}

// A lane with a MIDI CC sends its stage's value when it changes (and on
// PLAY, so the receiver starts in step).
static void lane_cc(seq_engine_t *e, int l, int64_t now, seq_actions_t *out, bool force) {
    if (!e->pat->lane_cc[l]) return;
    int st = clampi(e->ln[l].cur_stage, 0, NUM_STAGES - 1);
    int v = lane_cc_value(e->pat->mods[l].values[st]);
    if (!force && v == e->ln[l].last_cc) return;
    e->ln[l].last_cc = (int16_t)v;
    emit(out, now, ACT_CC, (uint8_t)l, v);
}

static void mod_tick(seq_engine_t *e, int l, int64_t now, seq_actions_t *out) {
    const mod_lane_cfg_t *m = &e->pat->mods[l];
    lane_rt_t *ln = &e->ln[l];
    ln->cur_stage = next_order_index(e, m->order, m->length, ln->cur_stage, &ln->pdir);
    ln->disp_stage = ln->cur_stage;
    lane_cc(e, l, now, out, false);
}

// --- per-track pulse handling (Python _stage_enter / _fire_pulse) ---------

// Length of `mult` master 1/16s: from the tempo on the internal clock (exact
// integer maths, as before), or from the measured MIDI clock rate.
static int64_t p16_mul(const seq_engine_t *e, int mult) {
    if (e->ext_clock && e->ext_bpm_x100 > 0) return (1500000000LL * mult) / e->ext_bpm_x100;
    if (e->ext_clock && e->ext_p16_us > 0) return e->ext_p16_us * mult;
    return (15000000LL * mult) / e->bpm;
}

static int64_t track_pulse_us(const seq_engine_t *e, int t) {
    return p16_mul(e, DIV_VALUES[e->pat->tracks[t].div_i]);
}

static void stage_enter(seq_engine_t *e, int t, int64_t now, seq_actions_t *out) {
    track_rt_t *tr = &e->tr[t];
    const track_cfg_t *cfg = &e->pat->tracks[t];

    // End any held gate from the previous stage. FIX: keyed on whether a
    // hold is actually open, not on the previous stage's *current* gate
    // mode, so editing a HOLD stage mid-hold can't leave a gate stuck high.
    if (tr->holding) gate_off(e, t, now, out);

    int idx = next_stage_index(e, t);
    if (idx < 0) {
        tr->cur_stage = -1;
        tr->pulse_left = 1;   // rest one pulse, then try again
        tr->pulse_i = 0;
        tr->play_this = false;
        return;
    }
    tr->cur_stage = idx;
    tr->disp_stage = idx;
    const stage_t *st = &e->pat->stages[idx];
    tr->pulse_left = clampi(st->pulses + seq_mod_offset(e, t, MP_PULSES, 4), 1, 8);
    tr->pulse_i = 0;
    tr->play_this = (st->prob >= 100) || (rand_range(e, 100) < st->prob);

    tr->accum_run = clampi(tr->accum_run + st->accum, -10000, 10000);

    // slider -> invert -> RANGE -> pitch mod -> quantise -> transpose/octave/accum
    int pos = st->pitch;
    if (cfg->invert) pos = PITCH_STEPS - pos;
    int semi = stage_semi(e->pat, pos);
    semi += seq_mod_offset(e, t, MP_PITCH, 12);
    if (semi < 0) semi = 0;
    semi = quantise(semi, &SCALES[e->pat->scale_idx], e->pat->root);
    int midi = eff_base_midi(e->pat) + semi + eff_transpose(e, t) + 12 * eff_octave(e, t) +
               tr->accum_run;
    midi = fold_to_cv_range(midi);   // stay inside the 0-10 V span, in key

    int32_t slide_us = (int32_t)(track_pulse_us(e, t) * 3 / 5);
    if (tr->play_this)
        set_pitch(e, t, midi, st->slide, slide_us, now, out);
    else
        tr->cur_cents = midi * 100;   // keep pitch in sync even on a skipped roll
}

static void fire_pulse(seq_engine_t *e, int t, int64_t now, uint32_t track_tick,
                       seq_actions_t *out) {
    track_rt_t *tr = &e->tr[t];
    if (tr->cur_stage < 0 || !tr->play_this) return;
    const stage_t *st = &e->pat->stages[tr->cur_stage];
    int mode = st->gate;
    if (mode == GATE_REST) return;

    int32_t gate_e4 = eff_gate_e4(e, t);
    int32_t swing_e4 = eff_swing_e4(e, t);
    int64_t pulse_us = track_pulse_us(e, t);
    int64_t gate_us = pulse_us * gate_e4 / 10000;
    if (gate_us < SEQ_MIN_GATE_US) gate_us = SEQ_MIN_GATE_US;
    if (gate_us > pulse_us - SEQ_MIN_GATE_GAP_US) gate_us = pulse_us - SEQ_MIN_GATE_GAP_US;

    // Swing delays every other pulse. FIX: the Python keyed this on the
    // master 1/16 parity, so a track on /2 or /4 (which only ever lands on
    // even master pulses) could never swing. Using the track's own pulse
    // parity is identical for /1 tracks and makes swing work on the rest.
    int64_t swing_us = 0;
    if (swing_e4 > 0 && (track_tick & 1)) swing_us = pulse_us * swing_e4 / 20000;

    if (mode == GATE_HOLD) {
        if (tr->pulse_i == 0) {
            if (swing_us > 0)
                schedule(e, now + swing_us, EV_HOLD_ON, PRIO_GATE_ON, t, t, 0, 0);
            else
                gate_hold_on(e, t, now, out);
        }
        return;
    }
    if (mode == GATE_SING && tr->pulse_i > 0) return;

    int r = st->ratchet;
    if (r <= 1) {
        if (swing_us > 0)
            schedule(e, now + swing_us, EV_GATE_PULSE, PRIO_GATE_ON, t, t, 0, (int32_t)gate_us);
        else
            gate_pulse(e, t, now, (int32_t)gate_us, out);
        return;
    }
    int64_t sub = pulse_us / r;
    int64_t gsub = sub * gate_e4 / 10000;
    if (gsub < SEQ_MIN_RATCHET_US) gsub = SEQ_MIN_RATCHET_US;
    if (gsub > sub - SEQ_MIN_GATE_GAP_US) gsub = sub - SEQ_MIN_GATE_GAP_US;
    for (int k = 0; k < r; k++) {
        int64_t delay = swing_us + (pulse_us * k) / r;   // exact, no summed rounding
        if (delay == 0)
            gate_pulse(e, t, now, (int32_t)gsub, out);
        else
            schedule(e, now + delay, EV_GATE_PULSE, PRIO_GATE_ON, t, t, 0, (int32_t)gsub);
    }
}

static void track_tick(seq_engine_t *e, int t, int64_t now, seq_actions_t *out) {
    track_rt_t *tr = &e->tr[t];
    if (tr->pulse_left <= 0) stage_enter(e, t, now, out);
    fire_pulse(e, t, now, tr->ticks++, out);
    tr->pulse_i++;
    tr->pulse_left--;
}

static void master_pulse(seq_engine_t *e, int64_t now, seq_actions_t *out) {
    e->clk++;
    e->stats.pulses++;
    // mod lanes first, so a lane's new value is live for the tracks it
    // modulates on this same pulse
    for (int l = 0; l < NUM_MOD_LANES; l++) {
        if (e->clk >= e->ln[l].next16) {
            e->ln[l].next16 = e->clk + DIV_VALUES[e->pat->mods[l].div_i];
            mod_tick(e, l, now, out);
        }
    }
    for (int t = 0; t < NUM_TRACKS; t++) {
        if (e->clk >= e->tr[t].next16) {
            e->tr[t].next16 = e->clk + DIV_VALUES[e->pat->tracks[t].div_i];
            track_tick(e, t, now, out);
        }
    }
}

// --- transport ---------------------------------------------------------------

static void track_reset_play(track_rt_t *tr) {
    tr->cur_stage = -1;
    tr->pulse_i = 0;
    tr->pulse_left = 0;
    tr->pdir = 1;
    tr->accum_run = 0;
    tr->next16 = 0;
    tr->disp_stage = 0;
    tr->ticks = 0;
    tr->glide_tok++;
}

void seq_engine_init(seq_engine_t *e, const pattern_t *pat, uint32_t seed) {
    memset(e, 0, sizeof(*e));
    e->pat = pat;
    e->bpm = pat->bpm;
    e->rng = seed ? seed : 0x9e3779b9u;
    e->shared_owner = -1;
    for (int t = 0; t < NUM_TRACKS; t++) {
        track_reset_play(&e->tr[t]);
        e->tr[t].cur_cents = CV_BASE_MIDI * 100;
    }
    for (int l = 0; l < NUM_MOD_LANES; l++) e->ln[l].pdir = 1;
}

void seq_engine_start(seq_engine_t *e, int64_t now, seq_actions_t *out) {
    e->nev = 0;   // nothing from a previous run may fire into this one
    for (int t = 0; t < NUM_TRACKS; t++)
        if (e->tr[t].holding) gate_off(e, t, now, out);   // FIX: no stuck HOLD on reload
    e->playing = true;
    e->clk = 0;
    e->bpm = e->pat->bpm;
    e->anchor_n = 0;
    e->anchor_us = now;
    e->next_pulse_us = e->ext_clock ? INT64_MAX : pulse_time(e, 1);
    for (int l = 0; l < NUM_MOD_LANES; l++) {
        e->ln[l].cur_stage = 0;
        e->ln[l].pdir = 1;
        e->ln[l].disp_stage = 0;
        e->ln[l].next16 = DIV_VALUES[e->pat->mods[l].div_i];
        lane_cc(e, l, now, out, true);
    }
    for (int t = 0; t < NUM_TRACKS; t++) track_reset_play(&e->tr[t]);
    // sound stage 0 on both tracks now so the downbeat isn't late
    for (int t = 0; t < NUM_TRACKS; t++) {
        track_tick(e, t, now, out);
        e->tr[t].next16 = DIV_VALUES[e->pat->tracks[t].div_i];
    }
}

void seq_engine_stop(seq_engine_t *e, int64_t now, seq_actions_t *out) {
    e->playing = false;
    e->nev = 0;   // FIX: no swung/ratcheted gate can fire after STOP
    for (int t = 0; t < NUM_TRACKS; t++) {
        e->gate_tok[t]++;
        e->audio_tok[t]++;
        e->tr[t].holding = false;
        e->tr[t].glide_tok++;
        emit(out, now, ACT_GATE, t, 0);
        emit(out, now, ACT_AUDIO_OFF, t, 0);
    }
    e->shared_high = false;
    e->shared_owner = -1;
}

void seq_engine_reset(seq_engine_t *e, int64_t now, seq_actions_t *out) {
    for (int t = 0; t < NUM_TRACKS; t++)
        if (e->tr[t].holding) gate_off(e, t, now, out);
    if (e->playing) {
        seq_engine_start(e, now, out);
    } else {
        for (int t = 0; t < NUM_TRACKS; t++) track_reset_play(&e->tr[t]);
    }
}

void seq_engine_outputs_changed(seq_engine_t *e, int64_t now, seq_actions_t *out) {
    for (int t = 0; t < NUM_TRACKS; t++) {
        e->gate_tok[t]++;
        emit(out, now, ACT_GATE, t, 0);
    }
    e->shared_high = false;
    e->shared_owner = -1;
}

// Tempo changed: re-anchor at the last pulse so the grid continues from
// where it is (no jump, no drift) at the new rate.
static void retempo(seq_engine_t *e) {
    if (e->pat->bpm == e->bpm || e->ext_clock) return;
    if (e->playing) {
        e->anchor_us = pulse_time(e, e->clk);
        e->anchor_n = e->clk;
    }
    e->bpm = e->pat->bpm;
    if (e->playing) e->next_pulse_us = pulse_time(e, e->clk + 1);
}

// --- external clock ---------------------------------------------------------

void seq_engine_set_ext_clock(seq_engine_t *e, bool ext) {
    e->ext_clock = ext;
    e->ext_p16_us = 0;
    e->ext_bpm_x100 = 0;
    e->bpm = e->pat->bpm;
    e->next_pulse_us = INT64_MAX;   // the caller stops first; start() re-arms
}

void seq_engine_ext_tempo(seq_engine_t *e, int64_t p16_us) {
    if (p16_us > 0) e->ext_p16_us = p16_us;
    e->ext_bpm_x100 = 0;
}

void seq_engine_ext_tempo_exact(seq_engine_t *e, int64_t p16_us, int32_t bpm_x100) {
    seq_engine_ext_tempo(e, p16_us);
    e->ext_bpm_x100 = bpm_x100 > 0 ? bpm_x100 : 0;
}

void seq_engine_ext_pulse(seq_engine_t *e, int64_t t, seq_actions_t *out) {
    if (!e->playing || !e->ext_clock) return;
    master_pulse(e, t, out);
}

void seq_engine_continue(seq_engine_t *e, int64_t now, seq_actions_t *out) {
    (void)out;
    if (e->playing) return;
    e->playing = true;
    e->bpm = e->pat->bpm;
    e->anchor_n = e->clk;
    e->anchor_us = now;
    e->next_pulse_us = e->ext_clock ? INT64_MAX : pulse_time(e, e->clk + 1);
}

void seq_engine_locate(seq_engine_t *e, int64_t now, uint32_t pulses) {
    static seq_actions_t scratch;   // outputs of the fast-forward are discarded
    seq_actions_clear(&scratch);
    seq_engine_start(e, now, &scratch);
    if (pulses > 16384) pulses = 16384;   // MIDI SPP maximum
    for (uint32_t i = 1; i < pulses; i++) {
        seq_actions_clear(&scratch);
        master_pulse(e, now, &scratch);
        e->nev = 0;
    }
    e->nev = 0;
    e->playing = false;
    for (int t = 0; t < NUM_TRACKS; t++) {
        e->tr[t].holding = false;
        e->gate_tok[t]++;
        e->audio_tok[t]++;
        e->tr[t].glide_tok++;
    }
    e->shared_high = false;
    e->shared_owner = -1;
}

int64_t seq_engine_next_due(const seq_engine_t *e) {
    int64_t t = (e->playing && !e->ext_clock) ? e->next_pulse_us : INT64_MAX;
    if (e->nev > 0 && e->ev[0].t < t) t = e->ev[0].t;
    return t;
}

void seq_engine_advance(seq_engine_t *e, int64_t now, seq_actions_t *out) {
    retempo(e);
    int64_t horizon = now + SEQ_LOOKAHEAD_US;
    for (;;) {
        if (out->n > SEQ_MAX_ACTIONS - SEQ_ACTION_HEADROOM) break;   // rest next pass
        bool have_ev = e->nev > 0;
        bool have_pulse = e->playing && !e->ext_clock;
        if (!have_ev && !have_pulse) break;

        bool take_pulse;
        if (have_pulse && have_ev) {
            const seq_event_t *top = &e->ev[0];
            take_pulse = e->next_pulse_us < top->t ||
                         (e->next_pulse_us == top->t && PRIO_PULSE < top->prio);
        } else {
            take_pulse = have_pulse;
        }

        if (take_pulse) {
            int64_t tp = e->next_pulse_us;
            if (tp > horizon) break;
            int64_t p16 = seq_sixteenth_us(e->bpm);
            int64_t due = (now - tp) / p16 + 1;   // pulses owed right now
            if (due > SEQ_MAX_CATCHUP) {
                // A real stall: drop the backlog and resume in time
                // instead of flamming (Python MAX_CATCHUP behaviour).
                e->anchor_n = e->clk;
                e->anchor_us = now;
                e->next_pulse_us = pulse_time(e, e->clk + 1);
                e->stats.resyncs++;
                continue;
            }
            master_pulse(e, tp, out);
            e->next_pulse_us = pulse_time(e, e->clk + 1);
        } else {
            if (e->ev[0].t > horizon) break;
            seq_event_t ev = ev_pop(e);
            switch (ev.kind) {
            case EV_GATE_PULSE:
            case EV_HOLD_ON:
                if (now - ev.t > SEQ_MAX_LATE_GATE_US) {
                    e->stats.late_gates_dropped++;
                } else if (ev.kind == EV_GATE_PULSE) {
                    gate_pulse(e, ev.track, ev.t, ev.arg, out);
                } else {
                    gate_hold_on(e, ev.track, ev.t, out);
                }
                break;
            case EV_GATE_LOW:
                gate_low_event(e, &ev, out);
                break;
            case EV_AUDIO_OFF:
                if (e->audio_tok[ev.track] == ev.token)
                    emit(out, ev.t, ACT_AUDIO_OFF, ev.track, 0);
                break;
            case EV_GLIDE:
                glide_event(e, &ev, now, out);
                break;
            }
        }
    }
}
