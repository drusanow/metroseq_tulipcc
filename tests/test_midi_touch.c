// test_midi_touch.c - host tests for the MIDI clock follower and the touch
// calibration fit.   make -C tests test
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "seq_clock.h"
#include "studio/midi_parse.h"
#include "seq_engine.h"
#include "tulip/touch_aim.h"
#include "tulip/touch_cal.h"

static int failures;
#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            failures++;                                         \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);       \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static uint32_t rng = 777;
static int rnd(int n) {   // 0..n-1
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (int)(rng % (uint32_t)n);
}

// --- clock follower ----------------------------------------------------------------

static pattern_t pat;
static seq_engine_t eng;
static seq_actions_t acts;
static tclock_t mc;
static seq_clock_t sc;

// The firmware's arrangement: the Studio master clock following MIDI,
// driving the engine through seq_clock.
static void send(uint8_t type, uint16_t arg, int64_t t) {
    seq_actions_clear(&acts);
    // the firmware runs the engine's own events up to each clock first
    seq_engine_advance(&eng, t, &acts);
    tclock_midi(&mc, type, arg, NULL, t);
}

static void setup(void) {
    pattern_default(&pat);
    for (int i = 0; i < NUM_STAGES; i++) {
        pat.stages[i].pulses = 1;
        pat.stages[i].gate = GATE_MULT;
    }
    pat.tracks[1].div_i = 0;
    seq_engine_init(&eng, &pat, 1);
    tclock_init(&mc, 12000);
    sc = (seq_clock_t){.e = &eng, .acts = &acts};
    tclock_sink_t sink;
    seq_clock_sink(&sc, &sink);
    tclock_add_sink(&mc, &sink);
    tclock_set_ext(&mc, true, 0);
}

// Gate-on times for out 0 while feeding clocks; returns how many.
static int run_clocks(int64_t *t, int nclocks, int64_t period, int jitter, int64_t *gates,
                      int maxg, int64_t *clock_times) {
    int ng = 0;
    for (int k = 0; k < nclocks; k++) {
        int64_t tc = *t + (jitter ? rnd(2 * jitter + 1) - jitter : 0);
        if (clock_times) clock_times[k] = tc;
        send(MP_CLOCK, 0, tc);
        for (int i = 0; i < acts.n; i++)
            if (acts.a[i].type == ACT_GATE && acts.a[i].out == 0 && acts.a[i].value && ng < maxg)
                gates[ng++] = acts.a[i].due_us;
        *t += period;
    }
    return ng;
}

static void test_clock_follow(void) {
    setup();
    int64_t t = 1000000;
    const int64_t period = 20833;   // 120 BPM
    static int64_t gates[256], ct[1024];
    // a few clocks before START so the tempo is known
    run_clocks(&t, 12, period, 300, gates, 0, NULL);
    send(MP_START, 0, t - period / 2);
    int ng = run_clocks(&t, 24 * 8, period, 300, gates, 256, ct);   // 8 beats
    // MULT on every 1/16: a gate on clock 0, 6, 12, ... at exactly the
    // clock's timestamp (phase-locked, no lag)
    CHECK(ng == 32, "gates %d", ng);
    int exact = 1;
    for (int i = 0; i < ng && i < 32; i++)
        if (gates[i] != ct[i * 6]) exact = 0;
    CHECK(exact, "gates not on the 6th-clock timestamps");
    int bpm = tclock_ext_bpm_x10(&mc);
    CHECK(abs(bpm - 1200) <= 5, "bpm x10 %d", bpm);
    // gate length comes from the averaged tempo, not from the jittery clock
    CHECK(llabs(eng.ext_p16_us - 125000) < 500, "p16 %lld", (long long)eng.ext_p16_us);
    printf("clock follow @120bpm +-0.3ms jitter: %d gates exactly on every 6th clock, "
           "tempo %d.%d bpm\n", ng, bpm / 10, bpm % 10);
}

