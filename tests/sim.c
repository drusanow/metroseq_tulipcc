// sim.c - drive the C engine exactly like the firmware's sequencer task
// does (sleep until next_due, advance, execute actions) on a virtual clock,
// and print every output action. Used by the Python parity test.
//
//   sim <pattern.txt> <duration_ms>
//
// pattern.txt is the flat text format written by py_parity.py.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "seq_engine.h"

static const char *const ACT_NAMES[] = {"GATE", "PITCH", "AON", "AOFF", "CC"};

static int load_pattern(const char *path, pattern_t *p) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    pattern_default(p);
    char line[256];
    while (fgets(line, sizeof line, f)) {
        int i, a, b, c, d, e, g, h, k, m;
        int v[8];
        if (sscanf(line, "global %d %d %d %d %d", &a, &b, &c, &d, &e) == 5) {
            p->bpm = a; p->root = b; p->scale_idx = c; p->base_octave = d; p->merge_cv = e;
        } else if (sscanf(line, "stage %d %d %d %d %d %d %d %d %d", &i, &a, &b, &c, &d, &e, &g,
                          &h, &k) == 9) {
            stage_t *s = &p->stages[i];
            s->pitch = (uint8_t)rdiv(a * PITCH_STEPS, 36);   // the Python's semitones over 3 octaves
            s->pulses = b; s->gate = c; s->slide = d; s->skip = e;
            s->ratchet = g; s->prob = h; s->accum = k;
        } else if (sscanf(line, "track %d %d %d %d %d %d %d %d %d %d", &i, &a, &b, &c, &d, &e,
                          &g, &h, &k, &m) == 10) {
            track_cfg_t *t = &p->tracks[i];
            t->order = a; t->length = b; t->div_i = c; t->swing = d; t->gate_pct = e;
            t->transpose = g; t->octave = h; t->invert = k; t->mute_audio = m;
        } else if (sscanf(line, "mod %d %d %d %d %d %d %d %d %d %d %d %d %d %d", &i, &v[0],
                          &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &a, &b, &c, &d,
                          &e) == 14) {
            mod_lane_cfg_t *ml = &p->mods[i];
            for (int j = 0; j < 8; j++) ml->values[j] = v[j];
            ml->order = a; ml->length = b; ml->div_i = c; ml->depth = d; ml->dest = e;
        }
    }
    fclose(f);
    pattern_sanitise(p);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: sim pattern.txt duration_ms\n");
        return 2;
    }
    static pattern_t pat;
    if (load_pattern(argv[1], &pat)) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    int64_t end = (int64_t)atoi(argv[2]) * 1000;
    static seq_engine_t eng;
    static seq_actions_t acts;
    seq_engine_init(&eng, &pat, 1);
    seq_actions_clear(&acts);
    seq_engine_start(&eng, 0, &acts);
    for (;;) {
        for (int i = 0; i < acts.n; i++)
            printf("%lld %s %d %d\n", (long long)acts.a[i].due_us, ACT_NAMES[acts.a[i].type],
                   acts.a[i].out, acts.a[i].value);
        seq_actions_clear(&acts);
        int64_t t = seq_engine_next_due(&eng);
        if (t > end) break;
        seq_engine_advance(&eng, t, &acts);
    }
    return 0;
}
