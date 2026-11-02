// test_d9.c - Drumster9's core on the host: the sample image, the kits and
// presets, the player on the master clock, the sound and FX, the project
// files. Run: make -C tests test
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "d9_dsp.h"
#include "d9_json.h"
#include "d9_model.h"
#include "d9_samples.h"
#include "d9_seq.h"

static int failures;
#define CHECK(c, ...)                                         \
    do {                                                      \
        if (!(c)) {                                           \
            failures++;                                       \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                              \
            printf("\n");                                     \
        }                                                     \
    } while (0)

static uint8_t *s_img;
static long s_img_len;
static d9_index_t s_ix;

static const int16_t *pcm_of(int preset, uint32_t *len) {
    const d9_sample_t *s = d9_sample(&s_ix, preset);
    *len = s ? s->frames : 0;
    return s ? (const int16_t *)(s_img + s->offset) : NULL;
}

// --- the sample image -----------------------------------------------------------------------

static void test_samples(const char *path) {
    FILE *f = fopen(path, "rb");
    CHECK(f, "no %s (tools/make_d9_samples.py)", path);
    if (!f) exit(1);
    fseek(f, 0, SEEK_END);
    s_img_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    s_img = malloc(s_img_len);
    CHECK(fread(s_img, 1, s_img_len, f) == (size_t)s_img_len, "read");
    fclose(f);
    CHECK(d9_index_bytes(s_img) > 0 && d9_index_bytes(s_img) < 8192, "index size %u", d9_index_bytes(s_img));
    CHECK(d9_index_parse(&s_ix, s_img, (uint32_t)s_img_len), "parse");
    CHECK(s_ix.nsamples == 155 && s_ix.nbanks == 9, "%d samples, %d banks", s_ix.nsamples, s_ix.nbanks);
    CHECK(s_ix.total == (uint32_t)s_img_len && s_img_len <= 0x400000, "size %ld", s_img_len);
    const d9_sample_t *k = d9_sample(&s_ix, 0);
    CHECK(k && !strcmp(k->name, "TR-808 Bass Drum 1") && k->frames == 68832, "preset 0");
    // the sketch's names at its preset numbers
    static const struct {
        int p;
        const char *n;
    } names[] = {{12, "TR-808 Snare 1"}, {256, "909BD"},        {280, "SnareDrum 1"},
                 {286, "MR-12#SNR"},     {296, "Tokyo Burst bd"}, {312, "Metal Kick"},
                 {350, "Noice Kick"},    {377, "Shaker Tiny"},   {391, "Narrow"}};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char t[24];
        CHECK(!strcmp(d9_sample_name(&s_ix, names[i].p, t, sizeof t), names[i].n), "preset %d: %s",
              names[i].p, d9_sample_name(&s_ix, names[i].p, t, sizeof t));
    }
    CHECK(d9_sample_ms(&s_ix, 0) == 3121, "808 kick %d ms", d9_sample_ms(&s_ix, 0));
    CHECK(d9_sample(&s_ix, 19) == NULL && d9_sample(&s_ix, 392) == NULL, "gaps");
    int missing = 0;
    for (int kt = 0; kt < D9_KITS; kt++)
        for (int l = 0; l < D9_LANES; l++)
            if (D9_KIT[kt].s[l] != D9_NO_SAMPLE && !d9_sample(&s_ix, D9_KIT[kt].s[l])) missing++;
    CHECK(missing == 0, "%d kit samples missing", missing);
    CHECK(d9_bank_find(&s_ix, "extras") == 8 && s_ix.banks[8].first == 378, "banks");
    // a damaged image is refused
    uint8_t bad[64];
    memcpy(bad, s_img, 64);
    bad[0] = 'X';
    CHECK(!d9_index_parse(&s_ix, bad, 64), "bad magic");
    d9_index_parse(&s_ix, s_img, (uint32_t)s_img_len);
    printf("samples: 155 in 9 banks (%ld KB), the sketch's preset numbers and names, every kit's\n",
           s_img_len / 1024);
}

// --- model ---------------------------------------------------------------------------------------

