// gx_tool.c - host side of the GinTonix checks (gx_parity.py, test_gx.c).
//
//   gx_tool layout          < pattern     hits of two passes (see gx_parity.py)
//   gx_tool fmt             < patches     each knob's display text
//   gx_tool kit N                         factory kit N as state JSON
//   gx_tool mt              < kit.json    Microtonic kit -> state JSON
//   gx_tool render N bars out.wav         factory kit N, pattern A.., to a WAV
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "gx_engine.h"
#include "gx_json.h"
#include "gx_seq.h"

static int cmd_layout(void) {
    static gx_state_t st;
    gx_state_init(&st);
    int len, rate, fr, choke, mute;
    float swing;
    if (scanf("%d %d %f %d %d %d", &len, &rate, &swing, &fr, &choke, &mute) != 6) return 2;
    gx_pat_t *p = &st.pats[0];
    p->len = (uint8_t)len;
    for (int c = 0; c < GX_NCH; c++) {
        unsigned t, a, f;
        if (scanf("%u %u %u", &t, &a, &f) != 3) return 2;
        p->trig[c] = (uint16_t)t;
        p->acc[c] = (uint16_t)a;
        p->fill[c] = (uint16_t)f;
    }
    st.rate = (uint8_t)rate;
    st.swing = swing;
    st.fill_rate = (uint8_t)fr;
    gx_live_t live = {.mute = (uint8_t)mute, .choke = (uint8_t)choke, .next = -1};
    gx_seq_t q;
    gx_seq_init(&q);
    tclock_tl_t tl = {.epoch = 1, .running = 1, .p16_us = 125000, .max_pos = INT64_MAX};
    int64_t P = (int64_t)len * gx_step_units(rate);
    gx_hit_t hits[64];
    // feed it in small windows, as the audio blocks do
    for (int64_t pos = -500;; pos += 97) {
        if (pos > 2 * P - 1) pos = 2 * P - 1;
        int n = gx_seq_run(&q, &st, &live, &tl, pos, hits, 64);
        for (int i = 0; i < n; i++)
            printf("%lld %d %.4f\n", (long long)hits[i].pos, hits[i].ch, hits[i].vel);
        if (pos == 2 * P - 1) break;
    }
    return 0;
}

static int cmd_fmt(void) {
    gx_patch_t p;
    for (;;) {
        for (int i = 0; i < GX_NKNOB; i++)
            if (scanf("%f", &p.k[i]) != 1) return 0;
        for (int i = 0; i < GX_NSW; i++) {
            int v;
            if (scanf("%d", &v) != 1) return 0;
            p.s[i] = (uint8_t)v;
        }
        for (int i = 0; i < GX_NKNOB; i++) {
            char t[32];
            gx_fmt(i, &p, t, sizeof t);
            printf("%s%s", i ? "\t" : "", t);
        }
        printf("\n");
    }
}

static void print_state(const gx_state_t *s) {
    cJSON *o = gx_json_state(s);
    char *t = cJSON_PrintUnformatted(o);
    printf("%s\n", t);
    cJSON_free(t);
    cJSON_Delete(o);
}

static int cmd_mt(void) {
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf - 1, stdin);
    buf[n] = 0;
    cJSON *o = cJSON_Parse(buf);
    if (!o) return 2;
    static gx_state_t s;
    s.tempo = 120;
    gx_json_mt_kit(o, &s);
    print_state(&s);
    cJSON_Delete(o);
    return 0;
}

static void wav_header(FILE *f, int frames) {
    int data = frames * 4;
    fwrite("RIFF", 1, 4, f);
    int v = 36 + data;
    fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v = 16;
    fwrite(&v, 4, 1, f);
    short s = 1;
    fwrite(&s, 2, 1, f);
    s = 2;
    fwrite(&s, 2, 1, f);
    v = 44100;
    fwrite(&v, 4, 1, f);
    v = 44100 * 4;
    fwrite(&v, 4, 1, f);
    s = 4;
    fwrite(&s, 2, 1, f);
    s = 16;
    fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data, 4, 1, f);
}

