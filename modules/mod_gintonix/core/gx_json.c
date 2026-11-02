// gx_json.c - GinTonix files in the Python version's formats (gx_json.h).
#include "gx_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

static double num(const cJSON *o, const char *key, double def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static bool flag(const cJSON *o, const char *key, bool def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    return def;
}

static const char *str(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

// 5 decimals, like the Python's saved sounds
static double r5(float v) { return round((double)v * 100000.0) / 100000.0; }

// --- patch -----------------------------------------------------------------------------

cJSON *gx_json_patch(const gx_patch_t *p) {
    cJSON *o = cJSON_CreateObject();
    for (int i = 0; i < GX_NKNOB; i++) cJSON_AddNumberToObject(o, GX_KNOBS[i].key, r5(p->k[i]));
    for (int i = 0; i < GX_NSW; i++) cJSON_AddNumberToObject(o, GX_SWITCHES[i].key, p->s[i]);
    return o;
}

void gx_json_to_patch(const cJSON *o, gx_patch_t *p) {
    gx_patch_default(p);
    if (!cJSON_IsObject(o)) return;
    for (int i = 0; i < GX_NKNOB; i++) p->k[i] = clamp01((float)num(o, GX_KNOBS[i].key, p->k[i]));
    for (int i = 0; i < GX_NSW; i++) {
        int v = (int)num(o, GX_SWITCHES[i].key, p->s[i]);
        p->s[i] = (uint8_t)(v >= 0 && v < GX_SWITCHES[i].n ? v : GX_SWITCHES[i].def);
    }
}

// --- state ------------------------------------------------------------------------------------

static cJSON *u16_array(const uint16_t *v, int n) {
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(a, cJSON_CreateNumber(v[i]));
    return a;
}

static void read_u16_array(const cJSON *a, uint16_t *v, int n) {
    for (int i = 0; i < n; i++) {
        const cJSON *x = cJSON_IsArray(a) ? cJSON_GetArrayItem(a, i) : NULL;
        v[i] = cJSON_IsNumber(x) ? (uint16_t)((int)x->valuedouble & 0xFFFF) : 0;
    }
}

cJSON *gx_json_state(const gx_state_t *s) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "v", 1);
    cJSON *a = cJSON_AddArrayToObject(o, "a");
    cJSON *b = cJSON_AddArrayToObject(o, "b");
    cJSON *names = cJSON_AddArrayToObject(o, "names");
    for (int c = 0; c < GX_NCH; c++) {
        cJSON_AddItemToArray(a, gx_json_patch(&s->a[c]));
        cJSON_AddItemToArray(b, gx_json_patch(&s->b[c]));
        cJSON_AddItemToArray(names, cJSON_CreateString(s->names[c]));
    }
    cJSON_AddNumberToObject(o, "morph", r5(s->morph));
    cJSON_AddNumberToObject(o, "master", r5(s->master));
    cJSON *pats = cJSON_AddArrayToObject(o, "pats");
    for (int i = 0; i < GX_NPAT; i++) {
        const gx_pat_t *p = &s->pats[i];
        cJSON *q = cJSON_CreateObject();
        cJSON_AddNumberToObject(q, "len", p->len);
        cJSON_AddItemToObject(q, "trig", u16_array(p->trig, GX_NCH));
        cJSON_AddItemToObject(q, "acc", u16_array(p->acc, GX_NCH));
        cJSON_AddItemToObject(q, "fill", u16_array(p->fill, GX_NCH));
        cJSON_AddBoolToObject(q, "chain", p->chain);
        cJSON_AddItemToArray(pats, q);
    }
    cJSON_AddNumberToObject(o, "rate", s->rate);
    cJSON_AddNumberToObject(o, "swing", r5(s->swing));
    cJSON_AddNumberToObject(o, "fill_rate", s->fill_rate);
    cJSON_AddNumberToObject(o, "tempo", r5(s->tempo));
    cJSON_AddNumberToObject(o, "cur", s->cur);
    return o;
}

