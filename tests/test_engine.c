// test_engine.c - host unit tests for the timing properties the firmware
// relies on. Build and run with:  make -C tests test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "seq_engine.h"
#include "seq_json.h"

static int failures;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            failures++;                                               \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
        }                                                             \
    } while (0)

// Run the engine on an ideal clock and collect every action.
typedef struct {
    seq_action_t *a;
    int n, cap;
} log_t;

static void log_add(log_t *l, const seq_actions_t *acts) {
    for (int i = 0; i < acts->n; i++) {
        if (l->n == l->cap) {
            l->cap = l->cap ? l->cap * 2 : 1024;
            l->a = realloc(l->a, l->cap * sizeof(seq_action_t));
        }
        l->a[l->n++] = acts->a[i];
    }
}

static seq_engine_t eng;
static seq_actions_t acts;

static void run(const pattern_t *p, int64_t end_us, log_t *l) {
    l->n = 0;
    seq_engine_init(&eng, p, 1);
    seq_actions_clear(&acts);
    seq_engine_start(&eng, 0, &acts);
    log_add(l, &acts);
    for (;;) {
        int64_t t = seq_engine_next_due(&eng);
        if (t > end_us) break;
        seq_actions_clear(&acts);
        seq_engine_advance(&eng, t, &acts);
        log_add(l, &acts);
    }
}

static int rising(const log_t *l, int out, int64_t *times, int max) {
    int lvl = 0, n = 0;
    for (int i = 0; i < l->n; i++) {
        const seq_action_t *a = &l->a[i];
        if (a->type != ACT_GATE || a->out != out) continue;
        if (a->value && !lvl && n < max) times[n++] = a->due_us;
        lvl = a->value;
    }
    return n;
}

static void simple_pattern(pattern_t *p) {
    pattern_default(p);
    for (int i = 0; i < NUM_STAGES; i++) {
        p->stages[i].pulses = 1;
        p->stages[i].gate = GATE_MULT;
    }
    p->tracks[1].div_i = 0;
}

// 1. One hour at an awkward tempo: every gate edge lands exactly on the
//    ideal grid (anchor + n * 15e6 / bpm, truncated) - no accumulated drift.
static void test_no_drift(void) {
    static pattern_t p;
    simple_pattern(&p);
    p.bpm = 133;   // 15e6/133 = 112781.95... us, non-integer
    static log_t l;
    run(&p, 3600LL * 1000000, &l);
    static int64_t t[40000];
    int n = rising(&l, 0, t, 40000);
    CHECK(n > 31000, "expected ~31920 gates in an hour, got %d", n);
    int64_t worst = 0;
    for (int k = 0; k < n; k++) {
        int64_t ideal = ((int64_t)k * 15000000LL) / 133;
        int64_t err = t[k] - ideal;
        if (err < 0) err = -err;
        if (err > worst) worst = err;
    }
    CHECK(worst == 0, "grid error after 1h: %lld us", (long long)worst);
    printf("no drift over 1h @133bpm: %d gates, max grid error %lld us\n", n, (long long)worst);
}

// 2. Pitch is always written before the gate rises (CV settles first).
static void test_pitch_before_gate(void) {
    static pattern_t p;
    pattern_default(&p);
    static log_t l;
    run(&p, 20LL * 1000000, &l);
    int bad = 0;
    for (int i = 0; i < l.n; i++) {
        const seq_action_t *a = &l.a[i];
        if (a->type != ACT_PITCH) continue;
        // a pitch change must never come after a gate-on at the same instant
        for (int j = i - 1; j >= 0 && l.a[j].due_us == a->due_us; j--)
            if (l.a[j].type == ACT_GATE && l.a[j].out == a->out && l.a[j].value) bad++;
    }
    CHECK(bad == 0, "%d pitch writes landed after their gate-on", bad);
    printf("pitch-before-gate ordering: ok\n");
}