static void test_stop_continue_spp(void) {
    setup();
    int64_t t = 0;
    const int64_t period = 20833;
    static int64_t gates[256];
    run_clocks(&t, 6, period, 0, gates, 0, NULL);
    send(MP_START, 0, t);
    // downbeat clock + 32 more: five 1/16s played, stopped at clock 32
    run_clocks(&t, 1 + 32, period, 0, gates, 0, NULL);
    uint32_t clk_at_stop = eng.clk;
    send(MP_STOP, 0, t);
    CHECK(!eng.playing, "still playing after STOP");
    run_clocks(&t, 10, period, 0, gates, 0, NULL);          // clocks while stopped
    send(MP_CONTINUE, 0, t);
    // clocks 33..35 finish that 1/16 silently; clock 36 is the next pulse
    int ng0 = run_clocks(&t, 3, period, 0, gates, 256, NULL);
    CHECK(ng0 == 0, "pulse fired early after CONTINUE");
    int ng = run_clocks(&t, 1, period, 0, gates, 256, NULL);
    CHECK(ng == 1 && eng.clk == clk_at_stop + 1, "continue: gates %d clk %u (stop %u)", ng,
          eng.clk, clk_at_stop);

    // Song position: stop, locate to 1/16 #12, continue -> next pulse is #12
    send(MP_STOP, 0, t);
    send(MP_SPP, 12, t);
    send(MP_CONTINUE, 0, t);
    run_clocks(&t, 1, period, 0, gates, 0, NULL);
    CHECK(eng.clk == 12 && eng.playing, "SPP 12: clk %u", eng.clk);
    // with 8 one-pulse stages, 1/16 #12 is stage 5 (index 4)
    CHECK(eng.tr[0].cur_stage == 4, "SPP 12: stage %d", eng.tr[0].cur_stage);
    printf("STOP/CONTINUE resumes mid-bar; SPP 12 + CONTINUE lands on 1/16 #12: ok\n");
}

static void test_tempo_change_and_dropout(void) {
    setup();
    int64_t t = 0;
    static int64_t g[8];
    run_clocks(&t, 30, 20833, 0, g, 0, NULL);
    // one late clock (a hiccup) must not move the estimate
    t += 30000;
    run_clocks(&t, 1, 20833, 0, g, 0, NULL);
    int b1 = tclock_ext_bpm_x10(&mc);
    CHECK(abs(b1 - 1200) <= 5, "hiccup moved tempo to %d", b1);
    // a real change to 90 BPM is followed within a beat
    run_clocks(&t, 30, 27778, 0, g, 0, NULL);
    int b2 = tclock_ext_bpm_x10(&mc);
    CHECK(abs(b2 - 900) <= 5, "tempo after change %d", b2);
    // clock goes away -> forgotten
    CHECK(!tclock_ext_alive(&mc, t + TC_CLOCK_TIMEOUT_US + 1), "clock still alive");
    printf("tempo: hiccup ignored (%d.%d), 120->90 followed (%d.%d), dropout detected\n",
           b1 / 10, b1 % 10, b2 / 10, b2 % 10);
}

// --- the Studio master clock vs the engine's own clock --------------------------------

// metroseq used to time its 1/16s itself; now the shared master clock
// delivers them. Same pattern, same tempo: the CV output must be identical.
typedef struct {
    int64_t t;
    uint8_t type, out;
    int32_t value;
} rec_t;

static int record(seq_actions_t *a, rec_t *r, int n, int max) {
    for (int i = 0; i < a->n && n < max; i++)
        if (a->a[i].type == ACT_GATE || a->a[i].type == ACT_PITCH)
            r[n++] = (rec_t){a->a[i].due_us, a->a[i].type, a->a[i].out, a->a[i].value};
    seq_actions_clear(a);
    return n;
}

