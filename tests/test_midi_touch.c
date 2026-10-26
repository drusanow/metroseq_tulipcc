// test_midi_touch.c - host tests for the MIDI clock follower and the touch
// calibration fit.   make -C tests test
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "midi_clock.h"
#include "seq_engine.h"
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

// --- MIDI parser -----------------------------------------------------------------

static void test_parser(void) {
    midi_parser_t p;
    midi_parser_init(&p);
    // note-on with a clock byte in the middle, a sysex with a clock inside,
    // then song position 0x0102 = 2 + 1*128 = 130
    const uint8_t bytes[] = {0x90, 60, 0xF8, 100, 0xF0, 1, 2, 0xF8, 3, 0xF7,
                             0xF2, 0x02, 0x01, 0xFA, 0xFC, 0xFB};
    int clocks = 0, spp = -1, start = 0, stop = 0, cont = 0;
    for (unsigned i = 0; i < sizeof bytes; i++) {
        midi_ev_t ev = midi_parse_byte(&p, bytes[i]);
        if (ev.type == MIDI_EV_CLOCK) clocks++;
        if (ev.type == MIDI_EV_SPP) spp = ev.arg;
        if (ev.type == MIDI_EV_START) start++;
        if (ev.type == MIDI_EV_STOP) stop++;
        if (ev.type == MIDI_EV_CONTINUE) cont++;
    }
    CHECK(clocks == 2 && spp == 130 && start == 1 && stop == 1 && cont == 1,
          "clocks %d spp %d start %d stop %d cont %d", clocks, spp, start, stop, cont);
    printf("midi parser: real-time inside messages and sysex, SPP decode: ok\n");
}

// --- clock follower ----------------------------------------------------------------

static pattern_t pat;
static seq_engine_t eng;
static seq_actions_t acts;
static midi_clock_t mc;

static void send(uint8_t type, uint16_t arg, int64_t t) {
    midi_ev_t ev = {type, arg};
    seq_actions_clear(&acts);
    // the firmware runs advance() up to each event first
    seq_engine_advance(&eng, t, &acts);
    midi_clock_event(&mc, &eng, &ev, t, &acts);
}

static void setup(void) {
    pattern_default(&pat);
    for (int i = 0; i < NUM_STAGES; i++) {
        pat.stages[i].pulses = 1;
        pat.stages[i].gate = GATE_MULT;
    }
    pat.tracks[1].div_i = 0;
    seq_engine_init(&eng, &pat, 1);
    seq_engine_set_ext_clock(&eng, true);
    midi_clock_init(&mc);
}

// Gate-on times for out 0 while feeding clocks; returns how many.
static int run_clocks(int64_t *t, int nclocks, int64_t period, int jitter, int64_t *gates,
                      int maxg, int64_t *clock_times) {
    int ng = 0;
    for (int k = 0; k < nclocks; k++) {
        int64_t tc = *t + (jitter ? rnd(2 * jitter + 1) - jitter : 0);
        if (clock_times) clock_times[k] = tc;
        send(MIDI_EV_CLOCK, 0, tc);
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
    send(MIDI_EV_START, 0, t - period / 2);
    int ng = run_clocks(&t, 24 * 8, period, 300, gates, 256, ct);   // 8 beats
    // MULT on every 1/16: a gate on clock 0, 6, 12, ... at exactly the
    // clock's timestamp (phase-locked, no lag)
    CHECK(ng == 32, "gates %d", ng);
    int exact = 1;
    for (int i = 0; i < ng && i < 32; i++)
        if (gates[i] != ct[i * 6]) exact = 0;
    CHECK(exact, "gates not on the 6th-clock timestamps");
    int bpm = midi_clock_bpm_x10(&mc);
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
    send(MIDI_EV_START, 0, t);
    // downbeat clock + 32 more: five 1/16s played, stopped at clock 32
    run_clocks(&t, 1 + 32, period, 0, gates, 0, NULL);
    uint32_t clk_at_stop = eng.clk;
    send(MIDI_EV_STOP, 0, t);
    CHECK(!eng.playing, "still playing after STOP");
    run_clocks(&t, 10, period, 0, gates, 0, NULL);          // clocks while stopped
    send(MIDI_EV_CONTINUE, 0, t);
    // clocks 33..35 finish that 1/16 silently; clock 36 is the next pulse
    int ng0 = run_clocks(&t, 3, period, 0, gates, 256, NULL);
    CHECK(ng0 == 0, "pulse fired early after CONTINUE");
    int ng = run_clocks(&t, 1, period, 0, gates, 256, NULL);
    CHECK(ng == 1 && eng.clk == clk_at_stop + 1, "continue: gates %d clk %u (stop %u)", ng,
          eng.clk, clk_at_stop);

    // Song position: stop, locate to 1/16 #12, continue -> next pulse is #12
    send(MIDI_EV_STOP, 0, t);
    send(MIDI_EV_SPP, 12, t);
    send(MIDI_EV_CONTINUE, 0, t);
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
    int b1 = midi_clock_bpm_x10(&mc);
    CHECK(abs(b1 - 1200) <= 5, "hiccup moved tempo to %d", b1);
    // a real change to 90 BPM is followed within a beat
    run_clocks(&t, 30, 27778, 0, g, 0, NULL);
    int b2 = midi_clock_bpm_x10(&mc);
    CHECK(abs(b2 - 900) <= 5, "tempo after change %d", b2);
    // clock goes away -> forgotten
    CHECK(!midi_clock_alive(&mc, t + MIDI_CLOCK_TIMEOUT_US + 1), "clock still alive");
    printf("tempo: hiccup ignored (%d.%d), 120->90 followed (%d.%d), dropout detected\n",
           b1 / 10, b1 % 10, b2 / 10, b2 % 10);
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

int main(void) {
    test_parser();
    test_clock_follow();
    test_stop_continue_spp();
    test_tempo_change_and_dropout();
    test_touch_cal();
    printf(failures ? "\n%d FAILURES\n" : "\nall midi/touch tests passed\n", failures);
    return failures ? 1 : 0;
}