// 3. Ratchets are evenly spaced and every hit has a >= 1 ms low gap.
static void test_ratchet_gaps(void) {
    static pattern_t p;
    simple_pattern(&p);
    p.bpm = 300;   // fastest tempo
    for (int i = 0; i < NUM_STAGES; i++) p.stages[i].ratchet = 8;
    p.tracks[0].gate_pct = 100;
    static log_t l;
    run(&p, 2LL * 1000000, &l);
    int lvl = 0;
    int64_t last_low = -1000000, last_high = 0, min_low = 1 << 30, min_high = 1 << 30;
    int highs = 0;
    for (int i = 0; i < l.n; i++) {
        const seq_action_t *a = &l.a[i];
        if (a->type != ACT_GATE || a->out != 0) continue;
        if (a->value && !lvl) {
            if (highs && a->due_us - last_low < min_low) min_low = a->due_us - last_low;
            last_high = a->due_us;
            highs++;
        } else if (!a->value && lvl) {
            if (a->due_us - last_high < min_high) min_high = a->due_us - last_high;
            last_low = a->due_us;
        }
        lvl = a->value;
    }
    CHECK(highs >= 300, "expected >=300 ratchet hits in 2s, got %d", highs);
    CHECK(min_low >= SEQ_MIN_GATE_GAP_US, "min low gap %lld us", (long long)min_low);
    CHECK(min_high >= SEQ_MIN_RATCHET_US, "min high %lld us", (long long)min_high);
    printf("x8 ratchet @300bpm, 100%% gate: %d hits, min high %lld us, min low %lld us\n", highs,
           (long long)min_high, (long long)min_low);
}

// 4. A 1 s stall (flash write, debugger, ...) resyncs cleanly: no burst
//    of catch-up gates, and the grid continues one pulse after the stall.
static void test_stall_resync(void) {
    static pattern_t p;
    simple_pattern(&p);
    seq_engine_init(&eng, &p, 1);
    seq_actions_clear(&acts);
    seq_engine_start(&eng, 0, &acts);
    int64_t t = 0;
    while ((t = seq_engine_next_due(&eng)) < 1000000) {
        seq_actions_clear(&acts);
        seq_engine_advance(&eng, t, &acts);
    }
    int64_t stall_end = t + 1000000;
    seq_actions_clear(&acts);
    seq_engine_advance(&eng, stall_end, &acts);
    int gates = 0;
    for (int i = 0; i < acts.n; i++)
        if (acts.a[i].type == ACT_GATE && acts.a[i].value) gates++;
    CHECK(gates == 0, "stall produced a burst of %d gate-ons", gates);
    CHECK(eng.stats.resyncs == 1, "resyncs = %u", eng.stats.resyncs);
    int64_t next = seq_engine_next_due(&eng);
    int64_t p16 = seq_sixteenth_us(p.bpm);
    CHECK(next >= stall_end && next <= stall_end + p16, "next pulse %lld after stall end %lld",
          (long long)next, (long long)stall_end);
    printf("1s stall: resynced, 0 burst gates, next pulse +%lld us\n",
           (long long)(next - stall_end));
}

// 5. Small lateness (< MAX_CATCHUP pulses) is caught up, not dropped.
static void test_small_late_catchup(void) {
    static pattern_t p;
    simple_pattern(&p);
    seq_engine_init(&eng, &p, 1);
    seq_actions_clear(&acts);
    seq_engine_start(&eng, 0, &acts);
    int64_t p16 = seq_sixteenth_us(p.bpm);
    seq_actions_clear(&acts);
    seq_engine_advance(&eng, p16 + p16 / 2, &acts);   // 1.5 pulses late
    CHECK(eng.clk == 1, "clk %u", eng.clk);
    CHECK(eng.stats.resyncs == 0, "unexpected resync");
    printf("late by 0.5 pulse: caught up, no resync\n");
}

