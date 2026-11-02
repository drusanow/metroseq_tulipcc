// test_wavetron.c - host tests for Wavetron's engine, tables, WAV loader,
// effects and patch files.
//   make -C tests test
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wt_dsp.h"
#include "wt_patch.h"

static int failures;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

#define SR 44100
static wt_catalog_t cat;
static wt_engine_t e;
static float L[SR * 4], R[SR * 4];

static void render(int n) {
    memset(L, 0, n * sizeof(float));
    memset(R, 0, n * sizeof(float));
    for (int i = 0; i < n; i += 48) wt_render(&e, L + i, R + i, n - i < 48 ? n - i : 48);
}

static bool all_finite(int n) {
    for (int i = 0; i < n; i++)
        if (!isfinite(L[i]) || !isfinite(R[i])) return false;
    return true;
}

static float peak(int a, int b) {
    float p = 0;
    for (int i = a; i < b; i++) p = fmaxf(p, fmaxf(fabsf(L[i]), fabsf(R[i])));
    return p;
}

static float rms(const float *x, int a, int b) {
    double s = 0;
    for (int i = a; i < b; i++) s += x[i] * x[i];
    return (float)sqrt(s / (b - a));
}

// a plain test sound: OSC A only, the SAW table at position 0 (a sine),
// no filter, no effects, no modulation, an organ envelope
static wt_sound_t plain(void) {
    wt_sound_t s;
    wt_sound_default(&s);
    wt_mx_clear(&s);
    s.p[WP_VMODE] = VM_MONO;
    s.p[WP_A_POS] = 0;
    s.p[WP_B_LVL] = 0;
    s.p[WP_FTYPE] = FT_OFF;
    s.p[WP_RV_LVL] = 0;
    s.p[WP_UNI_DRIFT] = 0;
    s.p[WP_AA] = 0;
    s.p[WP_AS] = 1;
    s.p[WP_AR] = 5;
    s.p[WP_VSENS] = 0;
    return s;
}

static void reset(const wt_sound_t *s) {
    wt_panic(&e);
    wt_set_sound(&e, s);
    wt_set_tables(&e, &cat.t[0].w, &cat.t[1].w);
}

static float freq_of(const float *x, int a, int b) {
    int n = 0, first = -1, last = -1;
    for (int i = a + 1; i < b; i++)
        if (x[i - 1] < 0 && x[i] >= 0) {
            if (first < 0) first = i;
            last = i;
            n++;
        }
    return n > 1 ? (float)(n - 1) * SR / (last - first) : 0;
}

// --- tests -------------------------------------------------------------------------------

static void test_tables(void) {
    CHECK(cat.n == WT_NBUILTIN, "%d built-ins", cat.n);
    for (int k = 0; k < WT_NBUILTIN; k++) {
        CHECK(cat.t[k].w.ncyc == 16, "%s: %d cycles", cat.t[k].name, cat.t[k].w.ncyc);
        // every cycle normalised to 0.95 (the sketch's _wt_norm_cycle)
        for (int c = 0; c < 16; c++) {
            int pk = 0;
            for (int i = 0; i < WT_CYCLE; i++) pk = abs(cat.t[k].w.data[c * WT_CYCLE + i]) > pk ? abs(cat.t[k].w.data[c * WT_CYCLE + i]) : pk;
            CHECK(abs(pk - 31129) < 40, "%s cycle %d peak %d", cat.t[k].name, c, pk);
        }
    }
    // SAW: a sine at position 0, a saw at 1
    const wt_wave_t *w = &cat.t[0].w;
    CHECK(fabsf(wt_read(w, 0.25f, 0) - 0.95f) < 0.01f, "SAW pos 0 = sine (%f)", wt_read(w, 0.25f, 0));
    CHECK(fabsf(wt_read(w, 0.75f, 0) + 0.95f) < 0.01f, "SAW pos 0 trough");
    CHECK(wt_read(w, 0.99f, 1) > 0.85f, "SAW pos 1 ends near the top (%f)", wt_read(w, 0.99f, 1));
    // position crossfades between cycles
    float mid = wt_read(w, 0.1f, 0.5f), lo = wt_read(w, 0.1f, 7.0f / 15), hi = wt_read(w, 0.1f, 8.0f / 15);
    CHECK(mid >= fminf(lo, hi) - 1e-3f && mid <= fmaxf(lo, hi) + 1e-3f, "crossfade");
    printf("tables: 6 built-ins x 16 cycles, normalised, crossfading: ok\n");
}

