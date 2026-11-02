// test_sloe.c - host tests for the Sloe synth voices.   make -C tests test
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "sloe_dsp.h"

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

static float run(sl_engine_t *e, int frames, bool *finite) {
    float peak = 0, l[48], r[48];
    for (int i = 0; i < frames; i += 48) {
        memset(l, 0, sizeof l);
        memset(r, 0, sizeof r);
        sl_render(e, l, r, 48);
        for (int j = 0; j < 48; j++) {
            if (!isfinite(l[j])) *finite = false;
            if (fabsf(l[j]) > peak) peak = fabsf(l[j]);
        }
    }
    return peak;
}

static int active(const sl_engine_t *e) {
    int n = 0;
    for (int i = 0; i < SL_VOICES; i++) n += e->v[i].active;
    return n;
}

int main(void) {
    static sl_engine_t e;
    bool finite = true;
    // every preset: sounds, stays finite and in level, lets go after note-off
    for (int p = 0; p < SL_NPRESETS; p++) {
        sl_init(&e);
        e.p = SL_PRESETS[p].p;
        sl_note_on(&e, 0x80, 60, 60, 0.8f, 0);
        sl_note_on(&e, 0x80, 64, 64, 0.8f, 0);
        float pk = run(&e, 44100 / 2, &finite);
        sl_note_off(&e, 0x80, 60);
        sl_note_off(&e, 0x80, 64);
        run(&e, 44100 * 10, &finite);
        CHECK(pk > 0.02f && pk < 1.2f, "%s peak %.3f", SL_PRESETS[p].name, pk);
        CHECK(active(&e) == 0, "%s still sounding after release", SL_PRESETS[p].name);
    }
    CHECK(finite, "non-finite output");
    printf("presets: %d sound, stay in level and release\n", SL_NPRESETS);

    // a routed track (src 0): one voice per track; a slide glides there
    sl_init(&e);
    e.p = SL_PRESETS[0].p;
    sl_note_on(&e, 0, 0, 48, 0.8f, 0);
    run(&e, 4410, &finite);
    sl_note_off(&e, 0, 0);
    sl_note_on(&e, 0, 0, 60, 0.8f, 100000);   // slide over 100 ms
    run(&e, 441, &finite);                     // 10 ms in
    float mid = -1;
    for (int i = 0; i < SL_VOICES; i++)
        if (e.v[i].on) mid = e.v[i].pitch;
    CHECK(mid > 48.5f && mid < 58.0f, "slide 10 ms in: pitch %.2f", mid);
    run(&e, 44100 / 2, &finite);
    float end = -1;
    for (int i = 0; i < SL_VOICES; i++)
        if (e.v[i].on) end = e.v[i].pitch;
    CHECK(fabsf(end - 60) < 0.05f && active(&e) == 1, "slide end %.2f, voices %d", end, active(&e));
    printf("routed track: retriggers its own voice, SLIDE glides 48 -> 60 (%.1f at 10 ms)\n", mid);

    // poly: 8 held notes on 6 voices steal the oldest
    sl_init(&e);
    for (int k = 0; k < 8; k++) sl_note_on(&e, 0x80, (uint8_t)(60 + k), 60.0f + k, 1, 0);
    CHECK(active(&e) == SL_VOICES, "poly voices %d", active(&e));
    bool has67 = false, has60 = false;
    for (int i = 0; i < SL_VOICES; i++) {
        has67 |= e.v[i].key == 67;
        has60 |= e.v[i].key == 60 && e.v[i].on;
    }
    CHECK(has67 && !has60, "voice stealing");
    // ALL OFF from one source
    sl_all_off(&e, 0x80);
    run(&e, 44100 * 5, &finite);
    CHECK(active(&e) == 0, "all off");
    printf("poly: 6 voices, oldest stolen, ALL OFF releases: ok\n");
    printf(failures ? "\n%d FAILURES\n" : "\nall sloe tests passed\n", failures);
    return failures ? 1 : 0;
}