// 6. Tempo change: the grid continues from the last pulse, no jump back.
static void test_tempo_change(void) {
    static pattern_t p;
    simple_pattern(&p);
    seq_engine_init(&eng, &p, 1);
    seq_actions_clear(&acts);
    seq_engine_start(&eng, 0, &acts);
    int64_t t;
    while ((t = seq_engine_next_due(&eng)) < 500000) {
        seq_actions_clear(&acts);
        seq_engine_advance(&eng, t, &acts);
    }
    uint32_t clk = eng.clk;
    int64_t last_pulse = ((int64_t)clk * 15000000LL) / 120;
    p.bpm = 90;
    seq_actions_clear(&acts);
    seq_engine_advance(&eng, last_pulse + 1000, &acts);
    int64_t expect = last_pulse + 15000000LL / 90;
    CHECK(eng.next_pulse_us == expect, "next pulse %lld expected %lld",
          (long long)eng.next_pulse_us, (long long)expect);
    printf("120->90 bpm: next pulse exactly one new-tempo pulse after the last\n");
}

// 7. Merge mode: TRK2 never steals the shared output while TRK1's gate is up.
static void test_merge_priority(void) {
    static pattern_t p;
    pattern_default(&p);
    p.merge_cv = 1;
    p.tracks[0].gate_pct = 90;
    p.tracks[1].div_i = 0;
    static log_t l;
    run(&p, 10LL * 1000000, &l);
    int on_out1 = 0;
    for (int i = 0; i < l.n; i++)
        if (l.a[i].type == ACT_GATE && l.a[i].out == 1 && l.a[i].value) on_out1++;
    CHECK(on_out1 == 0, "merge mode raised the TRK2 output %d times", on_out1);
    printf("merge mode: all gates on the shared output\n");
}

// 8. HOLD: one rising edge per HOLD stage and the gate drops when the
//    stage ends - even if the stage is edited away from HOLD mid-hold.
static void test_hold_edit_no_stuck_gate(void) {
    static pattern_t p;
    simple_pattern(&p);
    for (int i = 0; i < NUM_STAGES; i++) {
        p.stages[i].gate = GATE_HOLD;
        p.stages[i].pulses = 4;
    }
    seq_engine_init(&eng, &p, 1);
    seq_actions_clear(&acts);
    seq_engine_start(&eng, 0, &acts);
    p.stages[0].gate = GATE_REST;   // user edits the holding stage
    int64_t t;
    int lvl = 1;
    int64_t high_since = 0, longest = 0;
    while ((t = seq_engine_next_due(&eng)) < 3000000) {
        seq_actions_clear(&acts);
        seq_engine_advance(&eng, t, &acts);
        for (int i = 0; i < acts.n; i++) {
            if (acts.a[i].type != ACT_GATE || acts.a[i].out != 0) continue;
            if (acts.a[i].value && !lvl) high_since = t;
            if (!acts.a[i].value && lvl && t - high_since > longest) longest = t - high_since;
            lvl = acts.a[i].value;
        }
    }
    int64_t stage_len = 4 * seq_sixteenth_us(p.bpm);
    CHECK(longest <= stage_len, "gate held %lld us > stage %lld us", (long long)longest,
          (long long)stage_len);
    printf("HOLD edited mid-hold: longest gate %lld us (stage %lld us)\n", (long long)longest,
           (long long)stage_len);
}

// 9. STOP drops everything that was queued (no gate after STOP).
static void test_stop_flushes(void) {
    static pattern_t p;
    simple_pattern(&p);
    p.tracks[0].swing = 60;
    for (int i = 0; i < NUM_STAGES; i++) p.stages[i].ratchet = 4;
    seq_engine_init(&eng, &p, 1);
    seq_actions_clear(&acts);
    seq_engine_start(&eng, 0, &acts);
    seq_actions_clear(&acts);
    seq_engine_advance(&eng, seq_engine_next_due(&eng), &acts);
    seq_actions_clear(&acts);
    seq_engine_stop(&eng, 1000, &acts);
    CHECK(seq_engine_next_due(&eng) == INT64_MAX, "events still queued after stop");
    printf("STOP: queue flushed\n");
}