static void test_master_equivalence(void) {
    static rec_t ra[20000], rb[20000];
    static pattern_t p;
    int compared = 0, mismatches = 0;
    for (int trial = 0; trial < 20; trial++) {
        pattern_default(&p);
        p.bpm = 60 + rnd(180);
        for (int i = 0; i < NUM_STAGES; i++) {
            p.stages[i].pulses = 1 + rnd(4);
            p.stages[i].ratchet = 1 + rnd(3);
            p.stages[i].gate = rnd(GATE_COUNT);
            p.stages[i].slide = rnd(3) == 0;
            p.stages[i].pitch = rnd(PITCH_STEPS);
        }
        for (int t = 0; t < NUM_TRACKS; t++) {
            p.tracks[t].div_i = rnd(4);
            p.tracks[t].swing = rnd(3) ? 0 : 20 + rnd(30);
            p.tracks[t].gate_pct = 10 + rnd(90);
        }
        pattern_sanitise(&p);
        const int64_t t0 = 50000, end = t0 + 8000000;
        // A: the engine's own internal clock
        static seq_engine_t ea, eb;
        static seq_actions_t aa, ab;
        seq_engine_init(&ea, &p, 7);
        seq_actions_clear(&aa);
        seq_engine_start(&ea, t0, &aa);
        int na = record(&aa, ra, 0, 20000);
        for (;;) {
            int64_t due = seq_engine_next_due(&ea);
            if (due > end) break;
            seq_engine_advance(&ea, due, &aa);
            na = record(&aa, ra, na, 20000);
        }
        // B: driven by the master clock (downbeat at t0)
        seq_engine_init(&eb, &p, 7);
        seq_actions_clear(&ab);
        static tclock_t clk;
        static seq_clock_t c;
        tclock_init(&clk, p.bpm * 100);
        c = (seq_clock_t){.e = &eb, .acts = &ab};
        tclock_sink_t sink;
        seq_clock_sink(&c, &sink);
        tclock_add_sink(&clk, &sink);
        tclock_play(&clk, t0 - TC_START_LEAD_US);
        int nb = 0;
        for (;;) {
            int64_t d1 = seq_engine_next_due(&eb), d2 = tclock_next_due(&clk);
            int64_t due = d1 < d2 ? d1 : d2;
            if (due > end) break;
            if (d1 <= due) seq_engine_advance(&eb, due, &ab);
            tclock_advance(&clk, due);
            nb = record(&ab, rb, nb, 20000);
        }
        // same events at the same times (order within one instant may differ)
        if (na != nb) {
            mismatches++;
            printf("  trial %d: %d vs %d events\n", trial, na, nb);
            continue;
        }
        for (int i = 0; i < na; i++) {
            bool found = false;
            for (int j = i > 8 ? i - 8 : 0; j < na && j < i + 8 && !found; j++)
                found = rb[j].t == ra[i].t && rb[j].type == ra[i].type &&
                        rb[j].out == ra[i].out && rb[j].value == ra[i].value;
            if (!found) {
                if (!mismatches)
                    printf("  trial %d bpm %d event %d: %lld %d/%d/%d vs %lld %d/%d/%d\n", trial,
                           p.bpm, i, (long long)ra[i].t, ra[i].type, ra[i].out, ra[i].value,
                           (long long)rb[i].t, rb[i].type, rb[i].out, rb[i].value);
                mismatches++;
                break;
            }
        }
        compared += na;
    }
    CHECK(!mismatches, "%d trials differ", mismatches);
    printf("master clock vs engine's own clock: 20 random patterns, %d CV events identical\n",
           compared);
}

// --- touch calibration -----------------------------------------------------------

static const float SX[5] = {64, 960, 960, 64, 512};
static const float SY[5] = {64, 64, 536, 536, 300};

// Make raw readings for the targets through the inverse of a known map,
// with up to `noise` px of tap error.
static void raw_from(const float m[6], float noise, float *rx, float *ry) {
    // invert the 2x2 part
    double det = m[0] * m[4] - m[1] * m[3];
    for (int i = 0; i < 5; i++) {
        double x = SX[i] - m[2] + (noise ? (rnd(2001) - 1000) / 1000.0 * noise : 0);
        double y = SY[i] - m[5] + (noise ? (rnd(2001) - 1000) / 1000.0 * noise : 0);
        rx[i] = (float)((m[4] * x - m[1] * y) / det);
        ry[i] = (float)((-m[3] * x + m[0] * y) / det);
    }
}