static void test_pitch(void) {
    wt_sound_t s = plain();
    reset(&s);
    wt_note_on(&e, 0, 69, 69, 1, 0);
    render(SR + SR / 4);
    float f = freq_of(L, SR / 4, SR + SR / 4);
    CHECK(fabsf(f - 440) < 0.5f, "A4 at %.2f Hz", f);
    CHECK(all_finite(SR), "finite");
    // coarse +12, fine +50c
    s.p[WP_A_COARSE] = 12;
    s.p[WP_A_FINE] = 50;
    reset(&s);
    wt_note_on(&e, 0, 69, 69, 1, 0);
    render(SR);
    float f2 = freq_of(L, SR / 4, SR);
    float want = 880 * powf(2, 0.5f / 12);
    CHECK(fabsf(f2 - want) < 1, "coarse + fine: %.2f (want %.2f)", f2, want);
    // pitch bend: +2 semitones at full wheel
    s.p[WP_A_COARSE] = 0;
    s.p[WP_A_FINE] = 0;
    reset(&s);
    e.bend = 1;
    wt_note_on(&e, 0, 69, 69, 1, 0);
    render(SR);
    float f3 = freq_of(L, SR / 4, SR);
    CHECK(fabsf(f3 - 440 * powf(2, 2.0f / 12)) < 1, "bend: %.2f", f3);
    e.bend = 0;
    printf("pitch: A4 %.2f Hz, coarse/fine %.2f, bend %.2f: ok\n", f, f2, f3);
}

static void test_env(void) {
    wt_sound_t s = plain();
    s.p[WP_AA] = 100;
    s.p[WP_ACURVE] = EC_LIN;
    s.p[WP_AR] = 200;
    reset(&s);
    wt_note_on(&e, 0, 60, 60, 1, 0);
    render(SR / 2);
    float a50 = peak(SR * 45 / 1000, SR * 55 / 1000), a150 = peak(SR * 140 / 1000, SR * 160 / 1000);
    CHECK(a50 > 0.35f * a150 && a50 < 0.65f * a150, "LIN attack half way at 50 ms (%.3f of %.3f)", a50, a150);
    wt_note_off(&e, 0, 60);
    render(SR / 2);
    float r100 = peak(SR * 90 / 1000, SR * 110 / 1000), r300 = peak(SR * 290 / 1000, SR / 2);
    CHECK(r100 > 0.3f * a150 && r100 < 0.7f * a150, "LIN release half way (%.3f)", r100);
    CHECK(r300 < 1e-3f, "silent after the release (%.5f)", r300);
    bool idle = true;
    for (int v = 0; v < WT_NV; v++) idle &= !e.v[v].active;
    CHECK(idle, "voices idle after release");
    // each curve reaches its sustain and releases to silence
    for (int c = 0; c < 4; c++) {
        s.p[WP_ACURVE] = c;
        s.p[WP_AA] = 50;
        s.p[WP_AD] = 100;
        s.p[WP_AS] = 0.5f;
        reset(&s);
        wt_note_on(&e, 0, 60, 60, 1, 0);
        render(SR / 2);
        float sus = e.v[0].amp.v;
        CHECK(fabsf(sus - 0.5f) < 0.02f, "%s: sustain %.3f", WT_EG_CURVES[c], sus);
        wt_note_off(&e, 0, 60);
        render(SR / 2);
        CHECK(!e.v[0].active, "%s: released", WT_EG_CURVES[c]);
    }
    printf("envelopes: LIN timing, 4 curves to sustain and release: ok\n");
}

static int sounding(void) {
    int n = 0;
    for (int v = 0; v < WT_NV; v++) n += e.v[v].gate;
    return n;
}