// 10. Swing works on a /2 track (the Python never swung /2 or slower).
static void test_swing_div2(void) {
    static pattern_t p;
    simple_pattern(&p);
    p.tracks[0].div_i = 1;
    p.tracks[0].swing = 50;
    static log_t l;
    run(&p, 4LL * 1000000, &l);
    static int64_t t[64];
    int n = rising(&l, 0, t, 64);
    int64_t pulse = 2 * seq_sixteenth_us(p.bpm);
    int64_t off = t[1] - pulse;
    CHECK(n > 10 && off == pulse * 5000 / 20000, "second /8 gate offset %lld", (long long)off);
    printf("swing 50%% on /8 track: off-beat delayed %lld us\n", (long long)off);
}

// 11. Glide: pitch moves monotonically from the old to the new note.
static void test_glide(void) {
    static pattern_t p;
    simple_pattern(&p);
    p.stages[1].slide = 1;
    static log_t l;
    run(&p, 400000, &l);
    int steps = 0, mono = 1;
    int32_t prev = -1;
    int64_t t1 = seq_sixteenth_us(p.bpm);
    for (int i = 0; i < l.n; i++) {
        const seq_action_t *a = &l.a[i];
        if (a->type != ACT_PITCH || a->out != 0 || a->due_us < t1 ||
            a->due_us > t1 + t1 * 3 / 5)
            continue;
        if (prev >= 0 && a->value < prev) mono = 0;
        prev = a->value;
        steps++;
    }
    CHECK(steps > 20 && mono, "glide steps %d monotonic %d", steps, mono);
    printf("slide: %d pitch updates, monotonic\n", steps);
}

// 12. Fuzz: random patterns, random live edits while playing (exactly
//     what the UI can do), random stalls. The queue must never overflow,
//     no action may be dropped, output time must never go backwards.
static uint32_t frng = 12345;
static int fr(int n) {
    frng ^= frng << 13;
    frng ^= frng >> 17;
    frng ^= frng << 5;
    return (int)(frng % (uint32_t)n);
}

static void random_edit(pattern_t *p) {
    switch (fr(12)) {
    case 0: p->bpm = MIN_BPM + fr(MAX_BPM - MIN_BPM + 1); break;
    case 1: p->merge_cv = !p->merge_cv; break;
    case 3: p->range_oct = MIN_RANGE_OCT + fr(MAX_RANGE_OCT); break;
    case 4: {
        int l = fr(NUM_MOD_LANES);
        p->lane_cc[l] = fr(2) ? 1 + fr(127) : 0;
        p->lane_cc_ch[l] = 1 + fr(16);
        break;
    }
    case 2:
        p->root = fr(12);
        p->scale_idx = fr(NUM_SCALES);
        p->base_octave = MIN_BASE_OCTAVE + fr(MAX_BASE_OCTAVE - MIN_BASE_OCTAVE + 1);
        break;
    default: {
        stage_t *s = &p->stages[fr(NUM_STAGES)];
        track_cfg_t *t = &p->tracks[fr(NUM_TRACKS)];
        mod_lane_cfg_t *m = &p->mods[fr(NUM_MOD_LANES)];
        s->pitch = fr(PITCH_STEPS + 1); s->pulses = 1 + fr(8); s->gate = fr(GATE_COUNT);
        s->slide = fr(2); s->skip = fr(4) == 0; s->ratchet = 1 + fr(8); s->prob = fr(101);
        s->accum = fr(49) - 24;
        t->order = fr(ORD_COUNT); t->length = 1 + fr(NUM_STAGES); t->div_i = fr(NUM_DIVS);
        t->swing = fr(61); t->gate_pct = 5 + fr(96); t->transpose = fr(49) - 24;
        t->octave = fr(7) - 3; t->invert = fr(2);
        m->values[fr(NUM_STAGES)] = fr(201) - 100; m->order = fr(ORD_COUNT);
        m->length = 1 + fr(NUM_STAGES); m->div_i = fr(NUM_DIVS); m->depth = fr(101);
        m->dest = fr(NUM_DESTS);
    }
    }
}