static void test_touch_cal(void) {
    float rx[5], ry[5], m[6];
    touch_cal_fit_t fit;
    // Tulip's default map, perfect taps -> recovered exactly
    const float def[6] = TOUCH_CAL_DEFAULT;
    raw_from(def, 0, rx, ry);
    CHECK(touch_cal_solve(rx, ry, SX, SY, 5, m, &fit) && fit.rms_px < 0.01f &&
              fabsf(m[4] - 0.8f) < 1e-4f && fabsf(m[2] + 2.0f) < 0.01f,
          "default not recovered: %f %f rms %f", m[2], m[4], fit.rms_px);
    // a skewed, offset panel with realistic +-3 px tap error
    const float skew[6] = {1.03f, 0.02f, -11.0f, -0.015f, 0.79f, 4.0f};
    raw_from(skew, 3.0f, rx, ry);
    bool ok = touch_cal_solve(rx, ry, SX, SY, 5, m, &fit);
    float worst = 0;
    for (int i = 0; i < 5; i++) {   // how well it maps the TRUE positions
        float x, y, rx0[5], ry0[5];
        raw_from(skew, 0, rx0, ry0);
        touch_cal_apply(m, rx0[i], ry0[i], &x, &y);
        float d = hypotf(x - SX[i], y - SY[i]);
        if (d > worst) worst = d;
    }
    CHECK(ok && worst < 4.0f, "skew fit ok=%d worst %f", ok, worst);
    // one tap 60 px off its target -> rejected
    raw_from(def, 0, rx, ry);
    rx[2] += 60;
    CHECK(!touch_cal_solve(rx, ry, SX, SY, 5, m, &fit), "bad tap accepted");
    // all taps in one spot -> degenerate
    for (int i = 0; i < 5; i++) rx[i] = ry[i] = 300;
    CHECK(!touch_cal_solve(rx, ry, SX, SY, 5, m, &fit), "degenerate accepted");
    printf("touch cal: exact recovery, skew+noise within %.1f px, bad tap and degenerate "
           "rejected\n", worst);
}

// A panel that bends towards its edges: an affine map plus a smooth pull
// that grows with the square of the distance from the centre, up to 14 px
// at the top and bottom edges.
static const float BENT[6] = {1.02f, 0.01f, -6.0f, -0.01f, 0.81f, -8.0f};
static void bent_raw(float sx, float sy, float *rx, float *ry) {
    float v = (sy - 300) / 300.0f, u = (sx - 512) / 512.0f;
    float y = sy - 14.0f * v * fabsf(v) * (1 - 0.3f * u * u);   // where the panel says it is
    float x = sx - 8.0f * u * fabsf(u);
    // invert the affine map for that point
    double det = BENT[0] * BENT[4] - BENT[1] * BENT[3];
    double ax = x - BENT[2], ay = y - BENT[5];
    *rx = (float)((BENT[4] * ax - BENT[1] * ay) / det);
    *ry = (float)((-BENT[3] * ax + BENT[0] * ay) / det);
}

static void test_touch_grid(void) {
    float rx[9], ry[9];
    for (int i = 0; i < 9; i++)
        bent_raw(TOUCH_GRID_X(i % 3, 1024), TOUCH_GRID_Y(i / 3, 600), &rx[i], &ry[i]);
    touch_map_t map;
    touch_cal_fit_t fit;
    CHECK(touch_cal_solve_grid(rx, ry, 1024, 600, &map, &fit) && map.grid, "grid solve failed: rms %f",
          fit.rms_px);
    // the bottom bar (y 550..590) and the middle, affine alone vs with the grid
    float worst_aff = 0, worst_grid = 0, bottom_grid = 0;
    for (int y = 30; y <= 590; y += 20)
        for (int x = 20; x <= 1000; x += 40) {
            float r1, r2, ax, ay, gx, gy;
            bent_raw((float)x, (float)y, &r1, &r2);
            touch_cal_apply(map.m, r1, r2, &ax, &ay);
            touch_map_apply(&map, 1024, 600, r1, r2, &gx, &gy);
            float da = hypotf(ax - x, ay - y), dg = hypotf(gx - x, gy - y);
            if (y >= 550 && da > worst_aff) worst_aff = da;
            if (y >= 550 && dg > bottom_grid) bottom_grid = dg;
            if (dg > worst_grid) worst_grid = dg;
        }
    CHECK(worst_grid < 3.0f && bottom_grid < worst_aff,
          "grid didn't help: worst %f px with it, bottom row %f px with, %f px without", worst_grid,
          bottom_grid, worst_aff);
    // the targets themselves come out exact
    float x, y;
    touch_map_apply(&map, 1024, 600, rx[7], ry[7], &x, &y);
    CHECK(fabsf(x - 512) < 0.5f && fabsf(y - 560) < 0.5f, "bottom-middle target off: %f %f", x, y);
    // one tap 50 px off -> refused (the correction would exceed 40 px)
    rx[8] += 60;
    CHECK(!touch_cal_solve_grid(rx, ry, 1024, 600, &map, &fit), "bad grid tap accepted");
    // no grid: exactly the affine map
    touch_map_t aff = {.m = TOUCH_CAL_DEFAULT};
    touch_map_apply(&aff, 1024, 600, 500, 400, &x, &y);
    CHECK(fabsf(x - 498) < 0.01f && fabsf(y - 310.4f) < 0.01f, "affine-only map changed: %f %f", x, y);
    printf("touch grid: 9 targets, bottom row %.1f px (affine alone %.1f px), worst %.1f px anywhere, "
           "bad tap refused\n", bottom_grid, worst_aff, worst_grid);
}

