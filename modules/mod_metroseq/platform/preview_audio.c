// preview_audio.c - two-voice preview synth, rendered into the Studio mix.
//
// Mirrors the Python audio_init() voice: downward saw, low-pass at 2.5 kHz
// (resonance 1.2), envelope 1.0 -> 0.4 over 30 ms, 120 ms release, amp 0.4
// at velocity 0.5.
#include "preview_audio.h"

#include <math.h>
#include <string.h>

#include "engine/seq_model.h"

#define SR STUDIO_SR

typedef struct {
    volatile int32_t note;    // midi, or -1 for off
    volatile uint32_t gen;    // bumps on every note-on (retrigger)
    // render-side state
    uint32_t seen_gen;
    bool on;
    float phase, inc, env, lp, bp;
} voice_t;

static voice_t s_v[NUM_TRACKS];
static volatile bool s_enabled = true;

void preview_note_on(int track, int midi) {
    if (!s_enabled || track < 0 || track >= NUM_TRACKS) return;
    s_v[track].note = midi;
    s_v[track].gen++;
}

void preview_note_off(int track) {
    if (track >= 0 && track < NUM_TRACKS) s_v[track].note = -1;
}

void preview_set_enabled(bool on) {
    s_enabled = on;
    if (!on)
        for (int t = 0; t < NUM_TRACKS; t++) s_v[t].note = -1;
}

bool preview_enabled(void) { return s_enabled; }

void preview_init(void) {
    for (int t = 0; t < NUM_TRACKS; t++) s_v[t].note = -1;
}

void preview_render(float *l, float *r, const studio_block_t *b) {
    const float att = 1.0f / (0.030f * SR);          // 1.0 -> 0.4 in 30 ms
    const float rel = 0.4f / (0.120f * SR);          // 0.4 -> 0 in 120 ms
    static float f, q;
    if (f == 0.0f) {
        f = 2.0f * sinf((float)M_PI * 2500.0f / SR);   // SVF cutoff
        q = 1.0f / 1.2f;
    }
    for (int t = 0; t < NUM_TRACKS; t++) {
        voice_t *v = &s_v[t];
        int32_t note = v->note;
        uint32_t gen = v->gen;
        if (note >= 0 && gen != v->seen_gen) {
            v->seen_gen = gen;
            v->on = true;
            v->env = 1.0f;
            v->inc = 440.0f * powf(2.0f, (note - 69) / 12.0f) / SR;
        } else if (note < 0) {
            v->on = false;
        }
    }
    for (int t = 0; t < NUM_TRACKS; t++) {
        voice_t *v = &s_v[t];
        if (!v->on && v->env <= 0.0f) continue;
        for (int i = 0; i < b->n; i++) {
            if (v->on) {
                if (v->env > 0.4f) v->env -= att * 0.6f;
            } else if (v->env > 0.0f) {
                v->env -= rel;
                if (v->env < 0.0f) v->env = 0.0f;
            }
            v->phase += v->inc;
            if (v->phase >= 1.0f) v->phase -= 1.0f;
            float saw = 1.0f - 2.0f * v->phase;   // SAW_DOWN
            // Chamberlin state-variable low-pass
            v->lp += f * v->bp;
            float hp = saw - v->lp - q * v->bp;
            v->bp += f * hp;
            float s = v->lp * v->env * 0.2f;   // amp 0.4 * vel 0.5
            l[i] += s;
            r[i] += s;
        }
    }
}
