// test_studio.c - host tests for the Studio master clock and MIDI parser.
//   make -C tests test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "studio/midi_parse.h"
#include "studio/tclock.h"

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

// --- a recording sink + MIDI out -------------------------------------------------

typedef struct {
    char kind;   // S start, C cont, P pulse, X stop, L locate
    int64_t t;
    uint32_t pos;
} call_t;

static call_t calls[20000];
static int ncalls;
static int64_t last_p16;

static void rec(char k, int64_t t, uint32_t pos) {
    if (ncalls < (int)(sizeof calls / sizeof calls[0])) calls[ncalls++] = (call_t){k, t, pos};
}
static void s_start(void *x, int64_t t) { (void)x; rec('S', t, 0); }
static void s_cont(void *x, int64_t t) { (void)x; rec('C', t, 0); }
static void s_stop(void *x, int64_t t) { (void)x; rec('X', t, 0); }
static void s_locate(void *x, int64_t t, uint32_t p) { (void)x; rec('L', t, p); }
static void s_pulse(void *x, int64_t t, uint32_t p) { (void)x; rec('P', t, p); }
static void s_tempo(void *x, int64_t p16, int32_t b) { (void)x; (void)b; last_p16 = p16; }

static const tclock_sink_t SINK = {NULL, s_start, s_cont, s_stop, s_locate, s_pulse, s_tempo};

static uint8_t outb[40000];
static int64_t outt[40000];
static int nout;
static int64_t cur_now;
static void out_fn(void *x, const uint8_t *b, int n) {
    (void)x;
    for (int i = 0; i < n && nout < 40000; i++) {
        outt[nout] = cur_now;
        outb[nout++] = b[i];
    }
}

static void reset_rec(void) {
    ncalls = 0;
    nout = 0;
}

// run the clock like the RT task does: wake at each due time
static void run_until(tclock_t *c, int64_t end) {
    for (;;) {
        int64_t due = tclock_next_due(c);
        if (due > end) break;
        cur_now = due;
        tclock_advance(c, due);
    }
    cur_now = end;
}

// --- parser -------------------------------------------------------------------------

static void test_parser(void) {
    midi_parser_t p;
    mp_init(&p);
    // note on, running status note on, clock inside, CC, program change,
    // MMC play, MMC locate 0:00:02:00, SPP 130, start/stop/continue
    const uint8_t bytes[] = {0x91, 60, 100, 62, 0xF8, 90, 0xB1, 7, 99, 0xC1, 5,
                             0xF0, 0x7F, 0x7F, 0x06, 0x02, 0xF7,
                             0xF0, 0x7F, 0x7F, 0x06, 0x44, 0x06, 0x01, 0x20, 0, 2, 0, 0, 0xF7,
                             0xF2, 0x02, 0x01, 0xFA, 0xFC, 0xFB,
                             0xF0, 1, 2, 3, 0xF7};   // other sysex: ignored
    int notes = 0, cc = 0, pc = 0, clocks = 0, mmc_play = 0, mmc_loc = -1, spp = -1, rt = 0;
    int note2_ok = 0;
    for (unsigned i = 0; i < sizeof bytes; i++) {
        mp_ev_t ev = mp_byte(&p, bytes[i]);
        switch (ev.type) {
        case MP_CHANNEL:
            if ((ev.b[0] & 0xF0) == 0x90) {
                notes++;
                if (notes == 2 && ev.b[1] == 62 && ev.b[2] == 90 && ev.len == 3) note2_ok = 1;
            }
            if ((ev.b[0] & 0xF0) == 0xB0 && ev.b[1] == 7 && ev.b[2] == 99) cc++;
            if ((ev.b[0] & 0xF0) == 0xC0 && ev.b[1] == 5 && ev.len == 2) pc++;
            break;
        case MP_CLOCK: clocks++; break;
        case MP_MMC:
            if (ev.d[0] == MMC_PLAY) mmc_play++;
            if (ev.d[0] == MMC_LOCATE) mmc_loc = ev.d[1] * 3600 + ev.d[2] * 60 + ev.d[3];
            break;
        case MP_SPP: spp = ev.arg; break;
        case MP_START: case MP_STOP: case MP_CONTINUE: rt++; break;
        }
    }
    CHECK(notes == 2 && note2_ok, "running status notes %d ok %d", notes, note2_ok);
    CHECK(cc == 1 && pc == 1, "cc %d pc %d", cc, pc);
    CHECK(clocks == 1, "clocks %d", clocks);
    CHECK(mmc_play == 1 && mmc_loc == 2, "mmc play %d locate %d", mmc_play, mmc_loc);
    CHECK(spp == 130 && rt == 3, "spp %d rt %d", spp, rt);
    printf("midi parser: running status, CC/PC, MMC play + locate, SPP, real-time: ok\n");
}