static void test_fuzz(void) {
    static pattern_t p;
    uint32_t worst_q = 0, dropped = 0, backwards = 0, out_of_range = 0;
    int worst_batch = 0, dropped_reported = 0;
    long actions = 0;
    for (int run = 0; run < 300; run++) {
        pattern_default(&p);
        for (int k = 0; k < 20; k++) random_edit(&p);
        seq_engine_init(&eng, &p, run + 1);
        seq_actions_clear(&acts);
        seq_engine_start(&eng, 0, &acts);
        int64_t t = 0, next_edit = 50000, last_out;
        while (t < 20LL * 1000000) {
            t = seq_engine_next_due(&eng);
            if (t > next_edit) {
                t = next_edit;
                random_edit(&p);
                if (fr(40) == 0) {   // a UI command
                    seq_actions_clear(&acts);
                    switch (fr(3)) {
                    case 0: seq_engine_reset(&eng, t, &acts); break;
                    case 1: seq_engine_outputs_changed(&eng, t, &acts); break;
                    case 2: seq_engine_start(&eng, t, &acts); break;
                    }
                }
                if (fr(50) == 0) t += fr(400000);   // a stall
                next_edit = t + fr(200000);
            }
            seq_actions_clear(&acts);
            seq_engine_advance(&eng, t, &acts);
            dropped += acts.dropped;
            actions += acts.n;
            if (acts.n > worst_batch) worst_batch = acts.n;
            if (acts.dropped && !dropped_reported) {
                dropped_reported = 1;
                printf("  batch overflow: run %d t=%lld n=%d nev=%d\n", run, (long long)t,
                       acts.n, eng.nev);
            }
            // within one batch, output is in time order and never early
            last_out = INT64_MIN;
            for (int i = 0; i < acts.n; i++) {
                if (acts.a[i].due_us < last_out || acts.a[i].due_us > t + SEQ_LOOKAHEAD_US)
                    backwards++;
                // every pitch must sit inside the DAC's 0-10 V span (C0..C10)
                if (acts.a[i].type == ACT_PITCH &&
                    (acts.a[i].value < CV_MIN_MIDI * 100 || acts.a[i].value > CV_MAX_MIDI * 100))
                    out_of_range++;
                last_out = acts.a[i].due_us;
            }
            if ((uint32_t)eng.nev > worst_q) worst_q = eng.nev;
        }
        CHECK(eng.stats.queue_overflows == 0, "run %d: queue overflow", run);
    }
    CHECK(dropped == 0, "%u actions dropped", dropped);
    CHECK(backwards == 0, "%u actions out of time order", backwards);
    CHECK(out_of_range == 0, "%u pitches outside 0-10 V", out_of_range);
    printf("fuzz: 300 x 20s random patterns + live edits + stalls: %ld actions, "
           "peak queue %u/%d, peak batch %d/%d, %u dropped, all pitches in 0-10 V\n", actions, worst_q, SEQ_MAX_EVENTS,
           worst_batch, SEQ_MAX_ACTIONS, dropped);
}

// 12. RANGE: the sliders span 1-4 octaves; the top of the slider is the
//     range in semitones above the base, and a version 1 pattern still
//     plays the same notes.
static int first_pitch(const pattern_t *p) {
    static log_t l;
    run(p, 1000, &l);
    for (int i = 0; i < l.n; i++)
        if (l.a[i].type == ACT_PITCH && l.a[i].out == 0) return l.a[i].value / 100;
    return -1;
}

