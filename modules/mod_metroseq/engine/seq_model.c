// seq_model.c - name tables, quantiser and pattern helpers.
#include "seq_model.h"

#include <stdio.h>
#include <string.h>

const char *const GATE_NAMES[GATE_COUNT] = {"MULT", "SING", "HOLD", "REST"};
const char *const ORDER_NAMES[ORD_COUNT] = {"FWD", "REV", "PEND", "RAND", "BRWN"};
const uint8_t DIV_VALUES[NUM_DIVS] = {1, 2, 3, 4, 6, 8};
const char *const DIV_NAMES[NUM_DIVS] = {"1/16", "1/8", "1/8.", "1/4", "1/4.", "1/2"};
const char *const ROOT_NAMES[12] = {"C", "C#", "D", "D#", "E", "F",
                                    "F#", "G", "G#", "A", "A#", "B"};
const char *const DEST_NAMES[NUM_DESTS] = {
    "OFF",
    "T1 Transpose", "T1 Octave", "T1 Pitch", "T1 Pulses", "T1 Gate len", "T1 Swing",
    "T2 Transpose", "T2 Octave", "T2 Pitch", "T2 Pulses", "T2 Gate len", "T2 Swing",
};

const scale_t SCALES[NUM_SCALES] = {
    {"Chromatic", 12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
    {"Major",     7,  {0, 2, 4, 5, 7, 9, 11}},
    {"Minor",     7,  {0, 2, 3, 5, 7, 8, 10}},
    {"Dorian",    7,  {0, 2, 3, 5, 7, 9, 10}},
    {"Phrygian",  7,  {0, 1, 3, 5, 7, 8, 10}},
    {"Lydian",    7,  {0, 2, 4, 6, 7, 9, 11}},
    {"Mixolyd.",  7,  {0, 2, 4, 5, 7, 9, 10}},
    {"Maj Pent",  5,  {0, 2, 4, 7, 9}},
    {"Min Pent",  5,  {0, 3, 5, 7, 10}},
    {"Harm Min",  7,  {0, 2, 3, 5, 7, 8, 11}},
    {"Blues",     6,  {0, 3, 5, 6, 7, 10}},
};

int quantise(int semi, const scale_t *scale, int root) {
    if (semi < 0) semi = 0;
    int s = semi - root;
    int octv = pfloordiv(s, 12);
    int within = pmod(s, 12);
    int best = scale->ivals[0];
    int bestd = 99;
    for (int i = 0; i < scale->n; i++) {
        int d = scale->ivals[i] - within;
        if (d < 0) d = -d;
        if (d < bestd) {
            bestd = d;
            best = scale->ivals[i];
        }
    }
    return octv * 12 + best + root;
}

void note_name(int midi, char *buf, int len) {
    if (midi < 0) {
        snprintf(buf, len, "?");
        return;
    }
    snprintf(buf, len, "%s%d", ROOT_NAMES[midi % 12], (midi / 12) - 1);
}

void pattern_sanitise(pattern_t *p) {
    p->bpm = clampi(p->bpm, MIN_BPM, MAX_BPM);
    p->root = clampi(p->root, 0, 11);
    p->scale_idx = clampi(p->scale_idx, 0, NUM_SCALES - 1);
    p->base_octave = clampi(p->base_octave, MIN_BASE_OCTAVE, MAX_BASE_OCTAVE);
    p->merge_cv = p->merge_cv ? 1 : 0;
    for (int i = 0; i < NUM_STAGES; i++) {
        stage_t *s = &p->stages[i];
        s->pitch = clampi(s->pitch, 0, PITCH_STEPS);
        s->pulses = clampi(s->pulses, 1, 8);
        s->gate = clampi(s->gate, 0, GATE_COUNT - 1);
        s->slide = s->slide ? 1 : 0;
        s->skip = s->skip ? 1 : 0;
        s->ratchet = clampi(s->ratchet, 1, 8);
        s->prob = clampi(s->prob, 0, 100);
        s->accum = clampi(s->accum, -24, 24);
    }
    for (int t = 0; t < NUM_TRACKS; t++) {
        track_cfg_t *tr = &p->tracks[t];
        tr->order = clampi(tr->order, 0, ORD_COUNT - 1);
        tr->length = clampi(tr->length, 1, NUM_STAGES);
        tr->div_i = clampi(tr->div_i, 0, NUM_DIVS - 1);
        tr->swing = clampi(tr->swing, 0, 60);
        tr->gate_pct = clampi(tr->gate_pct, 5, 100);
        tr->transpose = clampi(tr->transpose, -24, 24);
        tr->octave = clampi(tr->octave, -3, 3);
        tr->invert = tr->invert ? 1 : 0;
        tr->mute_audio = tr->mute_audio ? 1 : 0;
    }
    for (int l = 0; l < NUM_MOD_LANES; l++) {
        mod_lane_cfg_t *m = &p->mods[l];
        for (int i = 0; i < NUM_STAGES; i++)
            m->values[i] = clampi(m->values[i], -100, 100);
        m->order = clampi(m->order, 0, ORD_COUNT - 1);
        m->length = clampi(m->length, 1, NUM_STAGES);
        m->div_i = clampi(m->div_i, 0, NUM_DIVS - 1);
        m->depth = clampi(m->depth, 0, 100);
        m->dest = clampi(m->dest, 0, NUM_DESTS - 1);
        p->lane_cc[l] = clampi(p->lane_cc[l], 0, 127);
        p->lane_cc_ch[l] = clampi(p->lane_cc_ch[l], 1, 16);
    }
    p->range_oct = clampi(p->range_oct, MIN_RANGE_OCT, MAX_RANGE_OCT);
}

void pattern_upgrade_v1(pattern_t *p) {
    // pitches were semitones over 36; now positions over PITCH_STEPS at a
    // 3-octave RANGE, which plays exactly the same notes
    for (int i = 0; i < NUM_STAGES; i++)
        p->stages[i].pitch = (uint8_t)rdiv(clampi(p->stages[i].pitch, 0, 36) * PITCH_STEPS, 36);
    p->range_oct = DEF_RANGE_OCT;
    for (int l = 0; l < NUM_MOD_LANES; l++) {
        p->lane_cc[l] = 0;
        p->lane_cc_ch[l] = 1;
    }
}

static void pattern_blank(pattern_t *p) {
    memset(p, 0, sizeof(*p));
    p->bpm = 120;
    for (int i = 0; i < NUM_STAGES; i++) {
        p->stages[i].pulses = 1;
        p->stages[i].gate = GATE_MULT;
        p->stages[i].ratchet = 1;
        p->stages[i].prob = 100;
    }
    for (int t = 0; t < NUM_TRACKS; t++) {
        p->tracks[t].order = ORD_FWD;
        p->tracks[t].length = NUM_STAGES;
        p->tracks[t].gate_pct = 50;
    }
    for (int l = 0; l < NUM_MOD_LANES; l++) {
        p->mods[l].order = ORD_FWD;
        p->mods[l].length = NUM_STAGES;
        p->mods[l].div_i = 2;   // 1/8. - a slow-ish LFO feel
        p->lane_cc_ch[l] = 1;
    }
    p->range_oct = DEF_RANGE_OCT;
}

void pattern_default(pattern_t *p) {
    static const uint8_t pitches[NUM_STAGES] = {0, 3, 5, 7, 10, 7, 5, 3};
    static const uint8_t pulses[NUM_STAGES] = {1, 1, 2, 1, 1, 2, 1, 1};
    static const uint8_t gates[NUM_STAGES] = {GATE_MULT, GATE_MULT, GATE_SING, GATE_MULT,
                                              GATE_MULT, GATE_HOLD, GATE_MULT, GATE_SING};
    static const int8_t tri[NUM_STAGES] = {-100, -60, -20, 20, 60, 100, 40, -40};

    pattern_blank(p);
    for (int i = 0; i < NUM_STAGES; i++) {
        p->stages[i].pitch = (uint8_t)rdiv(pitches[i] * PITCH_STEPS, 12 * DEF_RANGE_OCT);
        p->stages[i].pulses = pulses[i];
        p->stages[i].gate = gates[i];
    }
    // TRK 1: straight forward. TRK 2: reverse, half speed, up an octave.
    p->tracks[0].order = ORD_FWD;
    p->tracks[0].div_i = 0;
    p->tracks[1].order = ORD_REV;
    p->tracks[1].div_i = 1;
    p->tracks[1].octave = 1;
    // MOD lane 1: triangle routed to T1 transpose, depth 0 (inert).
    for (int i = 0; i < NUM_STAGES; i++) p->mods[0].values[i] = tri[i];
    p->mods[0].dest = 1;
    p->mods[0].depth = 0;
    p->mods[0].div_i = 3;
}