static void test_model(void) {
    CHECK(d9_euclid(32, 4) == ((1u << 7) | (1u << 15) | (1u << 23) | (1u << 31)), "euclid 4/32");
    CHECK(d9_euclid(32, 0) == 0 && d9_euclid(32, 40) == 0xFFFFFFFFu, "euclid edges");
    static d9_state_t s;
    d9_state_init(&s);
    d9_apply_preset(&s, 0);
    CHECK(s.mask[L_KICK] == 0x01010101u && s.mask[L_CHAT] == 0x11111111u, "4 on Floor");
    CHECK(s.bank[0].used && s.bank[0].m[L_KICK] == s.mask[L_KICK], "the slot follows");
    // random fills keep the sketch's shapes
    uint32_t r = 12345;
    int bad = 0;
    for (int i = 0; i < 2000; i++) {
        uint32_t k = d9_random_steps(L_KICK, &r);
        if (k && (!(k & 1) || (k & ~0x11111111u))) bad++;          // starts the bar, on the beat grid
        uint32_t c = d9_random_steps(L_CYMBAL, &r);
        if (c & ~0x01010101u) bad++;                              // on quarters
        uint32_t h = d9_random_steps(L_CHAT, &r);
        if (h & ~0x55555555u) bad++;                              // on 1/16s
    }
    CHECK(bad == 0, "%d random fills off their grid", bad);
    // banks: switching keeps the grid it leaves
    d9_slot_switch(&s, 1);
    CHECK(s.mask[L_KICK] == 0 && s.bank_active == 1, "empty slot starts empty");
    s.mask[L_SNARE] = 0xF;
    d9_slot_store(&s);
    d9_slot_switch(&s, 0);
    CHECK(s.mask[L_KICK] == 0x01010101u, "back to A");
    CHECK(s.bank[1].m[L_SNARE] == 0xF, "B kept");
    // solo
    s.solo[L_TOM] = 1;
    CHECK(!d9_audible(&s, L_KICK) && d9_audible(&s, L_TOM), "solo");
    s.solo[L_TOM] = 0;
    s.muted[L_KICK] = 1;
    CHECK(!d9_audible(&s, L_KICK), "mute");
    printf("model: 8 presets, Euclidean fills on the sketch's grids, A-J slots, mute / solo: ok\n");
}

// --- the player ------------------------------------------------------------------------------------

static tclock_tl_t tl_at(int64_t ref_pos) {
    return (tclock_tl_t){.epoch = 1, .running = 1, .ref_pos = ref_pos, .ref_t = 0, .p16_us = 125000,
                         .max_pos = INT64_MAX, .bpm_x100 = 12000};
}