// --- touch_aim: two buttons 100 x 40 with a 6 px gap, at the bottom ------------------
static bool on_btn(int x, int y, void *ctx) {
    (void)ctx;
    bool row = y >= 550 && y < 590;
    return row && ((x >= 100 && x < 200) || (x >= 206 && x < 306));
}

static int feed(touch_aim_t *a, int x, int y, bool p, uint32_t t, aim_ev_t *out) {
    aim_ev_t in = {(int16_t)x, (int16_t)y, p};
    return touch_aim_feed(a, &in, t, out);
}

static void test_touch_aim(void) {
    touch_aim_t a;
    aim_ev_t o[2];
    touch_aim_init(&a, on_btn, NULL, 1024, 600);
    // on the button: straight through, at once
    CHECK(feed(&a, 150, 570, true, 0, o) == 1 && o[0].pressed && o[0].x == 150 && o[0].y == 570, "direct");
    CHECK(feed(&a, 152, 571, true, 5, o) == 1 && o[0].x == 152, "direct move");
    CHECK(feed(&a, 152, 571, false, 10, o) == 1 && !o[0].pressed, "direct release");
    // 12 px below the row (an off calibration): snapped up onto it at once
    CHECK(feed(&a, 150, 599, true, 100, o) == 1 && o[0].pressed && on_btn(o[0].x, o[0].y, NULL),
          "snap from below: %d,%d", o[0].x, o[0].y);
    int dx = o[0].x - 150, dy = o[0].y - 599;
    CHECK(feed(&a, 150, 597, true, 105, o) == 1 && o[0].x == 150 + dx && o[0].y == 597 + dy,
          "snapped finger keeps its offset: %d,%d (+%d,%d)", o[0].x, o[0].y, dx, dy);
    feed(&a, 150, 597, false, 110, o);
    // in the 6 px gap: the nearer button
    CHECK(feed(&a, 201, 570, true, 200, o) == 1 && on_btn(o[0].x, o[0].y, NULL) && o[0].x < 203,
          "gap snap: %d", o[0].x);
    feed(&a, 201, 570, false, 205, o);
    // far from anything, held still: nothing at first...
    CHECK(feed(&a, 150, 515, true, 300, o) == 0, "held back");
    // ...the samples wander and settle 20 px nearer; at 150 ms it's found
    int n = 0;
    for (uint32_t t = 305; t < 460 && !n; t += 5) n = feed(&a, 150, 525, true, t, o);
    CHECK(n == 1 && o[0].pressed && on_btn(o[0].x, o[0].y, NULL), "aim found the button");
    CHECK(feed(&a, 150, 525, false, 470, o) == 1 && !o[0].pressed, "aim release");
    // a quick tap on nothing still arrives as a press and a release
    CHECK(feed(&a, 600, 300, true, 600, o) == 0 && feed(&a, 600, 300, false, 640, o) == 2 && o[0].pressed &&
              !o[1].pressed && o[0].x == 600,
          "tap on nothing");
    // a drag from nothing: through once it moves, from where it is
    feed(&a, 600, 300, true, 700, o);
    CHECK(feed(&a, 600, 340, true, 720, o) == 1 && o[0].pressed && o[0].y == 340, "drag passes");
    CHECK(feed(&a, 600, 380, true, 740, o) == 1 && o[0].y == 380, "drag follows");
    feed(&a, 600, 380, false, 760, o);
    // held still with no event at all: the tick finds it
    CHECK(feed(&a, 150, 518, true, 800, o) == 0 && touch_aim_tick(&a, 900, o) == 0 &&
              touch_aim_tick(&a, 960, o) == 1 && on_btn(o[0].x, o[0].y, NULL),
          "tick aim");
    feed(&a, 150, 518, false, 1000, o);
    printf("touch aim: direct, snap from below and in a gap, hold-to-aim, taps and drags on nothing\n");
}

