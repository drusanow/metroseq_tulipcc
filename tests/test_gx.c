// test_gx.c - host tests for the GinTonix drum machine: voices, the
// pattern player on the master clock, and files.   make -C tests test
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "gx_engine.h"
#include "gx_json.h"
#include "gx_seq.h"

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

#define SR 44100

// --- one drum alone -------------------------------------------------------------------

typedef struct {
    float peak, zc_hz;       // loudest sample, zero-crossing rate of the first 30 ms
    int len;                 // samples until the voice stopped
    bool finite;
} drum_stats_t;

static drum_stats_t play_one(const gx_patch_t *p, float vel, float *buf, int max) {
    gx_vp_t vp;
    gx_vp_from_patch(&vp, p, 1.0f);
    gx_voice_t v = {.seed = 12345};
    gx_voice_trigger(&v, &vp, vel);
    drum_stats_t st = {0, 0, max, true};
    for (int i = 0; i < max; i += 48) {
        float l[48] = {0}, r[48] = {0};
        bool live = gx_voice_render(&v, &vp, l, r, 48);
        for (int j = 0; j < 48 && i + j < max; j++) {
            float x = l[j] + r[j];
            buf[i + j] = x;
            if (!isfinite(x)) st.finite = false;
            if (fabsf(x) > st.peak) st.peak = fabsf(x);
        }
        if (!live) {
            st.len = i + 48;
            for (int j = i + 48; j < max; j++) buf[j] = 0;
            break;
        }
    }
    int zc = 0, n = SR * 30 / 1000;
    for (int i = 1; i < n && i < st.len; i++)
        if ((buf[i - 1] < 0) != (buf[i] < 0)) zc++;
    st.zc_hz = zc * (float)SR / (2.0f * n);
    return st;
}

static void test_voices(void) {
    static float buf[SR * 12];
    int bad = 0;
    float kick_zc = 0, hat_zc = 0;
    for (int i = 0; i < GX_NDRUMS; i++) {
        const gx_drum_t *d = &GX_DRUMS[i];
        drum_stats_t s = play_one(&d->p, 1.0f, buf, SR * 12);
        // it must end: attack + longest decay, plus slack
        float longest = gx_attack_ms(d->p.k[K_OSC_ATT]) + gx_decay_ms(d->p.k[K_OSC_DEC]);
        float nl = gx_attack_ms(d->p.k[K_N_ATT]) + gx_decay_ms(d->p.k[K_N_DEC]);
        if (nl > longest) longest = nl;
        int limit = (int)(longest * 1.6f * SR / 1000) + SR / 10;
        if (!s.finite || s.peak < 0.02f || s.peak > 1.6f || s.len > limit) {
            bad++;
            printf("  %s: peak %.3f len %d ms (limit %d) finite %d\n", d->name, s.peak,
                   s.len * 1000 / SR, limit * 1000 / SR, s.finite);
        }
        if (!strcmp(d->name, "808 Kick")) kick_zc = s.zc_hz;
        if (!strcmp(d->name, "Closed Hat")) hat_zc = s.zc_hz;
    }
    CHECK(!bad, "%d drums out of range", bad);
    CHECK(kick_zc > 20 && kick_zc < 250, "808 kick sits at %.0f Hz", kick_zc);
    CHECK(hat_zc > 3000, "closed hat only %.0f Hz", hat_zc);
    printf("voices: %d factory drums finite, in level, finished in time; kick ~%.0f Hz, hat ~%.0f Hz\n",
           GX_NDRUMS, kick_zc, hat_zc);

    // velocity sensitivity: accent louder than normal on a sensitive drum
    const gx_patch_t *snare = gx_drum_by_name("Tonic Snare");
    float pa = play_one(snare, 1.0f, buf, SR).peak, pn = play_one(snare, 0.5f, buf, SR).peak;
    CHECK(pa > pn * 1.15f, "accent %.3f vs normal %.3f", pa, pn);
    // a drum with no velocity sensitivity doesn't care
    gx_patch_t flat = *gx_drum_by_name("Clave");
    flat.k[K_VEL_OSC] = 0;
    float fa = play_one(&flat, 1.0f, buf, SR).peak, fn = play_one(&flat, 0.5f, buf, SR).peak;
    CHECK(fabsf(fa - fn) < 1e-4f, "insensitive drum %.4f vs %.4f", fa, fn);
    printf("velocity: accent %.2f vs normal %.2f (VEL 40%%/60%%); 0%% = same level\n", pa, pn);

    // CPU: render cost of all 8 voices of a busy kit (host, for scale)
    static gx_engine_t e;
    gx_engine_init(&e);
    static gx_state_t st;
    gx_kit_state(&GX_KITS[5], &st);   // Industrial: distortion, long tails
    for (int c = 0; c < GX_NCH; c++) {
        gx_vp_from_patch(&e.vp[c], &st.a[c], 1.0f);
        gx_voice_trigger(&e.v[c], &e.vp[c], 1.0f);
    }
    clock_t c0 = clock();
    float l[48], r[48];
    for (int b = 0; b < 2000; b++) {
        memset(l, 0, sizeof l);
        memset(r, 0, sizeof r);
        if (b % 400 == 0)
            for (int c = 0; c < GX_NCH; c++) gx_voice_trigger(&e.v[c], &e.vp[c], 1.0f);
        gx_engine_render(&e, NULL, 0, l, r, 48);
    }
    double us = (double)(clock() - c0) / CLOCKS_PER_SEC * 1e6 / (2000.0 * 48 * GX_NCH);
    printf("cost: %.3f us per voice-sample on this host (8 voices at 44.1 kHz = %.1f%% of it)\n",
           us, us * GX_NCH * SR / 1e4);
}