static void test_seq(void) {
    static d9_state_t s;
    d9_state_init(&s);
    d9_apply_preset(&s, 0);
    d9_seq_t q;
    d9_seq_init(&q);
    tclock_tl_t tl = tl_at(0);
    volatile int8_t next = -1;
    uint32_t sw = 0;
    d9_hit_t h[64];
    int kicks = 0, chats = 0, at_ok = 1;
    // one bar, in blocks of 1 ms
    for (int64_t t = 0; t < 2000000; t += 1000) {
        int64_t end = tclock_tl_pos(&tl, t + 1000) - 1;
        int n = d9_seq_run(&q, &s, &tl, end, &next, &sw, h, 64);
        for (int i = 0; i < n; i++) {
            if (h[i].pos % D9_STEP_UNITS) at_ok = 0;
            if (h[i].lane == L_KICK) kicks++;
            if (h[i].lane == L_CHAT) chats++;
        }
    }
    CHECK(kicks == 4 && chats == 8 && at_ok, "one bar: %d kicks, %d hats", kicks, chats);
    // a queued slot switches on the downbeat, and that downbeat already plays it
    d9_slot_switch(&s, 2);
    s.mask[L_TOM] = 1;   // C: a tom on step 1
    d9_slot_store(&s);
    d9_slot_switch(&s, 0);
    d9_seq_init(&q);
    next = -1;
    int64_t t = 0;
    for (; t < 500000; t += 1000) d9_seq_run(&q, &s, &tl, tclock_tl_pos(&tl, t + 1000) - 1, &next, &sw, h, 64);
    next = 2;   // queued mid-bar
    int tom_at = -1;
    for (; t < 4000000 && tom_at < 0; t += 1000) {
        int n = d9_seq_run(&q, &s, &tl, tclock_tl_pos(&tl, t + 1000) - 1, &next, &sw, h, 64);
        for (int i = 0; i < n; i++)
            if (h[i].lane == L_TOM) tom_at = (int)h[i].pos;
    }
    CHECK(tom_at == D9_BAR_UNITS && s.bank_active == 2 && next == -1, "slot C on the next downbeat (%d)", tom_at);
    // mute: no hits; a kit with no sample for the role: no hits
    s.muted[L_TOM] = 1;
    d9_seq_init(&q);
    int toms = 0;
    for (t = 0; t < 2000000; t += 1000) {
        int n = d9_seq_run(&q, &s, &tl, tclock_tl_pos(&tl, t + 1000) - 1, &next, &sw, h, 64);
        for (int i = 0; i < n; i++) toms += h[i].lane == L_TOM;
    }
    CHECK(toms == 0, "muted lane hit %d times", toms);
    // started mid-bar (MIDI Song Position): steps keep their place in the bar
    d9_apply_preset(&s, 0);
    d9_seq_init(&q);
    tl = tl_at(4 * TC_U16);   // beat 2
    int n = d9_seq_run(&q, &s, &tl, 4 * TC_U16 + 10, &next, &sw, h, 64);
    CHECK(n > 0 && h[0].step == 8 && h[0].lane == L_KICK, "from beat 2: step %d", n ? h[0].step : -1);
    tl.running = 0;
    CHECK(d9_seq_run(&q, &s, &tl, 99999999, &next, &sw, h, 64) == 0 && !q.active, "stopped");
    printf("player: 1/32 steps on the grid, a slot on the next downbeat, mute, SPP: ok\n");
}

// --- sound -------------------------------------------------------------------------------------------

static d9_engine_t eng;
static void *alloc(unsigned n) { return calloc(1, n); }

static void mix_default(d9_mix_t *m) {
    memset(m, 0, sizeof *m);
    for (int l = 0; l < D9_NVOICE; l++) m->lane_gain[l] = 1.0f;
    for (int b = 0; b < D9_BUSES; b++) d9_bus_default(&m->bus[b]);
}

#define N 48
static float L[44100 * 3 + N], R[44100 * 3 + N];

// render `frames` with one hit of preset at frame `at`
static void play(const d9_mix_t *m, int lane, int preset, int at, int frames) {
    memset(L, 0, sizeof L);
    memset(R, 0, sizeof R);
    for (int f = 0; f < frames; f += N) {
        d9_ev_t ev;
        int nev = 0;
        if (at >= f && at < f + N) {
            uint32_t len;
            const int16_t *p = pcm_of(preset, &len);
            ev = (d9_ev_t){at - f, (uint8_t)lane, 1.0f, p, len};
            nev = 1;
        }
        d9_engine_render(&eng, m, &ev, nev, L + f, R + f, N);
    }
}

static int first_sound(int from) {
    for (int i = from; i < 44100 * 3; i++)
        if (fabsf(L[i]) > 1e-4f) return i;
    return -1;
}

static float energy(int a, int b) {
    double e = 0;
    for (int i = a; i < b; i++) e += (double)L[i] * L[i];
    return (float)e;
}