// The device's audio path in miniature: timeline -> hits -> frames -> voices.
static int cmd_render(int kit, int bars, const char *path) {
    static gx_state_t st;
    gx_kit_state(&GX_KITS[kit], &st);
    static gx_engine_t e;
    gx_engine_init(&e);
    gx_live_t live = {.next = -1};
    for (int c = 0; c < GX_NCH; c++) {
        gx_patch_t p;
        gx_eff(&st, c, &p);
        gx_vp_from_patch(&e.vp[c], &p, st.master);
        if (p.s[S_CHOKE]) live.choke |= (uint8_t)(1u << c);
    }
    tclock_tl_t tl = {.epoch = 1, .running = 1, .p16_us = (int64_t)(15000000.0f / st.tempo),
                      .max_pos = INT64_MAX, .ref_t = 0};
    gx_seq_t q;
    gx_seq_init(&q);
    int frames = (int)(bars * 16 * tl.p16_us * 44100LL / 1000000) + 44100;
    FILE *f = path ? fopen(path, "wb") : NULL;
    if (f) wav_header(f, frames);
    float peak = 0, l[48], r[48];
    double sum2 = 0;
    long hits_total = 0;
    for (int fr0 = 0; fr0 < frames; fr0 += 48) {
        int64_t t0 = (int64_t)fr0 * 1000000 / 44100;
        int64_t t1 = (int64_t)(fr0 + 48) * 1000000 / 44100;
        int64_t pos_end = tclock_tl_pos(&tl, t1 - 1);
        if (fr0 + 48 >= frames - 44100) tl.running = 1;
        gx_hit_t hits[64];
        int nh = gx_seq_run(&q, &st, &live, &tl, pos_end, hits, 64);
        gx_ev_t ev[64];
        for (int i = 0; i < nh; i++) {
            int64_t t = tclock_tl_time(&tl, hits[i].pos);
            int fr = (int)((t - t0) * 44100 / 1000000);
            ev[i] = (gx_ev_t){fr < 0 ? 0 : (fr > 47 ? 47 : fr), hits[i].ch, hits[i].vel};
        }
        hits_total += nh;
        memset(l, 0, sizeof l);
        memset(r, 0, sizeof r);
        gx_engine_render(&e, ev, nh, l, r, 48);
        for (int i = 0; i < 48; i++) {
            float a = fabsf(l[i]) > fabsf(r[i]) ? fabsf(l[i]) : fabsf(r[i]);
            if (a > peak) peak = a;
            sum2 += (double)l[i] * l[i] + (double)r[i] * r[i];
            if (f) {
                short s[2];
                float x = l[i] > 1 ? 1 : (l[i] < -1 ? -1 : l[i]);
                float y = r[i] > 1 ? 1 : (r[i] < -1 ? -1 : r[i]);
                s[0] = (short)(x * 32000);
                s[1] = (short)(y * 32000);
                fwrite(s, 2, 2, f);
            }
        }
    }
    if (f) fclose(f);
    printf("%s: %ld hits, peak %.3f, rms %.4f\n", GX_KITS[kit].name, hits_total, peak,
           sqrt(sum2 / (2.0 * frames)));
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "layout")) return cmd_layout();
    if (!strcmp(argv[1], "fmt")) return cmd_fmt();
    if (!strcmp(argv[1], "mt")) return cmd_mt();
    if (!strcmp(argv[1], "kit") && argc > 2) {
        static gx_state_t s;
        gx_kit_state(&GX_KITS[atoi(argv[2])], &s);
        print_state(&s);
        return 0;
    }
    if (!strcmp(argv[1], "render") && argc > 3)
        return cmd_render(atoi(argv[2]), atoi(argv[3]), argc > 4 ? argv[4] : NULL);
    return 2;
}