static void test_choke(void) {
    static gx_engine_t e;
    gx_engine_init(&e);
    gx_vp_from_patch(&e.vp[2], gx_drum_by_name("Closed Hat"), 1.0f);
    gx_vp_from_patch(&e.vp[3], gx_drum_by_name("Open Hat"), 1.0f);
    float l[48], r[48];
    gx_ev_t open = {0, 3, 1.0f}, closed = {10, 2, 1.0f};
    memset(l, 0, sizeof l);
    memset(r, 0, sizeof r);
    gx_engine_render(&e, &open, 1, l, r, 48);
    for (int b = 0; b < 40; b++) gx_engine_render(&e, NULL, 0, l, r, 48);
    CHECK(e.v[3].active, "open hat should still ring");
    gx_engine_render(&e, &closed, 1, l, r, 48);
    for (int b = 0; b < 5; b++) gx_engine_render(&e, NULL, 0, l, r, 48);   // ~5 ms
    CHECK(!e.v[3].active && e.v[2].active, "choke: open %d closed %d", e.v[3].active,
          e.v[2].active);
    printf("choke: the closed hat cuts the open hat within 5 ms\n");
}

// --- the player on the master clock ---------------------------------------------------

static gx_hit_t hits[256];

// run the player over song positions (from, to] in small windows
static int run(gx_seq_t *q, gx_state_t *st, gx_live_t *lv, tclock_tl_t *tl, int64_t from,
               int64_t to, gx_hit_t *out, int max) {
    int n = 0;
    for (int64_t p = from; p < to && n < max;) {
        p += 300;
        if (p > to) p = to;
        n += gx_seq_run(q, st, lv, tl, p, out + n, max - n);
    }
    return n;
}