static void test_dsp(void) {
    CHECK(d9_engine_init(&eng, alloc), "init");
    d9_mix_t m;
    mix_default(&m);
    // the hit starts on its frame, and is the sample at 2x
    play(&m, L_KICK, 0, 100, 44100);
    int on = first_sound(0);
    CHECK(on >= 100 && on < 104, "kick starts at frame %d", on);
    uint32_t len;
    const int16_t *p = pcm_of(0, &len);
    int k0 = 0;
    while (k0 < (int)len && abs(p[k0]) < 4) k0++;
    float want = p[k0 + 200] / 32768.0f;
    CHECK(fabsf(L[100 + 2 * (k0 + 200)] - want) < 1e-4f && L[0] == 0, "sample at 2x (%f vs %f)",
          L[100 + 2 * (k0 + 200)], want);
    CHECK(energy(100 + 2 * (int)len + 10, 44100 * 3) == 0, "silent after the sample");
    // lane gain 0 (mute / solo / stopped): nothing
    m.lane_gain[L_KICK] = 0;
    d9_engine_silence(&eng);
    play(&m, L_KICK, 0, 0, 4410);
    CHECK(energy(0, 4410) < 1e-6f, "gate closed");
    m.lane_gain[L_KICK] = 1;
    // a retrigger mid-sample doesn't click: no sample-to-sample jump bigger
    // than the sample itself makes
    d9_engine_silence(&eng);
    memset(L, 0, sizeof L);
    memset(R, 0, sizeof R);
    uint32_t sl;
    const int16_t *sp = pcm_of(268, &sl);   // 909 snare
    float maxd_hit = 0;
    for (uint32_t i = 1; i < sl; i++) {
        float d = fabsf((float)(sp[i] - sp[i - 1])) / 32768.0f;
        if (d > maxd_hit) maxd_hit = d;
    }
    for (int f = 0; f < 22050; f += N) {
        d9_ev_t ev = {7, L_SNARE, 1.0f, sp, sl};
        d9_engine_render(&eng, &m, &ev, (f % (N * 50)) == 0 ? 1 : 0, L + f, R + f, N);
    }
    float maxd = 0;
    for (int i = 1; i < 22050; i++) {
        float d = fabsf(L[i] - L[i - 1]);
        if (d > maxd) maxd = d;
    }
    CHECK(maxd < 1.6f * maxd_hit, "retrigger jump %.3f (the sample's own %.3f)", maxd, maxd_hit);
    // the filter: LPF at 300 Hz takes the top off a hat
    d9_engine_silence(&eng);
    play(&m, L_CHAT, 260, 0, 8000);   // 909 hat
    float bright = energy(0, 8000);
    m.lane_bus[L_CHAT] = 1;
    m.bus[1].filter = FT_LPF;
    m.bus[1].freq = 300;
    d9_engine_silence(&eng);
    play(&m, L_CHAT, 260, 0, 8000);
    float dark = energy(0, 8000);
    CHECK(dark < 0.1f * bright, "LPF 300 Hz: %.1f%% of the energy", 100 * dark / bright);
    m.bus[1].filter = FT_HPF;
    m.bus[1].freq = 2000;
    m.lane_bus[L_KICK] = 1;
    d9_engine_silence(&eng);
    play(&m, L_KICK, 0, 0, 44100);   // 808 kick through an HPF at 2 kHz
    float hp = energy(0, 44100);
    m.bus[1].filter = FT_OFF;
    d9_engine_silence(&eng);
    play(&m, L_KICK, 0, 0, 44100);
    CHECK(energy(0, 44100) > 0 && hp < 0.05f * energy(0, 44100), "HPF 2 kHz thins the kick (%.1f%%)",
          100 * hp / energy(0, 44100));
    // the echo: a copy echo_ms later
    m.lane_bus[L_CLAP] = 2;
    m.bus[2].echo_level = 0.5f;
    m.bus[2].echo_ms = 250;
    m.bus[2].echo_fb = 0;
    d9_engine_silence(&eng);
    play(&m, L_CLAP, 3, 0, 44100);   // 808 clap, 146 ms
    int dry_on = first_sound(0), echo_on = first_sound(2 * 3208 + 100);
    CHECK(abs(echo_on - dry_on - 11025) <= 8, "echo %d frames after the hit (250 ms = 11025)", echo_on - dry_on);
    // the reverb: a tail where the dry hit is long gone
    m.bus[2].echo_level = 0;
    m.bus[2].rev_level = 0.5f;
    d9_engine_silence(&eng);
    play(&m, L_CLAP, 3, 0, 44100);
    float tail = energy(2 * 3208 + 2000, 2 * 3208 + 12000);
    CHECK(tail > 1e-4f, "reverb tail %g", tail);
    // EQ +15 dB low adds bass, -15 dB takes it away
    m.bus[2].rev_level = 0;
    m.bus[2].eq_l = 15;
    d9_engine_silence(&eng);
    m.lane_bus[L_KICK] = 2;
    play(&m, L_KICK, 0, 0, 44100);
    float boost = energy(0, 44100);
    m.bus[2].eq_l = -15;
    d9_engine_silence(&eng);
    play(&m, L_KICK, 0, 0, 44100);
    float cut = energy(0, 44100);
    m.bus[2].eq_l = 0;
    d9_engine_silence(&eng);
    play(&m, L_KICK, 0, 0, 44100);
    float flat = energy(0, 44100);
    CHECK(boost > 3 * flat && cut < 0.3f * flat, "EQ low: +15 x%.1f, -15 x%.2f", boost / flat, cut / flat);
    // everything at once settles; an idle bus goes quiet
    for (int b = 0; b < D9_BUSES; b++) {
        m.bus[b].rev_level = 1;
        m.bus[b].rev_live = 1;
        m.bus[b].echo_level = 1;
        m.bus[b].echo_fb = 0.95f;
        m.bus[b].filter = FT_LPF;
        m.bus[b].reso = 8;
        m.bus[b].eq_l = m.bus[b].eq_m = m.bus[b].eq_h = 15;
    }
    d9_engine_silence(&eng);
    float pk = 0;
    for (int k = 0; k < 40; k++) {
        play(&m, k % D9_LANES, D9_KIT[1].s[k % D9_LANES], 10, 2048);
        for (int i = 0; i < 2048; i++) pk = fmaxf(pk, fabsf(L[i]));
    }
    CHECK(isfinite(pk) && pk < 200, "everything at once: peak %f", pk);
    mix_default(&m);
    d9_engine_silence(&eng);
    play(&m, L_CLAP, 3, 0, 44100);
    bool idle = true;
    for (int b = 0; b < D9_BUSES; b++) idle &= !eng.bus[b].active;
    CHECK(idle, "buses idle once the sound has gone");
    // CPU: 7 lanes, all 4 buses with every effect
    for (int b = 0; b < D9_BUSES; b++) {
        m.bus[b].rev_level = m.bus[b].echo_level = 0.5f;
        m.bus[b].filter = FT_LPF;
        m.bus[b].eq_m = 3;
    }
    for (int l = 0; l < D9_LANES; l++) m.lane_bus[l] = (uint8_t)(l % D9_BUSES);
    clock_t c0 = clock();
    for (int k = 0; k < 200; k++)
        for (int l = 0; l < D9_LANES; l++) play(&m, l, D9_KIT[0].s[l], 0, 4410);
    double sec = (double)(clock() - c0) / CLOCKS_PER_SEC, audio = 200.0 * D9_LANES * 4410 / 44100;
    printf("dsp: on the frame, 2x linear, click-free retrigger, LPF/HPF, echo at 250 ms, reverb,\n"
           "     EQ, settles, idles; 4 busy buses = %.1f%% of real time here (sanitizers on)\n",
           100 * sec / audio);
}