// A new pattern (a preset) waits for the top of TRK 1's sequence, then the
// tracks start over with it on that very 1/16.
static pattern_t next_pat;
static int swaps;
static void do_swap(void) {
    pat = next_pat;
    swaps++;
}

static void test_swap_on_top(void) {
    setup();
    sc.swap = do_swap;
    sc.swap_at = SWAP_AT_TRK1;
    int64_t t = 0;
    const int64_t period = 20833;
    static int64_t gates[64];
    run_clocks(&t, 6, period, 0, gates, 0, NULL);
    send(MP_START, 0, t);
    run_clocks(&t, 1 + 6 * 3, period, 0, gates, 0, NULL);   // into stage 4
    next_pat = pat;
    next_pat.stages[0].pitch = PITCH_STEPS;   // stage 1 now plays 3 octaves up
    seq_clock_swap(&sc);
    swaps = 0;
    int when = -1;
    for (int k = 0; k < 6 * 8 && when < 0; k++) {
        run_clocks(&t, 1, period, 0, gates, 0, NULL);
        if (swaps) when = (int)eng.clk;
    }
    // stages 4..8 still played: the swap comes on 1/16 number 8 (the top)
    CHECK(when == 0 && eng.tr[0].cur_stage == 0, "swapped at clk %d stage %d", when,
          eng.tr[0].cur_stage);
    CHECK(eng.tr[0].cur_cents == (CV_BASE_MIDI + 36) * 100, "new pattern's pitch %d",
          (int)eng.tr[0].cur_cents);
    // stopped: the next PLAY starts with the new one
    send(MP_STOP, 0, t);
    next_pat.stages[0].pitch = 0;
    seq_clock_swap(&sc);
    t += period;
    send(MP_START, 0, t);
    run_clocks(&t, 1, period, 0, gates, 0, NULL);   // the downbeat
    CHECK(swaps == 2 && eng.tr[0].cur_cents == CV_BASE_MIDI * 100, "swap on PLAY");
    printf("new pattern: waits for the top of TRK 1's sequence, then starts from stage 1\n");
}

// SWAP_AT_BAR (the default) and 2 BARS: with TRK 1 at 7 steps and TRK 2 at
// 5, a preset queued on 1/16 number 3 comes in on the bar line (16, or 32),
// where every other app's downbeat is - and both tracks start over there,
// each from its first stage.
static void test_swap_on_bar(void) {
    for (int bars = 1; bars <= 2; bars++) {
        setup();
        sc.swap = do_swap;
        sc.swap_at = bars == 1 ? SWAP_AT_BAR : SWAP_AT_2BARS;
        pat.tracks[0].length = 7;
        pat.tracks[1].length = 5;
        int64_t t = 0;
        const int64_t period = 20833;
        static int64_t gates[64];
        run_clocks(&t, 6, period, 0, gates, 0, NULL);
        send(MP_START, 0, t);
        run_clocks(&t, 1 + 6 * 3, period, 0, gates, 0, NULL);   // 1/16 number 3
        next_pat = pat;
        next_pat.stages[0].pitch = PITCH_STEPS;
        seq_clock_swap(&sc);
        swaps = 0;
        int clocks = 1 + 6 * 3, when = -1;
        for (int k = 0; k < 6 * 40 && when < 0; k++) {
            run_clocks(&t, 1, period, 0, gates, 0, NULL);
            clocks++;
            if (swaps) when = (clocks - 1) / 6;   // the 1/16 it came in on
        }
        // (TRK 2 plays REV by default: its first stage of 5 is the 5th)
        CHECK(when == 16 * bars && eng.tr[0].cur_stage == 0 && eng.tr[1].cur_stage == 4,
              "%d bar(s): swapped on 1/16 %d, stages %d %d", bars, when, eng.tr[0].cur_stage,
              eng.tr[1].cur_stage);
    }
    printf("new pattern: on the next bar (or 2-bar) line whatever the track lengths (7 / 5), "
           "both tracks from stage 1\n");
}

int main(void) {
    test_clock_follow();
    test_swap_on_top();
    test_swap_on_bar();
    test_stop_continue_spp();
    test_tempo_change_and_dropout();
    test_master_equivalence();
    test_touch_cal();
    test_touch_grid();
    test_touch_aim();
    printf(failures ? "\n%d FAILURES\n" : "\nall midi/touch tests passed\n", failures);
    return failures ? 1 : 0;
}