static void test_player(void) {
    static gx_state_t st;
    gx_state_init(&st);
    const int64_t U = TC_U16;
    // A: kick on 1 and 9; B: kick on 5; A chains into B
    st.pats[0].trig[0] = 1 | (1 << 8);
    st.pats[1].trig[0] = 1 << 4;
    st.pats[0].chain = 1;
    gx_live_t lv = {.next = -1};
    gx_seq_t q;
    gx_seq_init(&q);
    tclock_tl_t tl = {.epoch = 1, .running = 1, .p16_us = 125000, .max_pos = INT64_MAX};
    int n = run(&q, &st, &lv, &tl, -1, 64 * U - 1, hits, 256);
    // A A? no: A -> B -> A -> B over 4 bars: 0, 8 | 16+4 | 32, 40 | 48+4
    int64_t want[] = {0, 8, 20, 32, 40, 52};
    bool ok = n == 6;
    for (int i = 0; i < n && ok; i++) ok = hits[i].pos == want[i] * U;
    CHECK(ok, "chain A->B: %d hits", n);
    printf("chain A>B over 4 bars: hits on 1/16s 0 8 | 20 | 32 40 | 52: %s\n", ok ? "ok" : "FAIL");

    // queued pattern: switch at the end of the pass, not before
    gx_seq_init(&q);
    st.pats[0].chain = 0;
    tl.epoch = 2;
    n = run(&q, &st, &lv, &tl, -1, 4 * U, hits, 256);   // into bar 1
    lv.next = 1;
    n = run(&q, &st, &lv, &tl, 4 * U, 32 * U, hits, 256);
    CHECK(n == 2 && hits[0].pos == 8 * U && hits[1].pos == 20 * U && lv.next == -1,
          "queued B: %d hits", n);
    printf("pattern queued mid-bar: rest of A, then B from the next bar: ok\n");

    // MIDI Continue from song position 40 (bar 3, 1/16 #9): A's 2nd kick first
    gx_seq_init(&q);
    tl = (tclock_tl_t){.epoch = 7, .running = 1, .ref_pos = 40 * U - 280, .ref_t = 0,
                       .p16_us = 125000, .max_pos = INT64_MAX};
    n = run(&q, &st, &lv, &tl, 40 * U - 281, 48 * U - 1, hits, 256);
    CHECK(n == 1 && hits[0].pos == 40 * U, "continue at 40: %d hits, first %lld", n,
          n ? (long long)hits[0].pos : -1LL);
    printf("Song Position 40 + Continue: next hit exactly on 1/16 #40 (pass-aligned): ok\n");

    // live mute: muted channel skipped from the next step
    gx_seq_init(&q);
    tl = (tclock_tl_t){.epoch = 9, .running = 1, .p16_us = 125000, .max_pos = INT64_MAX};
    st.pats[0].trig[1] = 0xFFFF;
    n = run(&q, &st, &lv, &tl, -1, 4 * U - 1, hits, 256);
    int ch1 = 0;
    for (int i = 0; i < n; i++) ch1 += hits[i].ch == 1;
    lv.mute = 2;
    n = run(&q, &st, &lv, &tl, 4 * U - 1, 16 * U - 1, hits, 256);
    int ch1b = 0;
    for (int i = 0; i < n; i++) ch1b += hits[i].ch == 1;
    CHECK(ch1 == 4 && ch1b == 0, "mute: before %d after %d", ch1, ch1b);
    lv.mute = 0;
    printf("mute: takes effect on the next step: ok\n");

    // stop -> nothing; start again (new epoch) -> from the top
    tl.running = 0;
    n = run(&q, &st, &lv, &tl, 16 * U, 20 * U, hits, 256);
    CHECK(n == 0, "stopped but %d hits", n);
    tl = (tclock_tl_t){.epoch = 10, .running = 1, .p16_us = 125000, .max_pos = INT64_MAX};
    n = gx_seq_run(&q, &st, &lv, &tl, 10, hits, 256);
    CHECK(n == 2 && hits[0].pos == 0, "restart: %d hits", n);
    printf("stop / start again: silent while stopped, downbeat on restart: ok\n");
}