static void test_modes(void) {
    // POLY: 4 notes, the 5th steals the oldest
    wt_sound_t s = plain();
    s.p[WP_VMODE] = VM_POLY;
    reset(&s);
    for (int k = 0; k < 4; k++) wt_note_on(&e, 0, 60 + k, 60 + k, 1, 0), render(480);
    CHECK(sounding() == 4, "poly: 4 voices (%d)", sounding());
    wt_note_on(&e, 0, 70, 70, 1, 0);
    bool has60 = false, has70 = false;
    for (int v = 0; v < WT_NV; v++) {
        has60 |= e.v[v].gate && e.v[v].key == 60;
        has70 |= e.v[v].gate && e.v[v].key == 70;
    }
    CHECK(!has60 && has70, "poly: the oldest stolen");
    wt_note_off(&e, 0, 61);
    CHECK(sounding() == 3, "poly: note off frees one");

    // MONO: legato, last-note priority, back to the held note
    s.p[WP_VMODE] = VM_MONO;
    reset(&s);
    wt_note_on(&e, 0, 60, 60, 1, 0);
    render(4800);
    wt_note_on(&e, 0, 64, 64, 1, 0);
    CHECK(e.v[0].amp.st != 1, "mono: legato, no retrigger");
    CHECK(e.v[0].target == 64 && sounding() == 1, "mono: last note");
    wt_note_off(&e, 0, 64);
    CHECK(e.v[0].target == 60 && e.v[0].gate, "mono: back to the held note");
    wt_note_off(&e, 0, 60);
    CHECK(!e.v[0].gate, "mono: off");

    // glide: halfway-ish after a third of the time
    s.p[WP_GLIDE] = 300;
    reset(&s);
    wt_note_on(&e, 0, 60, 60, 1, 0);
    render(4800);
    wt_note_off(&e, 0, 60);
    wt_note_on(&e, 0, 72, 72, 1, 0);
    render(SR / 10);
    CHECK(e.v[0].pitch > 62 && e.v[0].pitch < 71, "glide under way (%.2f)", e.v[0].pitch);
    render(SR);
    CHECK(fabsf(e.v[0].pitch - 72) < 0.05f, "glide arrives");

    // UNISON: all 4 voices, detuned, spread in stereo
    s = plain();
    s.p[WP_VMODE] = VM_UNISON;
    s.p[WP_UNI_DET] = 20;
    s.p[WP_UNI_WIDTH] = 1;
    reset(&s);
    wt_note_on(&e, 0, 60, 60, 1, 0);
    render(SR);
    CHECK(sounding() == 4, "unison: 4 voices");
    float d = 0;
    for (int i = SR / 2; i < SR; i++) d += fabsf(L[i] - R[i]);
    CHECK(d / (SR / 2) > 0.01f, "unison: stereo (%.4f)", d / (SR / 2));
    printf("voices: POLY steal, MONO legato + fallback, glide, UNISON 4 wide: ok\n");
}