// --- internal clock -----------------------------------------------------------------

static void test_internal(void) {
    tclock_t c;
    tclock_init(&c, 12000);
    tclock_add_sink(&c, &SINK);
    tclock_set_out(&c, out_fn, NULL);
    tclock_set_clock_out(&c, true, 0);
    reset_rec();
    cur_now = 1000;
    tclock_play(&c, 1000);
    // the timeline knows about the downbeat before it happens
    CHECK(c.tl.running && c.tl.ref_t == 1000 + TC_START_LEAD_US && c.tl.ref_pos == 0,
          "pending downbeat published");
    run_until(&c, 1000 + TC_START_LEAD_US + 125000 * 64 + 10);
    int64_t t0 = 1000 + TC_START_LEAD_US;
    CHECK(ncalls == 65 && calls[0].kind == 'S' && calls[0].t == t0, "start %d", ncalls);
    int bad = 0;
    for (int i = 1; i < ncalls; i++)
        if (calls[i].kind != 'P' || calls[i].pos != (uint32_t)i || calls[i].t != t0 + 125000LL * i)
            bad++;
    CHECK(!bad, "%d pulses off the grid", bad);
    // MIDI out: SPP 0, Start, then 6 clocks per 1/16 exactly on the grid
    CHECK(outb[0] == 0xF2 && outb[3] == 0xFA, "spp + start first");
    int clocks = 0, offgrid = 0;
    for (int i = 4; i < nout; i++) {
        if (outb[i] != 0xF8) continue;
        int64_t want = t0 + (int64_t)clocks * 125000 / 6;
        if (outt[i] != want) offgrid++;
        clocks++;
    }
    CHECK(clocks == 64 * 6 + 1 && !offgrid, "clock out %d, %d off grid", clocks, offgrid);
    // the timeline agrees with the pulses
    tclock_tl_t tl = c.tl;
    CHECK(tclock_tl_pos(&tl, t0 + 125000LL * 64) == 64 * TC_U16, "tl pos");
    CHECK(tclock_tl_pos(&tl, t0 + 125000LL * 64 + 62500) == 64 * TC_U16 + TC_U16 / 2, "tl half");
    CHECK(tclock_tl_time(&tl, 65LL * TC_U16) == t0 + 125000LL * 65, "tl time");

    // tempo change half way through a 1/16: position stays continuous
    int64_t now = t0 + 125000LL * 64 + 62500;
    int64_t before = tclock_tl_pos(&c.tl, now);
    cur_now = now;
    tclock_set_bpm(&c, 15000, now);   // 150 BPM: 1/16 = 100 ms
    int64_t after = tclock_tl_pos(&c.tl, now);
    CHECK(llabs(after - before) <= 1, "position jump %lld -> %lld", (long long)before,
          (long long)after);
    CHECK(last_p16 == 100000, "sinks told the new tempo (%lld)", (long long)last_p16);
    reset_rec();
    run_until(&c, now + 50000 + 100000 * 3 + 10);
    CHECK(ncalls == 4 && calls[0].pos == 65 && calls[0].t == now + 50000 &&
              calls[1].t == now + 150000,
          "pulses after tempo change: n %d first %lld", ncalls, (long long)calls[0].t);

    // stop: sinks stopped, MIDI Stop + MMC sent, no more pulses
    reset_rec();
    cur_now = now + 400000;
    tclock_stop(&c, now + 400000);
    CHECK(ncalls == 1 && calls[0].kind == 'X' && !c.tl.running, "stop");
    CHECK(nout >= 1 && outb[0] == 0xFC, "MIDI stop out");
    CHECK(tclock_next_due(&c) == INT64_MAX, "idle after stop");

    // a stall of 1 s: resync instead of a burst
    tclock_play(&c, 0);
    run_until(&c, TC_START_LEAD_US + 10);
    reset_rec();
    cur_now = 2000000;
    tclock_advance(&c, 2000000);
    CHECK(ncalls == 1 && calls[0].t == 2000000 && c.resyncs == 1, "resync: %d calls", ncalls);
    printf("internal clock: exact grid, clock out, timeline, continuous tempo change, resync: ok\n");
}

// --- MIDI clock -------------------------------------------------------------------------