// The audio path: a hit lands on the exact sample of its clock time.
static void test_sample_accuracy(void) {
    static gx_state_t st;
    gx_state_init(&st);
    st.pats[0].trig[0] = 0xFFFF;   // every 1/16
    gx_live_t lv = {.next = -1};
    gx_seq_t q;
    gx_seq_init(&q);
    // 133 BPM, downbeat 7.3 ms after a block start
    tclock_tl_t tl = {.epoch = 1, .running = 1, .ref_pos = 0, .ref_t = 7300,
                      .p16_us = 15000000 / 133, .max_pos = INT64_MAX};
    int worst = 0, checked = 0;
    for (int64_t fr0 = 0; fr0 < SR * 4; fr0 += 48) {
        int64_t t0 = fr0 * 1000000 / SR, t1 = (fr0 + 48) * 1000000 / SR;
        // the hits that sound before the next block starts
        int n = gx_seq_run(&q, &st, &lv, &tl, tclock_tl_pos(&tl, t1 - 1), hits, 64);
        for (int i = 0; i < n; i++) {
            // as the device does: clock time -> nearest frame of the block
            int64_t t = tclock_tl_time(&tl, hits[i].pos);
            int fr = (int)(((t - t0) * SR + 500000) / 1000000);
            if (fr > 47) fr = 47;
            // the exact frame of this 1/16 on the timeline
            double exact = (7300.0 + (double)(hits[i].pos / TC_U16) * tl.p16_us) * SR / 1e6;
            int d = abs((int)(fr0 + fr) - (int)exact);
            if (d > worst) worst = d;
            checked++;
            CHECK(fr >= 0 && fr < 48, "hit outside its block: fr0 %lld t %lld t0 %lld pos %lld", (long long)fr0, (long long)t, (long long)t0, (long long)hits[i].pos);
        }
    }
    CHECK(worst <= 1 && checked > 30, "worst %d frames over %d hits", worst, checked);
    printf("sample accuracy: %d hits at 133 BPM each within %d frame of exact time\n", checked,
           worst);
}

// --- files ----------------------------------------------------------------------------

static void test_json(void) {
    static gx_state_t a, b;
    gx_kit_state(&GX_KITS[0], &a);
    a.morph = 0.37f;
    a.b[3].k[K_OSC_FREQ] = 0.91f;
    a.pats[5].len = 7;
    a.pats[5].trig[2] = 0x1234;
    a.pats[5].chain = 1;
    const char *path = "build/gx_test_bank.json";
    CHECK(gx_bank_save(path, "My Kit", &a), "save");
    char name[32];
    b.tempo = 99;
    CHECK(gx_bank_load(path, &b, name, sizeof name, false), "load");
    bool same = !strcmp(name, "My Kit") && fabsf(b.morph - 0.37f) < 1e-4f &&
                fabsf(b.b[3].k[K_OSC_FREQ] - 0.91f) < 1e-4f && b.pats[5].len == 7 &&
                b.pats[5].trig[2] == 0x1234 && b.pats[5].chain && fabsf(b.tempo - a.tempo) < 1e-3f &&
                !strcmp(b.names[1], a.names[1]);
    CHECK(same, "round trip");
    // the Python's own file (gintonix_drums.json from the sketch)
    gx_user_drum_t d[8];
    int n = gx_drums_load("../reference/gintonix/gintonix_drums.json", d, 8);
    CHECK(n == 2 && !strcmp(d[0].name, "boom") && !strcmp(d[1].name, "SD Dextrove 5") &&
              memcmp(&d[0].a, &d[0].b, sizeof d[0].a) != 0 &&
              memcmp(&d[1].a, &d[1].b, sizeof d[1].a) == 0 && d[1].a.s[S_N_ENV] == 2,
          "drums file: %d", n);
    printf("files: bank slot round trip; the sketch's gintonix_drums.json reads (%d sounds)\n", n);
}

int main(void) {
    test_voices();
    test_choke();
    test_player();
    test_sample_accuracy();
    test_json();
    printf(failures ? "\n%d FAILURES\n" : "\nall gintonix tests passed\n", failures);
    return failures ? 1 : 0;
}