static void test_filter_mod(void) {
    // LP24 at 300 Hz takes most of a bright saw away
    wt_sound_t s = plain();
    s.p[WP_A_POS] = 1;
    s.p[WP_FKBD] = 0;
    s.p[WP_FVEL] = 0;
    s.p[WP_FENV] = 0;
    reset(&s);
    wt_note_on(&e, 0, 48, 48, 1, 0);
    render(SR / 2);
    float open = rms(L, SR / 4, SR / 2);
    int types[] = {FT_LP12, FT_LP24, FT_HP, FT_BP, FT_NOTCH, FT_PHASER};
    for (int k = 0; k < 6; k++) {
        s.p[WP_FTYPE] = types[k];
        s.p[WP_CUTOFF] = 300;
        s.p[WP_RESO] = 4;
        reset(&s);
        wt_note_on(&e, 0, 48, 48, 1, 0);
        render(SR / 2);
        CHECK(all_finite(SR / 2), "%s finite", WT_FILTERS[types[k]]);
        CHECK(peak(0, SR / 2) < 3, "%s level %.2f", WT_FILTERS[types[k]], peak(0, SR / 2));
    }
    // without resonance, LP24 at 300 Hz keeps little of a 130 Hz saw's top
    s.p[WP_FTYPE] = FT_LP24;
    s.p[WP_RESO] = 0.7f;
    reset(&s);
    wt_note_on(&e, 0, 48, 48, 1, 0);
    render(SR / 2);
    float dark = rms(L, SR / 4, SR / 2);
    CHECK(dark < open * 0.8f, "LP24 darkens (%.3f vs %.3f)", dark, open);
    // MOD1 (an LFO) -> A.POS moves the position
    s = plain();
    s.mx[0][D_A_POS] = 0.4f;
    s.p[WP_A_POS] = 0.5f;
    s.p[WP_M1_RATE] = 5;
    reset(&s);
    wt_note_on(&e, 0, 60, 60, 1, 0);
    float lo = 1, hi = 0;
    for (int i = 0; i < 40; i++) {
        render(SR / 40);
        lo = fminf(lo, e.v[0].pos_a);
        hi = fmaxf(hi, e.v[0].pos_a);
    }
    CHECK(lo < 0.2f && hi > 0.8f, "LFO -> position: %.2f..%.2f", lo, hi);
    // MOD1 in AUDIO mode -> A.PIT: FM at audio rate (exponential, in
    // octaves, as the sketch's coefficient FM). A 220 Hz sine under a
    // 440 Hz operator at +-1 octave spreads into sidebands: its own line
    // loses most of its energy, while the level stays.
    float band[2];
    for (int fm = 0; fm < 2; fm++) {
        s = plain();
        s.p[WP_M1_MODE] = 1;
        s.p[WP_M1_RATIO] = 2;
        s.mx[0][D_A_PIT] = fm ? 1.0f : 0;
        reset(&s);
        wt_note_on(&e, 0, 57, 57, 1, 0);
        render(SR / 2);
        CHECK(all_finite(SR / 2), "FM finite");
        // Goertzel at 220 Hz
        double w = 2 * 3.14159265358979 * 220 / SR, c = 2 * cos(w), s1 = 0, s2 = 0;
        for (int i = SR / 4; i < SR / 2; i++) {
            double s0 = L[i] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        band[fm] = (float)sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / (SR / 4);
        if (fm) CHECK(peak(SR / 4, SR / 2) > 0.05f, "FM keeps its level (%.3f)", peak(SR / 4, SR / 2));
    }
    CHECK(band[1] < 0.3f * band[0], "FM spreads the 220 Hz line: %.4f of %.4f", band[1], band[0]);
    // MOD4 -> CUT is FILTER's ENV AMT
    CHECK(wt_mx_get(&s, 3, D_CUT) == s.p[WP_FENV], "MOD4 -> CUT = fenv");
    printf("filters: 6 types stable, LP24 darkens; LFO -> position; audio-rate FM: ok\n");
}

static void test_fx(void) {
    wt_sound_t s = plain();
    s.p[WP_AR] = 5;
    s.p[WP_EC_LVL] = 0.5f;
    s.p[WP_EC_MS] = 250;
    s.p[WP_EC_FB] = 0;
    reset(&s);
    wt_note_on(&e, 0, 60, 60, 1, 0);
    render(SR / 20);
    wt_note_off(&e, 0, 60);
    render(SR);
    float echo = peak(SR * 250 / 1000 - 2000, SR * 250 / 1000 + 2000);
    float gap = peak(SR * 120 / 1000, SR * 180 / 1000);
    CHECK(echo > 0.02f && gap < 0.01f, "echo at 250 ms (%.3f, gap %.4f)", echo, gap);
    s.p[WP_EC_LVL] = 0;
    s.p[WP_RV_LVL] = 0.5f;
    reset(&s);
    wt_note_on(&e, 0, 60, 60, 1, 0);
    render(SR / 10);
    wt_note_off(&e, 0, 60);
    render(SR);
    CHECK(peak(SR / 2, SR) > 1e-3f, "reverb tail (%.4f)", peak(SR / 2, SR));
    wt_panic(&e);
    render(SR / 10);
    CHECK(peak(0, SR / 10) == 0, "panic silences the tail");
    // everything at once stays finite and in range
    s = plain();
    s.p[WP_VMODE] = VM_UNISON;
    s.p[WP_D_DRIVE] = 8, s.p[WP_D_FOLD] = 1, s.p[WP_D_BITS] = 6, s.p[WP_D_RATE] = 4;
    s.p[WP_EQ_L] = 10, s.p[WP_EQ_H] = -10;
    s.p[WP_CH_LVL] = 0.7f, s.p[WP_EC_LVL] = 0.5f, s.p[WP_EC_FB] = 0.9f, s.p[WP_RV_LVL] = 1;
    s.p[WP_RV_LIVE] = 1;
    reset(&s);
    wt_note_on(&e, 0, 60, 60, 1, 0);
    render(SR * 3);
    CHECK(all_finite(SR * 3), "all effects: finite");
    // drive 8 into +10 dB of EQ is loud by design (Studio's limiter is
    // after it); what matters is that the feedback paths settle
    CHECK(peak(0, SR * 3) < 8, "all effects: bounded (%.2f)", peak(0, SR * 3));
    CHECK(peak(SR * 2, SR * 3) <= peak(0, SR) * 1.5f, "all effects: no runaway (%.2f then %.2f)",
          peak(0, SR), peak(SR * 2, SR * 3));
    // silence: once every tail is gone, Wavetron stops rendering (and a
    // new note brings it straight back)
    s = plain();
    s.p[WP_RV_LVL] = 0.3f;
    reset(&s);
    wt_note_on(&e, 0, 60, 60, 1, 0);
    render(SR / 10);
    wt_note_off(&e, 0, 60);
    render(SR * 4);
    CHECK(e.quiet >= SR / 4, "idle after the tail (%u)", e.quiet);
    render(SR / 10);
    CHECK(peak(0, SR / 10) == 0, "idle: nothing added");
    wt_note_on(&e, 0, 60, 60, 1, 0);
    render(SR / 10);
    CHECK(peak(SR / 20, SR / 10) > 0.05f && e.quiet == 0, "a note wakes it");
    wt_note_off(&e, 0, 60);
    printf("effects: echo time, reverb tail, panic, everything at once, idle when silent: ok\n");
}

// --- WAV files -----------------------------------------------------------------------------

static void put16(FILE *f, int v) { fputc(v & 255, f), fputc(v >> 8 & 255, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xFFFF), put16(f, v >> 16); }

static void write_wav(const char *path, int fmt, int nch, int bits, int frames, bool ext) {
    FILE *f = fopen(path, "wb");
    int bytes = bits / 8, data = frames * nch * bytes;
    int fmtlen = ext ? 40 : 16;
    fwrite("RIFF", 1, 4, f);
    put32(f, 4 + 8 + fmtlen + 8 + 12 + 8 + data);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    put32(f, fmtlen);
    put16(f, ext ? 0xFFFE : fmt);
    put16(f, nch);
    put32(f, 44100);
    put32(f, 44100 * nch * bytes);
    put16(f, nch * bytes);
    put16(f, bits);
    if (ext) {
        put16(f, 22), put16(f, bits), put32(f, 3);
        put16(f, fmt);   // the SubFormat GUID's first word
        for (int i = 0; i < 14; i++) fputc(0, f);
    }
    fwrite("LIST", 1, 4, f);   // a chunk to skip
    put32(f, 4);
    fwrite("INFO", 1, 4, f);
    fwrite("data", 1, 4, f);
    put32(f, data);
    for (int i = 0; i < frames; i++) {
        float v = sinf(6.2831853f * (i % 256) / 256.0f) * 0.5f;
        for (int c = 0; c < nch; c++) {
            if (fmt == 3) {
                float x = v;
                fwrite(&x, 4, 1, f);
            } else if (bits == 8) {
                fputc(128 + (int)(v * 127), f);
            } else if (bits == 16) {
                put16(f, (int)(v * 32767));
            } else if (bits == 24) {
                int x = (int)(v * 8388607);
                fputc(x & 255, f), fputc(x >> 8 & 255, f), fputc(x >> 16 & 255, f);
            } else {
                put32(f, (uint32_t)(int32_t)(v * 2147483647.0));
            }
        }
    }
    fclose(f);
}

static void test_wav(void) {
    struct {
        int fmt, nch, bits;
        bool ext;
    } cases[] = {{1, 1, 8, false},  {1, 1, 16, false}, {1, 2, 16, false}, {1, 1, 24, false},
                 {1, 2, 24, true},  {1, 1, 32, false}, {3, 1, 32, false}, {3, 2, 32, true}};
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        write_wav("build/t.wav", cases[k].fmt, cases[k].nch, cases[k].bits, 256 * 8 + 100, cases[k].ext);
        int16_t *d;
        int n;
        char err[40] = "";
        bool ok = wt_wav_decode("build/t.wav", &d, &n, err, sizeof err);
        CHECK(ok, "fmt %d %dch %d-bit%s: %s", cases[k].fmt, cases[k].nch, cases[k].bits,
              cases[k].ext ? " ext" : "", err);
        if (!ok) continue;
        CHECK(n == 256 * 8, "trimmed to whole cycles (%d)", n);
        CHECK(abs(d[64] - 16383) < 300, "sample value %d", d[64]);
        free(d);
    }
    int16_t *d;
    int n;
    char err[40];
    write_wav("build/t.wav", 1, 1, 16, 300, false);
    CHECK(!wt_wav_decode("build/t.wav", &d, &n, err, sizeof err) && strstr(err, "short"), "short: %s", err);
    FILE *f = fopen("build/t.wav", "wb");
    fputs("not a wave file at all", f);
    fclose(f);
    CHECK(!wt_wav_decode("build/t.wav", &d, &n, err, sizeof err) && !strcmp(err, "not a WAV"), "%s", err);
    CHECK(!wt_wav_decode("build/missing.wav", &d, &n, err, sizeof err), "missing file");

    // the catalogue: load, find again, evict the least recently used
    wt_catalog_t c;
    wt_cat_init(&c);
    char path[32];
    int idx[6];
    for (int k = 0; k < 6; k++) {
        snprintf(path, sizeof path, "build/t%d.wav", k);
        write_wav(path, 1, 1, 16, 256 * (2 + k), false);
        idx[k] = wt_cat_load(&c, path, -1, -1, err, sizeof err);
        CHECK(idx[k] >= WT_NBUILTIN, "load %d: %s", k, err);
    }
    CHECK(c.n == WT_CAT_MAX, "4 file slots");
    CHECK(wt_cat_find(&c, "build/t0.wav") < 0 && wt_cat_find(&c, "build/t1.wav") < 0, "LRU evicted");
    CHECK(wt_cat_find(&c, "build/t5.wav") == idx[5] && c.t[idx[5]].w.ncyc == 7, "found, 7 cycles");
    CHECK(wt_cat_load(&c, "build/t5.wav", -1, -1, err, sizeof err) == idx[5], "loaded once");
    wt_cat_free(&c);
    printf("WAV: 8/16/24/32-bit, float, stereo, extensible; short / not-a-WAV refused; LRU: ok\n");
}