static void test_range(void) {
    static pattern_t p;
    simple_pattern(&p);
    p.scale_idx = 0;   // chromatic: no snapping
    p.base_octave = 0;
    p.stages[0].pitch = PITCH_STEPS;
    int got[MAX_RANGE_OCT + 1];
    for (int r = MIN_RANGE_OCT; r <= MAX_RANGE_OCT; r++) {
        p.range_oct = r;
        got[r] = first_pitch(&p);
        CHECK(got[r] == CV_BASE_MIDI + 12 * r, "range %d: top of slider = %d", r, got[r]);
    }
    p.range_oct = 2;
    p.stages[0].pitch = PITCH_STEPS / 2;
    CHECK(first_pitch(&p) == CV_BASE_MIDI + 12, "range 2: middle = one octave up");
    p.tracks[0].invert = 1;
    p.stages[0].pitch = 0;
    CHECK(first_pitch(&p) == CV_BASE_MIDI + 24, "invert flips within the range");
    // version 1: semitones over 36 -> positions, the same notes
    int bad = 0;
    for (int semi = 0; semi <= 36; semi++) {
        static pattern_t v1;
        simple_pattern(&v1);
        v1.scale_idx = 0;
        v1.stages[0].pitch = (uint8_t)semi;
        pattern_upgrade_v1(&v1);
        if (stage_semi(&v1, v1.stages[0].pitch) != semi) bad++;
    }
    CHECK(bad == 0, "%d version 1 pitches moved", bad);
    printf("range: 1-4 octaves (top = base + %d/%d/%d/%d), invert, version 1 pitches kept\n",
           got[1] - CV_BASE_MIDI, got[2] - CV_BASE_MIDI, got[3] - CV_BASE_MIDI,
           got[4] - CV_BASE_MIDI);
}

// 13. A MOD lane with a CC sends its value when it changes, and on PLAY.
static void test_lane_cc(void) {
    static pattern_t p;
    simple_pattern(&p);
    static const int8_t vals[NUM_STAGES] = {-100, -100, 0, 0, 100, 50, 50, -50};
    for (int i = 0; i < NUM_STAGES; i++) p.mods[2].values[i] = vals[i];
    p.mods[2].div_i = 0;   // every 1/16
    p.lane_cc[2] = 74;
    p.bpm = 120;
    static log_t l;
    run(&p, 8 * 125000 - 1000, &l);   // stages 1..8 of the lane
    int got[16], n = 0;
    for (int i = 0; i < l.n && n < 16; i++)
        if (l.a[i].type == ACT_CC) {
            CHECK(l.a[i].out == 2, "CC from lane %d", l.a[i].out);
            got[n++] = l.a[i].value;
        }
    static const int want[] = {0, 64, 127, 95, 32};   // repeats aren't sent
    CHECK(n == 5, "%d CCs sent", n);
    for (int i = 0; i < n && i < 5; i++) CHECK(got[i] == want[i], "CC %d = %d", i, got[i]);
    p.lane_cc[2] = 0;
    run(&p, 1000000, &l);
    int none = 0;
    for (int i = 0; i < l.n; i++) none += l.a[i].type == ACT_CC;
    CHECK(none == 0, "lane with CC off sent %d", none);
    printf("lane CC: 0/64/127/95/32 on change only, nothing when off\n");
}

// 14. seq_engine_top_next(): true on the pulse TRK 1 starts its sequence
//     over (FWD, REV, PEND, with skips and multi-pulse stages), else on the
//     bar for RAND / BROWN.
static int tops_in(const pattern_t *p, int pulses, uint32_t *at, int max) {
    seq_engine_init(&eng, p, 1);
    seq_actions_clear(&acts);
    seq_engine_set_ext_clock(&eng, true);
    seq_engine_ext_tempo(&eng, 125000);
    seq_engine_start(&eng, 0, &acts);
    int n = 0;
    for (int k = 1; k <= pulses; k++) {
        bool top = seq_engine_top_next(&eng);
        seq_actions_clear(&acts);
        seq_engine_ext_pulse(&eng, k * 125000LL, &acts);
        // what really happened: TRK 1 entered its first playing stage
        if (top && n < max) at[n++] = eng.clk;
        if (top) CHECK(eng.tr[0].pulse_i == 1, "top at %u but mid-stage", eng.clk);
    }
    return n;
}