// --- the project files ----------------------------------------------------------------------------

static void test_json(void) {
    // a project as the Python wrote it
    const char *py =
        "{\"name\": \"Groove\", \"bpm\": 98, \"kit\": 1,"
        " \"lanes\": [{\"name\": \"Kick\", \"steps\": [0, 8, 16, 24], \"vol\": 0.8, \"muted\": false,"
        "   \"solo\": false, \"kit_override\": 4, \"bus\": 2},"
        "  {\"name\": \"Snare\", \"steps\": [8, 24], \"vol\": 0.5, \"muted\": true, \"solo\": false,"
        "   \"kit_override\": null, \"bus\": 0}],"
        " \"bus_fx\": [{\"rev_level\": 0.0}, {}, {\"rev_level\": 0.35, \"echo_ms\": 375,"
        "   \"filter_type\": 1, \"filter_freq\": 2400, \"drive\": 1.4}, {}],"
        " \"bank\": [{\"steps\": {\"Kick\": [0, 8, 16, 24], \"Snare\": [8, 24]}, \"kit\": 1, \"bpm\": 98},"
        "   null, {\"steps\": {\"Kick\": [0, 12]}, \"kit\": 5, \"bpm\": 98}, null, null, null, null,"
        "   null, null, null],"
        " \"bank_active\": 0, \"master_vol\": 12.5,"
        " \"kit_overrides\": {\"1\": {\"Snare\": 269, \"Bogus\": 3}, \"9\": {\"Kick\": 1}}}";
    cJSON *o = cJSON_Parse(py);
    static d9_state_t s, t;
    float bpm;
    d9_project_from_json(o, &s, &bpm);
    cJSON_Delete(o);
    CHECK(bpm == 98 && s.kit == 1 && s.master == 12.5f, "header");
    CHECK(s.kit_ovr[L_KICK] == 4 && s.bus[L_KICK] == 2 && fabsf(s.vol[L_KICK] - 0.8f) < 1e-6f, "kick lane");
    CHECK(s.muted[L_SNARE] && s.kit_ovr[L_SNARE] == -1, "snare lane");
    CHECK(s.fx[2].filter == FT_LPF && s.fx[2].freq == 2400 && s.fx[2].echo_ms == 375 &&
              fabsf(s.fx[2].rev_level - 0.35f) < 1e-6f && s.fx[1].rev_live == 0.75f,
          "bus fx");
    CHECK(s.kit_map[1][L_SNARE] == 269 && s.kit_map[1][L_KICK] == 256, "kit overrides");
    CHECK(s.bank[0].used && !s.bank[1].used && s.bank[2].used && s.bank[2].kit == 5 &&
              s.bank[2].m[L_KICK] == ((1u << 0) | (1u << 12)),
          "bank");
    CHECK(s.mask[L_KICK] == 0x01010101u && s.mask[L_SNARE] == ((1u << 8) | (1u << 24)), "grid");
    // round trip
    s.solo[L_TOM] = 1;
    s.fx[3].eq_h = -6;
    o = d9_project_to_json(&s, "Groove", 98);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    o = cJSON_Parse(txt);
    d9_project_from_json(o, &t, &bpm);
    cJSON_Delete(o);
    CHECK(strstr(txt, "\"kit_overrides\":{\"1\":{\"Snare\":269}}") != NULL, "only the changed samples");
    free(txt);
    bool same = !memcmp(s.mask, t.mask, sizeof s.mask) && !memcmp(s.kit_map, t.kit_map, sizeof s.kit_map) &&
                !memcmp(s.bus, t.bus, sizeof s.bus) && !memcmp(s.kit_ovr, t.kit_ovr, sizeof s.kit_ovr) &&
                !memcmp(s.muted, t.muted, sizeof s.muted) && !memcmp(s.solo, t.solo, sizeof s.solo) &&
                s.kit == t.kit && s.master == t.master && s.bank_active == t.bank_active && bpm == 98;
    for (int b = 0; b < D9_BUSES; b++) same &= !memcmp(&s.fx[b], &t.fx[b], sizeof s.fx[b]);
    for (int i = 0; i < D9_BANKS; i++)
        same &= s.bank[i].used == t.bank[i].used &&
                (!s.bank[i].used || (!memcmp(s.bank[i].m, t.bank[i].m, sizeof s.bank[i].m) && s.bank[i].kit == t.bank[i].kit));
    CHECK(same, "round trip");
    // junk is refused field by field
    o = cJSON_Parse("{\"kit\": 99, \"lanes\": [{\"name\": \"Kick\", \"bus\": 7, \"vol\": 5, \"steps\": [3, 40, -1]}],"
                    " \"bank_active\": 22, \"master_vol\": 999}");
    d9_project_from_json(o, &t, &bpm);
    cJSON_Delete(o);
    CHECK(t.kit == 0 && t.bus[L_KICK] == 0 && t.vol[L_KICK] == 1 && t.mask[L_KICK] == (1u << 3) &&
              t.bank_active == 0 && t.master == D9_MASTER_MAX,
          "junk");
    printf("projects: the sketch's file loads (lanes, buses, A-J, kit samples); round trip; junk\n");
}

int main(int argc, char **argv) {
    test_samples(argc > 1 ? argv[1] : "build/gamma9001.bin");
    test_model();
    test_seq();
    test_dsp();
    test_json();
    printf(failures ? "\n%d drumster9 test(s) FAILED\n" : "all drumster9 tests passed\n", failures);
    return failures ? 1 : 0;
}