void gx_json_to_state(const cJSON *o, gx_state_t *out, bool keep_tempo) {
    float tempo = out->tempo;
    gx_state_init(out);
    if (keep_tempo) out->tempo = tempo;
    if (!cJSON_IsObject(o)) return;
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, "a");
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(o, "b");
    if (!cJSON_IsArray(b)) b = a;   // no morph: B = A
    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(o, "names");
    for (int c = 0; c < GX_NCH; c++) {
        gx_json_to_patch(cJSON_IsArray(a) ? cJSON_GetArrayItem(a, c) : NULL, &out->a[c]);
        gx_json_to_patch(cJSON_IsArray(b) ? cJSON_GetArrayItem(b, c) : NULL, &out->b[c]);
        const cJSON *n = cJSON_IsArray(nm) ? cJSON_GetArrayItem(nm, c) : NULL;
        if (cJSON_IsString(n)) snprintf(out->names[c], sizeof out->names[c], "%s", n->valuestring);
    }
    out->morph = clamp01((float)num(o, "morph", 0));
    float m = (float)num(o, "master", 1.0);
    out->master = m < 0 ? 0 : (m > GX_MASTER_MAX ? GX_MASTER_MAX : m);
    const cJSON *pats = cJSON_GetObjectItemCaseSensitive(o, "pats");
    for (int i = 0; i < GX_NPAT; i++) {
        const cJSON *q = cJSON_IsArray(pats) ? cJSON_GetArrayItem(pats, i) : NULL;
        gx_pat_t *p = &out->pats[i];
        gx_pat_clear(p);
        if (!cJSON_IsObject(q)) continue;
        int len = (int)num(q, "len", 16);
        p->len = (uint8_t)(len < 1 ? 1 : (len > GX_NSTEP ? GX_NSTEP : len));
        read_u16_array(cJSON_GetObjectItemCaseSensitive(q, "trig"), p->trig, GX_NCH);
        read_u16_array(cJSON_GetObjectItemCaseSensitive(q, "acc"), p->acc, GX_NCH);
        read_u16_array(cJSON_GetObjectItemCaseSensitive(q, "fill"), p->fill, GX_NCH);
        p->chain = flag(q, "chain", false);
    }
    out->pats[GX_NPAT - 1].chain = 0;
    out->rate = (uint8_t)(((int)num(o, "rate", 2) % GX_NRATES + GX_NRATES) % GX_NRATES);
    out->swing = clamp01((float)num(o, "swing", 0));
    out->fill_rate = (uint8_t)(((int)num(o, "fill_rate", 0) % GX_NFILLS + GX_NFILLS) % GX_NFILLS);
    if (!keep_tempo) {
        float t = (float)num(o, "tempo", out->tempo);
        out->tempo = t < 20 ? 20 : (t > 300 ? 300 : t);
    }
    out->cur = (uint8_t)(((int)num(o, "cur", 0) % GX_NPAT + GX_NPAT) % GX_NPAT);
}

// --- Microtonic kits (kits/*.json, converted from .mtpreset) -------------------------------

static float kf(double hz) { return clamp01((float)(log((hz > 20 ? hz : 20) / 20.0) / log(1000.0))); }
static float kd(double ms) { return clamp01((float)(log((ms > 10 ? ms : 10) / 10.0) / log(1000.0))); }
static float ka(double ms) { return clamp01((float)pow((ms > 0 ? ms : 0) / 10000.0, 1.0 / 3.0)); }

static int pick(const char *v, const char *const *names, int n, int def) {
    if (!v) return def;
    for (int i = 0; i < n; i++)
        if (strcmp(v, names[i]) == 0) return i;
    return def;
}

// g(key, default) in the Python: null counts as missing
static double g(const cJSON *d, const char *key, double def) { return num(d, key, def); }