static void test_top_next(void) {
    static pattern_t p;
    uint32_t at[16];
    simple_pattern(&p);
    int n = tops_in(&p, 40, at, 16);   // FWD, 8 x 1 pulse
    CHECK(n == 5 && at[0] == 8 && at[1] == 16 && at[4] == 40, "FWD: %d tops, first %u", n, at[0]);
    p.tracks[0].length = 5;
    p.stages[0].skip = 1;              // starts on stage 2: 4 stages
    p.stages[2].pulses = 3;            // 1 + 3 + 1 + 1 = 6 pulses a pass
    n = tops_in(&p, 24, at, 16);
    CHECK(n == 4 && at[0] == 6 && at[3] == 24, "FWD skip/pulses: %d tops, first %u", n, at[0]);
    simple_pattern(&p);
    p.tracks[0].order = ORD_PEND;      // 0..7..1: 14 a pass
    n = tops_in(&p, 28, at, 16);
    CHECK(n == 2 && at[0] == 14 && at[1] == 28, "PEND: %d tops, first %u", n, at[0]);
    p.tracks[0].order = ORD_REV;
    p.tracks[0].div_i = 1;             // 1/8: 16 pulses a pass
    n = tops_in(&p, 32, at, 16);
    CHECK(n == 2 && at[0] == 16 && at[1] == 32, "REV /8: %d tops, first %u", n, at[0]);
    p.tracks[0].order = ORD_RAND;
    n = tops_in(&p, 48, at, 16);
    CHECK(n == 3 && at[0] == 16 && at[2] == 48, "RAND: on the bar, %d tops", n);
    printf("top of sequence: FWD, skips + pulses, PEND, REV /8; RAND on the bar\n");
}

// 15. JSON (Studio patches): a random pattern round-trips exactly; missing
//     keys keep their defaults; junk is clamped.
static void test_json(void) {
    static pattern_t p, q, d;
    int bad = 0;
    for (int k = 0; k < 50; k++) {
        pattern_default(&p);
        for (int e = 0; e < 40; e++) random_edit(&p);
        pattern_sanitise(&p);
        cJSON *o = pattern_to_json(&p);
        char *txt = cJSON_PrintUnformatted(o);
        cJSON *back = cJSON_Parse(txt);
        memset(&q, 0xA5, sizeof q);
        pattern_from_json(back, &q);
        // compare field by field (struct padding isn't data)
        if (memcmp(p.stages, q.stages, sizeof p.stages) || memcmp(p.tracks, q.tracks, sizeof p.tracks) ||
            memcmp(p.mods, q.mods, sizeof p.mods) || memcmp(p.lane_cc, q.lane_cc, sizeof p.lane_cc) ||
            memcmp(p.lane_cc_ch, q.lane_cc_ch, sizeof p.lane_cc_ch) || p.bpm != q.bpm ||
            p.root != q.root || p.scale_idx != q.scale_idx || p.base_octave != q.base_octave ||
            p.range_oct != q.range_oct || p.merge_cv != q.merge_cv)
            bad++;
        cJSON_free(txt);
        cJSON_Delete(o);
        cJSON_Delete(back);
    }
    CHECK(bad == 0, "%d of 50 patterns changed in a JSON round trip", bad);
    cJSON *o = cJSON_Parse("{\"bpm\": 99, \"range\": 9, \"stages\": [{\"pitch\": 200}]}");
    pattern_from_json(o, &q);
    cJSON_Delete(o);
    pattern_default(&d);
    CHECK(q.bpm == 99 && q.range_oct == MAX_RANGE_OCT && q.stages[0].pitch == PITCH_STEPS &&
              q.stages[1].pitch == d.stages[1].pitch && q.tracks[1].order == d.tracks[1].order,
          "partial / junk JSON");
    printf("JSON: 50 random patterns round-trip; missing keys default, junk clamped\n");
}

int main(void) {
    test_no_drift();
    test_pitch_before_gate();
    test_ratchet_gaps();
    test_stall_resync();
    test_small_late_catchup();
    test_tempo_change();
    test_merge_priority();
    test_hold_edit_no_stuck_gate();
    test_stop_flushes();
    test_swing_div2();
    test_glide();
    test_range();
    test_lane_cc();
    test_top_next();
    test_json();
    test_fuzz();
    printf(failures ? "\n%d FAILURES\n" : "\nall engine tests passed\n", failures);
    return failures ? 1 : 0;
}