// --- patches --------------------------------------------------------------------------------

static void test_patch(void) {
    // a record exactly as the sketch writes it (keys it has that we don't,
    // keys we have that it lacks, MOD4 -> CUT as "3:cut")
    const char *py =
        "{\"p\": {\"a_wt\": 2, \"b_wt\": 4, \"a_pos\": 0.6, \"cutoff\": 900.0, \"vmode\": 2,"
        " \"tempo\": 120.0, \"cv1_src\": 0, \"panic\": 0, \"m1_shape\": 5},"
        " \"mx\": {\"0:a_pos\": 0.5, \"3:cut\": 2.0, \"2:a_pit\": -1.0, \"9:cut\": 1, \"1:nope\": 1},"
        " \"a_wt_path\": null, \"b_wt_path\": \"/sd/tables/Growl.wav\"}";
    cJSON *rec = cJSON_Parse(py);
    wt_patch_t p;
    wt_patch_from_json(rec, &p);
    cJSON_Delete(rec);
    CHECK(p.a_builtin == 2 && !p.a_path[0], "OSC A: built-in HARM");
    CHECK(!strcmp(p.b_path, "/sd/tables/Growl.wav"), "OSC B by path");
    CHECK(fabsf(p.s.p[WP_A_POS] - 0.6f) < 1e-6f && p.s.p[WP_CUTOFF] == 900, "values");
    CHECK(p.s.p[WP_VMODE] == VM_POLY && p.s.p[WP_M1_SHAPE] == LS_SH, "enums");
    CHECK(p.s.p[WP_RESO] == WT_PARAMS[WP_RESO].def, "missing key -> default");
    CHECK(p.s.mx[0][D_A_POS] == 0.5f && p.s.mx[2][D_A_PIT] == -1, "matrix");
    CHECK(p.s.p[WP_FENV] == 2, "3:cut -> ENV AMT");
    CHECK(p.s.mx[1][D_A_POS] == 0 && p.s.mx[0][D_B_POS] == 0, "only what the patch has");
    // round trip
    cJSON *j = wt_patch_to_json(&p);
    wt_patch_t q;
    wt_patch_from_json(j, &q);
    cJSON_Delete(j);
    CHECK(!memcmp(&p.s, &q.s, sizeof p.s) && !strcmp(p.b_path, q.b_path) && q.a_builtin == 2, "round trip");
    // out of range values are clamped, NaN takes the default
    rec = cJSON_Parse("{\"p\": {\"reso\": 99, \"glide\": -5, \"ftype\": 3.4}}");
    wt_patch_from_json(rec, &q);
    cJSON_Delete(rec);
    CHECK(q.s.p[WP_RESO] == 16 && q.s.p[WP_GLIDE] == 0 && q.s.p[WP_FTYPE] == 3, "clamped / snapped");

    // the library file
    cJSON *lib = wt_lib_read("build/missing.json");
    CHECK(wt_lib_count(lib) == 0, "no file: empty");
    CHECK(wt_lib_put(lib, "  PAD  ", &p) && !strcmp(wt_lib_name(lib, 0), "PAD"), "trimmed name");
    CHECK(wt_lib_put(lib, "A VERY LONG NAME INDEED", &p) && strlen(wt_lib_name(lib, 1)) <= 12, "12 chars");
    CHECK(wt_lib_put(lib, "PAD", &q) && wt_lib_count(lib) == 2, "replace by name");
    CHECK(!wt_lib_put(lib, "   ", &p), "empty name refused");
    CHECK(!wt_lib_rename(lib, 1, "PAD") && wt_lib_rename(lib, 1, "LEAD"), "rename");
    for (int k = 0; k < 30; k++) {
        char nm[16];
        snprintf(nm, sizeof nm, "P%d", k);
        wt_lib_put(lib, nm, &p);
    }
    CHECK(wt_lib_count(lib) == WT_MAX_PATCHES, "24 at most");
    CHECK(wt_lib_write("build/lib.json", lib), "write");
    cJSON *back = wt_lib_read("build/lib.json");
    CHECK(wt_lib_count(back) == WT_MAX_PATCHES && wt_lib_find(back, "LEAD") == 1, "read back");
    wt_patch_t r;
    wt_patch_from_json(wt_lib_rec(back, 0), &r);
    CHECK(r.s.p[WP_RESO] == 16, "the replaced patch");
    wt_lib_delete(back, 0);
    CHECK(wt_lib_find(back, "LEAD") == 0, "delete");
    cJSON_Delete(lib);
    cJSON_Delete(back);
    printf("patches: the sketch's format, defaults, round trip, library: ok\n");
}