void gx_json_mt_patch(const cJSON *d, gx_patch_t *p) {
    static const char *const WAVES[] = {"Sine", "Triangle", "Saw"};
    static const char *const MODS[] = {"Decay", "Sine", "Noise"};
    static const char *const FILTS[] = {"LP", "BP", "HP"};
    static const char *const ENVS[] = {"Exp", "Linear", "Mod"};
    gx_patch_default(p);
    p->s[S_WAVE] = (uint8_t)pick(str(d, "OscWave"), WAVES, 3, 0);
    p->k[K_OSC_FREQ] = kf(g(d, "OscFreq", 632.46));
    int mode = pick(str(d, "ModMode"), MODS, 3, 0);
    p->s[S_MOD_MODE] = (uint8_t)mode;
    double st = g(d, "ModAmt", 0.0);
    const cJSON *rv = cJSON_GetObjectItemCaseSensitive(d, "ModRate");
    // ModRate: missing = the default, null = "never" (as in the Python)
    bool rnull = rv && cJSON_IsNull(rv);
    double r = cJSON_IsNumber(rv) ? rv->valuedouble : 353.33;
    if (mode == 0) {
        p->k[K_MOD_AMT] = clamp01((float)(0.5 + st / 192.0));
        if (rnull || r > 12000)
            p->k[K_MOD_RATE] = 0.0f;
        else
            p->k[K_MOD_RATE] = clamp01((float)(1.0 - log((r > 10 ? r : 10) / 10.0) / log(1248.0)));
    } else {
        p->k[K_MOD_AMT] = clamp01((float)(0.5 + st / 96.0));
        if (rnull || r <= 0)
            p->k[K_MOD_RATE] = 0.0f;
        else if (mode == 1)
            p->k[K_MOD_RATE] = clamp01((float)(log((r > 0.1 ? r : 0.1) / 0.1) / log(20000.0)));
        else
            p->k[K_MOD_RATE] = clamp01((float)(log(r > 1 ? r : 1) / log(20000.0)));
    }
    p->k[K_OSC_ATT] = ka(g(d, "OscAtk", 0.0));
    p->k[K_OSC_DEC] = kd(g(d, "OscDcy", 316.23));
    p->s[S_N_MODE] = (uint8_t)pick(str(d, "NFilMod"), FILTS, 3, 0);
    p->k[K_N_FREQ] = kf(g(d, "NFilFrq", 20000.0));
    double q = g(d, "NFilQ", 0.707);
    if (q < 0.1) q = 0.1;
    p->k[K_N_Q] = clamp01((float)(log(q / 0.1) / log(10.0) / 5.0));
    int env = pick(str(d, "NEnvMod"), ENVS, 3, 0);
    p->s[S_N_ENV] = (uint8_t)env;
    p->k[K_N_ATT] = ka(g(d, "NEnvAtk", 0.0));
    double nd = g(d, "NEnvDcy", 316.23);
    p->k[K_N_DEC] = kd(env == 1 ? nd * 1.5 : nd);
    p->k[K_MIX] = clamp01((float)(1.0 - g(d, "Mix", 50.0) / 100.0));   // Microtonic: osc share
    p->k[K_EQ_FREQ] = kf(g(d, "EQFreq", 632.46));
    p->k[K_EQ_GAIN] = clamp01((float)(0.5 + g(d, "EQGain", 0.0) / 80.0));
    p->k[K_DIST] = clamp01((float)(g(d, "DistAmt", 0.0) / 100.0));
    double lv = g(d, "Level", 0.0);
    p->k[K_LEVEL] = lv <= -59.0 ? 0.0f : clamp01((float)(GX_LEVEL_0DB * pow(10.0, lv / 40.0)));
    p->k[K_PAN] = clamp01((float)(0.5 + g(d, "Pan", 0.0) / 200.0));
    p->k[K_VEL_OSC] = clamp01((float)(g(d, "OscVel", 0.0) / 200.0));
    p->k[K_VEL_NOISE] = clamp01((float)(g(d, "NVel", 0.0) / 200.0));
    p->k[K_VEL_MOD] = clamp01((float)(g(d, "ModVel", 0.0) / 200.0));
    const char *ch = str(d, "Choke");
    p->s[S_CHOKE] = ch && strcmp(ch, "On") == 0;
}

void gx_json_mt_kit(const cJSON *k, gx_state_t *out) {
    float tempo = out->tempo;
    gx_state_init(out);
    out->tempo = tempo;
    const cJSON *drums = cJSON_GetObjectItemCaseSensitive(k, "drums");
    for (int c = 0; c < GX_NCH; c++) {
        const cJSON *d = cJSON_IsArray(drums) ? cJSON_GetArrayItem(drums, c) : NULL;
        if (cJSON_IsObject(d)) {
            gx_json_mt_patch(d, &out->a[c]);
            const char *n = str(d, "Name");
            snprintf(out->names[c], sizeof out->names[c], "%s", n ? n : "Drum");
        } else {
            gx_patch_default(&out->a[c]);
            snprintf(out->names[c], sizeof out->names[c], "Init");
        }
        out->b[c] = out->a[c];
    }
    const cJSON *pats = cJSON_GetObjectItemCaseSensitive(k, "pats");
    for (int i = 0; i < GX_NPAT; i++) {
        const cJSON *q = cJSON_IsArray(pats) ? cJSON_GetArrayItem(pats, i) : NULL;
        gx_pat_t *p = &out->pats[i];
        gx_pat_clear(p);
        if (!cJSON_IsObject(q)) continue;
        int len = (int)num(q, "len", 16);
        p->len = (uint8_t)(len < 1 ? 1 : (len > GX_NSTEP ? GX_NSTEP : len));
        p->chain = flag(q, "chain", false);
        read_u16_array(cJSON_GetObjectItemCaseSensitive(q, "t"), p->trig, GX_NCH);
        read_u16_array(cJSON_GetObjectItemCaseSensitive(q, "a"), p->acc, GX_NCH);
        read_u16_array(cJSON_GetObjectItemCaseSensitive(q, "f"), p->fill, GX_NCH);
    }
    out->pats[GX_NPAT - 1].chain = 0;
    const char *rate = str(k, "rate");
    out->rate = (uint8_t)pick(rate, GX_RATE_NAMES, GX_NRATES, 2);
    int fr = (int)(num(k, "fill", 2) + 0.5);
    if (fr < GX_FILL_RATES[0]) fr = GX_FILL_RATES[0];
    if (fr > GX_FILL_RATES[GX_NFILLS - 1]) fr = GX_FILL_RATES[GX_NFILLS - 1];
    out->fill_rate = (uint8_t)(fr - GX_FILL_RATES[0]);
    // MastVol + the loudness trim worked out when the kit was converted
    double mv = num(k, "master", 0.0) + num(k, "trim_db", 0.0);
    float m = (float)pow(10.0, mv / 20.0);
    out->master = m > GX_MASTER_MAX ? GX_MASTER_MAX : m;
    out->swing = clamp01((float)(num(k, "swing", 0.0) / 100.0));
    double t = num(k, "tempo", 120.0);
    out->tempo = (float)(t < 20 ? 20 : (t > 300 ? 300 : t));
    const char *pn = str(k, "pattern");
    out->cur = 0;
    if (pn && pn[0] && !pn[1]) {
        char u = pn[0] >= 'a' && pn[0] <= 'z' ? (char)(pn[0] - 32) : pn[0];
        const char *f = strchr(GX_PAT_NAMES, u);
        if (f && u) out->cur = (uint8_t)(f - GX_PAT_NAMES);
    }
}