static void midi(tclock_t *c, int type, uint16_t arg, int64_t t) {
    cur_now = t;
    tclock_midi(c, type, arg, NULL, t);
}

static void test_external(void) {
    tclock_t c;
    tclock_init(&c, 12000);
    tclock_add_sink(&c, &SINK);
    tclock_set_ext(&c, true, 0);
    reset_rec();
    const int64_t clk = 20833;   // 120 BPM
    int64_t t = 1000000;
    // clock running before start: tempo known, nothing plays
    for (int i = 0; i < 24; i++, t += clk) midi(&c, MP_CLOCK, 0, t);
    CHECK(ncalls == 0 && !c.tl.running, "no play before start");
    CHECK(tclock_ext_bpm_x10(&c) >= 1199 && tclock_ext_bpm_x10(&c) <= 1201, "bpm %d",
          tclock_ext_bpm_x10(&c));
    midi(&c, MP_START, 0, t - 500);
    CHECK(c.tl.armed, "armed after start");
    int64_t down = t;
    for (int i = 0; i < 6 * 16 + 1; i++, t += clk) midi(&c, MP_CLOCK, 0, t);
    CHECK(calls[0].kind == 'S' && calls[0].t == down, "downbeat on the first clock");
    int pulses = 0, bad = 0;
    for (int i = 1; i < ncalls; i++) {
        if (calls[i].kind != 'P') continue;
        pulses++;
        if (calls[i].t != down + clk * 6 * pulses || calls[i].pos != (uint32_t)pulses) bad++;
    }
    CHECK(pulses == 16 && !bad, "pulses %d bad %d", pulses, bad);
    // timeline: last clock is at 16*1680 units; extrapolation stops 2 1/16s ahead
    CHECK(c.tl.ref_pos == 16 * TC_U16 && c.tl.ref_t == down + clk * 96, "ext tl ref");
    CHECK(tclock_tl_pos(&c.tl, c.tl.ref_t + 10000000) == 18 * TC_U16, "ext tl clamp");
    int64_t mid = tclock_tl_pos(&c.tl, c.tl.ref_t + clk * 3);
    CHECK(llabs(mid - (16 * TC_U16 + TC_U16 / 2)) < 4, "ext extrapolation %lld", (long long)mid);

    // stop, song position 32 (bar 3), continue: next clock is 1/16 #32
    reset_rec();
    midi(&c, MP_STOP, 0, t);
    t += 100000;
    midi(&c, MP_SPP, 32, t);
    midi(&c, MP_CONTINUE, 0, t + 100);
    CHECK(ncalls == 3 && calls[0].kind == 'X' && calls[1].kind == 'L' && calls[1].pos == 32 &&
              calls[2].kind == 'C',
          "stop/locate/continue %d", ncalls);
    CHECK(tclock_tl_pos(&c.tl, t + 50000) < 32 * TC_U16, "nothing ahead of the master");
    t += 1000;
    midi(&c, MP_CLOCK, 0, t);
    CHECK(ncalls == 4 && calls[3].kind == 'P' && calls[3].pos == 32, "pulse 32 first");
    CHECK(c.tl.ref_pos == 32 * TC_U16, "tl at 32");

    // MMC stop / play from the top
    reset_rec();
    uint8_t stop[1] = {MMC_STOP}, play[1] = {MMC_PLAY};
    tclock_midi(&c, MP_MMC, 0, stop, t + 10);
    tclock_midi(&c, MP_SPP, 0, NULL, t + 20);
    tclock_midi(&c, MP_MMC, 0, play, t + 30);
    CHECK(c.armed && !c.running, "MMC play from the top arms");
    midi(&c, MP_CLOCK, 0, t + clk);
    CHECK(c.running && calls[ncalls - 1].kind == 'S', "MMC: start on the next clock");

    // internal clock ignores MIDI transport
    tclock_set_ext(&c, false, t + 2 * clk);
    reset_rec();
    midi(&c, MP_START, 0, t + 3 * clk);
    midi(&c, MP_CLOCK, 0, t + 4 * clk);
    CHECK(ncalls == 0 && !c.running, "internal ignores MIDI");
    printf("MIDI clock: tempo, start on first clock, hard pulses, SPP/continue, MMC, clamp: ok\n");
}

int main(void) {
    test_parser();
    test_internal();
    test_external();
    if (failures) {
        printf("%d FAILURES\n", failures);
        return 1;
    }
    printf("studio: all tests passed\n");
    return 0;
}