static void test_cpu(void) {
    // the worst case: 4 voices, FM, every filter stage and effect
    wt_sound_t s;
    wt_sound_default(&s);
    s.p[WP_VMODE] = VM_POLY;
    s.p[WP_M1_MODE] = 1;
    s.mx[0][D_A_PIT] = 0.5f;
    s.p[WP_A_DRV] = 4, s.p[WP_B_FOLD] = 1, s.p[WP_FDRV] = 3;
    s.p[WP_CH_LVL] = 0.5f, s.p[WP_EC_LVL] = 0.4f, s.p[WP_RV_LVL] = 0.3f, s.p[WP_EQ_M] = 6;
    reset(&s);
    for (int k = 0; k < 4; k++) wt_note_on(&e, 0, 48 + 7 * k, 48 + 7 * k, 1, 0);
    clock_t c0 = clock();
    render(SR * 2);
    double t = (double)(clock() - c0) / CLOCKS_PER_SEC;
    CHECK(all_finite(SR * 2), "worst case finite");
    printf("cpu: worst case %.0f%% of real time on this host (sanitizers on)\n", t / 2 * 100);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!wt_cat_init(&cat) || !wt_init(&e)) {
        printf("init failed\n");
        return 1;
    }
    test_tables();
    test_pitch();
    test_env();
    test_modes();
    test_filter_mod();
    test_fx();
    test_wav();
    test_patch();
    test_cpu();
    if (failures) {
        printf("%d wavetron test(s) FAILED\n", failures);
        return 1;
    }
    printf("all wavetron tests passed\n");
    return 0;
}