// --- files --------------------------------------------------------------------------------------

cJSON *gx_json_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 512 * 1024) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    cJSON *o = cJSON_Parse(buf);
    free(buf);
    return o;
}

bool gx_json_save(const char *path, const cJSON *o) {
    char *txt = cJSON_PrintUnformatted(o);
    if (!txt) return false;
    // write a temp file and rename it into place: a power cut mid-save
    // leaves the old file, never half of the new one
    char tmp[160];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = false;
    if (f) {
        size_t n = strlen(txt);
        ok = fwrite(txt, 1, n, f) == n;
        ok = (fclose(f) == 0) && ok;
        if (ok) {
            remove(path);
            ok = rename(tmp, path) == 0;
        } else {
            remove(tmp);
        }
    }
    cJSON_free(txt);
    return ok;
}

bool gx_load_kit_file(const char *path, gx_state_t *out, bool keep_tempo) {
    cJSON *o = gx_json_load(path);
    if (!o) return false;
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(o, "state");
    if (cJSON_IsObject(st)) {
        gx_json_to_state(st, out, keep_tempo);
    } else {
        float t = out->tempo;
        gx_json_mt_kit(o, out);
        if (keep_tempo) out->tempo = t;
    }
    cJSON_Delete(o);
    return true;
}

bool gx_bank_load(const char *path, gx_state_t *out, char *name, int name_len, bool keep_tempo) {
    cJSON *o = gx_json_load(path);
    if (!cJSON_IsObject(o)) {
        cJSON_Delete(o);
        return false;
    }
    gx_json_to_state(cJSON_GetObjectItemCaseSensitive(o, "state"), out, keep_tempo);
    const char *n = str(o, "name");
    if (name) snprintf(name, name_len, "%s", n ? n : "");
    cJSON_Delete(o);
    return true;
}

bool gx_bank_read_name(const char *path, char *name, int name_len) {
    cJSON *o = gx_json_load(path);
    if (!cJSON_IsObject(o)) {
        cJSON_Delete(o);
        return false;
    }
    const char *n = str(o, "name");
    snprintf(name, name_len, "%s", n ? n : "");
    cJSON_Delete(o);
    return true;
}

bool gx_bank_save(const char *path, const char *name, const gx_state_t *s) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddItemToObject(o, "state", gx_json_state(s));
    bool ok = gx_json_save(path, o);
    cJSON_Delete(o);
    return ok;
}

int gx_drums_load(const char *path, gx_user_drum_t *out, int max) {
    cJSON *o = gx_json_load(path);
    int n = 0;
    if (cJSON_IsArray(o)) {
        const cJSON *d;
        cJSON_ArrayForEach(d, o) {
            if (n >= max) break;
            const char *nm = str(d, "name");
            const cJSON *a = cJSON_GetObjectItemCaseSensitive(d, "a");
            if (!nm || !cJSON_IsObject(a)) continue;
            snprintf(out[n].name, sizeof out[n].name, "%s", nm);
            gx_json_to_patch(a, &out[n].a);
            const cJSON *b = cJSON_GetObjectItemCaseSensitive(d, "b");
            if (cJSON_IsObject(b))
                gx_json_to_patch(b, &out[n].b);
            else
                out[n].b = out[n].a;
            n++;
        }
    }
    cJSON_Delete(o);
    return n;
}

bool gx_drums_save(const char *path, const gx_user_drum_t *d, int n) {
    cJSON *o = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", d[i].name);
        cJSON_AddItemToObject(e, "a", gx_json_patch(&d[i].a));
        if (memcmp(&d[i].a, &d[i].b, sizeof d[i].a) != 0)
            cJSON_AddItemToObject(e, "b", gx_json_patch(&d[i].b));
        cJSON_AddItemToArray(o, e);
    }
    bool ok = gx_json_save(path, o);
    cJSON_Delete(o);
    return ok;
}
