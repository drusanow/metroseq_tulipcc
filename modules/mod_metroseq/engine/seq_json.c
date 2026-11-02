// seq_json.c - a metroseq pattern as JSON (see seq_json.h).
#include "seq_json.h"

static void num(cJSON *o, const char *k, int v) { cJSON_AddNumberToObject(o, k, v); }

static int get(const cJSON *o, const char *k, int def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

cJSON *pattern_to_json(const pattern_t *p) {
    cJSON *o = cJSON_CreateObject();
    num(o, "v", PATTERN_VERSION);
    num(o, "bpm", p->bpm);
    num(o, "root", p->root);
    num(o, "scale", p->scale_idx);
    num(o, "base_oct", p->base_octave);
    num(o, "range", p->range_oct);
    cJSON_AddBoolToObject(o, "merge_cv", p->merge_cv);
    cJSON *a = cJSON_AddArrayToObject(o, "stages");
    for (int i = 0; i < NUM_STAGES; i++) {
        const stage_t *s = &p->stages[i];
        cJSON *x = cJSON_CreateObject();
        num(x, "pitch", s->pitch);
        num(x, "pulses", s->pulses);
        num(x, "gate", s->gate);
        cJSON_AddBoolToObject(x, "slide", s->slide);
        cJSON_AddBoolToObject(x, "skip", s->skip);
        num(x, "ratchet", s->ratchet);
        num(x, "prob", s->prob);
        num(x, "accum", s->accum);
        cJSON_AddItemToArray(a, x);
    }
    a = cJSON_AddArrayToObject(o, "tracks");
    for (int t = 0; t < NUM_TRACKS; t++) {
        const track_cfg_t *tr = &p->tracks[t];
        cJSON *x = cJSON_CreateObject();
        num(x, "order", tr->order);
        num(x, "length", tr->length);
        num(x, "div", tr->div_i);
        num(x, "swing", tr->swing);
        num(x, "gate_pct", tr->gate_pct);
        num(x, "transpose", tr->transpose);
        num(x, "octave", tr->octave);
        cJSON_AddBoolToObject(x, "invert", tr->invert);
        cJSON_AddBoolToObject(x, "mute_audio", tr->mute_audio);
        cJSON_AddItemToArray(a, x);
    }
    a = cJSON_AddArrayToObject(o, "mods");
    for (int l = 0; l < NUM_MOD_LANES; l++) {
        const mod_lane_cfg_t *m = &p->mods[l];
        cJSON *x = cJSON_CreateObject();
        cJSON *v = cJSON_AddArrayToObject(x, "values");
        for (int i = 0; i < NUM_STAGES; i++) cJSON_AddItemToArray(v, cJSON_CreateNumber(m->values[i]));
        num(x, "order", m->order);
        num(x, "length", m->length);
        num(x, "div", m->div_i);
        num(x, "depth", m->depth);
        num(x, "dest", m->dest);
        num(x, "cc", p->lane_cc[l]);
        num(x, "cc_ch", p->lane_cc_ch[l]);
        cJSON_AddItemToArray(a, x);
    }
    return o;
}

void pattern_from_json(const cJSON *o, pattern_t *p) {
    pattern_default(p);
    if (!cJSON_IsObject(o)) return;
    p->bpm = get(o, "bpm", p->bpm);
    p->root = get(o, "root", p->root);
    p->scale_idx = get(o, "scale", p->scale_idx);
    p->base_octave = get(o, "base_oct", p->base_octave);
    p->range_oct = get(o, "range", p->range_oct);
    p->merge_cv = get(o, "merge_cv", p->merge_cv);
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, "stages");
    for (int i = 0; i < NUM_STAGES; i++) {
        const cJSON *x = cJSON_GetArrayItem(a, i);
        if (!cJSON_IsObject(x)) continue;
        stage_t *s = &p->stages[i];
        s->pitch = get(x, "pitch", s->pitch);
        s->pulses = get(x, "pulses", s->pulses);
        s->gate = get(x, "gate", s->gate);
        s->slide = get(x, "slide", s->slide);
        s->skip = get(x, "skip", s->skip);
        s->ratchet = get(x, "ratchet", s->ratchet);
        s->prob = get(x, "prob", s->prob);
        s->accum = get(x, "accum", s->accum);
    }
    a = cJSON_GetObjectItemCaseSensitive(o, "tracks");
    for (int t = 0; t < NUM_TRACKS; t++) {
        const cJSON *x = cJSON_GetArrayItem(a, t);
        if (!cJSON_IsObject(x)) continue;
        track_cfg_t *tr = &p->tracks[t];
        tr->order = get(x, "order", tr->order);
        tr->length = get(x, "length", tr->length);
        tr->div_i = get(x, "div", tr->div_i);
        tr->swing = get(x, "swing", tr->swing);
        tr->gate_pct = get(x, "gate_pct", tr->gate_pct);
        tr->transpose = get(x, "transpose", tr->transpose);
        tr->octave = get(x, "octave", tr->octave);
        tr->invert = get(x, "invert", tr->invert);
        tr->mute_audio = get(x, "mute_audio", tr->mute_audio);
    }
    a = cJSON_GetObjectItemCaseSensitive(o, "mods");
    for (int l = 0; l < NUM_MOD_LANES; l++) {
        const cJSON *x = cJSON_GetArrayItem(a, l);
        if (!cJSON_IsObject(x)) continue;
        mod_lane_cfg_t *m = &p->mods[l];
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(x, "values");
        for (int i = 0; i < NUM_STAGES; i++) {
            const cJSON *n = cJSON_GetArrayItem(v, i);
            if (cJSON_IsNumber(n)) m->values[i] = (int8_t)clampi(n->valueint, -100, 100);
        }
        m->order = get(x, "order", m->order);
        m->length = get(x, "length", m->length);
        m->div_i = get(x, "div", m->div_i);
        m->depth = get(x, "depth", m->depth);
        m->dest = get(x, "dest", m->dest);
        p->lane_cc[l] = get(x, "cc", p->lane_cc[l]);
        p->lane_cc_ch[l] = get(x, "cc_ch", p->lane_cc_ch[l]);
    }
    pattern_sanitise(p);
}
