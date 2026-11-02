// d9_json.c - Drumster9 projects as JSON (see d9_json.h).
#include "d9_json.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 4 places, so 0.15 reads 0.15 (not 0.15000000596)
static double r4(float v) { return round((double)v * 10000.0) / 10000.0; }

cJSON *d9_steps_json(uint32_t m) {
    cJSON *a = cJSON_CreateArray();
    for (int k = 0; k < D9_STEPS; k++)
        if (m & (1u << k)) cJSON_AddItemToArray(a, cJSON_CreateNumber(k));
    return a;
}

uint32_t d9_steps_from_json(const cJSON *a) {
    uint32_t m = 0;
    const cJSON *v;
    cJSON_ArrayForEach(v, a) {
        if (cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble < D9_STEPS) m |= 1u << (int)v->valuedouble;
    }
    return m;
}

static const struct {
    const char *key;
    size_t off;
} FX_KEYS[] = {
    {"rev_level", offsetof(d9_bus_t, rev_level)},   {"rev_liveness", offsetof(d9_bus_t, rev_live)},
    {"rev_damping", offsetof(d9_bus_t, rev_damp)},  {"echo_level", offsetof(d9_bus_t, echo_level)},
    {"echo_ms", offsetof(d9_bus_t, echo_ms)},       {"echo_fb", offsetof(d9_bus_t, echo_fb)},
    {"echo_tone", offsetof(d9_bus_t, echo_tone)},   {"eq_l", offsetof(d9_bus_t, eq_l)},
    {"eq_m", offsetof(d9_bus_t, eq_m)},             {"eq_h", offsetof(d9_bus_t, eq_h)},
    {"filter_freq", offsetof(d9_bus_t, freq)},      {"resonance", offsetof(d9_bus_t, reso)},
    {"drive", offsetof(d9_bus_t, drive)},
};
#define NFX (sizeof FX_KEYS / sizeof FX_KEYS[0])

static int role_index(const char *name) {
    for (int l = 0; l < D9_LANES; l++)
        if (!strcmp(name, D9_ROLE[l])) return l;
    return -1;
}

cJSON *d9_project_to_json(const d9_state_t *s, const char *name, float bpm) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", name ? name : "");
    cJSON_AddNumberToObject(o, "bpm", r4(bpm));
    cJSON_AddNumberToObject(o, "kit", s->kit);
    cJSON *lanes = cJSON_AddArrayToObject(o, "lanes");
    for (int l = 0; l < D9_LANES; l++) {
        cJSON *x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "name", D9_ROLE[l]);
        cJSON_AddItemToObject(x, "steps", d9_steps_json(s->mask[l]));
        cJSON_AddNumberToObject(x, "vol", r4(s->vol[l]));
        cJSON_AddBoolToObject(x, "muted", s->muted[l]);
        cJSON_AddBoolToObject(x, "solo", s->solo[l]);
        if (s->kit_ovr[l] >= 0)
            cJSON_AddNumberToObject(x, "kit_override", s->kit_ovr[l]);
        else
            cJSON_AddNullToObject(x, "kit_override");
        cJSON_AddNumberToObject(x, "bus", s->bus[l]);
        cJSON_AddItemToArray(lanes, x);
    }
    cJSON *fx = cJSON_AddArrayToObject(o, "bus_fx");
    for (int b = 0; b < D9_BUSES; b++) {
        cJSON *x = cJSON_CreateObject();
        for (size_t k = 0; k < NFX; k++)
            cJSON_AddNumberToObject(x, FX_KEYS[k].key, r4(*(const float *)((const char *)&s->fx[b] + FX_KEYS[k].off)));
        cJSON_AddNumberToObject(x, "filter_type", s->fx[b].filter);
        cJSON_AddItemToArray(fx, x);
    }
    cJSON *bank = cJSON_AddArrayToObject(o, "bank");
    for (int i = 0; i < D9_BANKS; i++) {
        const d9_slot_t *k = &s->bank[i];
        if (!k->used) {
            cJSON_AddItemToArray(bank, cJSON_CreateNull());
            continue;
        }
        cJSON *x = cJSON_CreateObject();
        cJSON *st = cJSON_AddObjectToObject(x, "steps");
        for (int l = 0; l < D9_LANES; l++) cJSON_AddItemToObject(st, D9_ROLE[l], d9_steps_json(k->m[l]));
        cJSON_AddNumberToObject(x, "kit", k->kit);
        cJSON_AddNumberToObject(x, "bpm", r4(bpm));
        cJSON_AddItemToArray(bank, x);
    }
    cJSON_AddNumberToObject(o, "bank_active", s->bank_active);
    cJSON_AddNumberToObject(o, "master_vol", r4(s->master));
    // only what differs from the kit's own samples, as the Python kept it
    cJSON *ko = cJSON_AddObjectToObject(o, "kit_overrides");
    for (int k = 0; k < D9_KITS; k++) {
        cJSON *roles = NULL;
        for (int l = 0; l < D9_LANES; l++) {
            if (s->kit_map[k][l] == D9_KIT[k].s[l]) continue;
            if (!roles) {
                char key[4];
                snprintf(key, sizeof key, "%d", k);
                roles = cJSON_AddObjectToObject(ko, key);
            }
            cJSON_AddNumberToObject(roles, D9_ROLE[l], s->kit_map[k][l]);
        }
    }
    return o;
}

static bool num(const cJSON *o, const char *k, double *v) {
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(x)) return false;
    *v = x->valuedouble;
    return true;
}

static bool is_int(double v, int lo, int hi) { return v == floor(v) && v >= lo && v <= hi; }

void d9_project_from_json(const cJSON *o, d9_state_t *s, float *bpm) {
    d9_state_init(s);
    *bpm = 0;
    if (!cJSON_IsObject(o)) return;
    double v;
    if (num(o, "master_vol", &v) && v >= 0) s->master = (float)v;
    // the KITS page's samples, per kit and role
    const cJSON *ko = cJSON_GetObjectItemCaseSensitive(o, "kit_overrides");
    const cJSON *kit;
    cJSON_ArrayForEach(kit, ko) {
        if (!kit->string) continue;
        char *end;
        long k = strtol(kit->string, &end, 10);
        if (*end || k < 0 || k >= D9_KITS || !cJSON_IsObject(kit)) continue;
        const cJSON *role;
        cJSON_ArrayForEach(role, kit) {
            int l = role->string ? role_index(role->string) : -1;
            if (l >= 0 && cJSON_IsNumber(role) && is_int(role->valuedouble, -1, 4095))
                s->kit_map[k][l] = (int16_t)role->valuedouble;
        }
    }
    if (num(o, "kit", &v) && is_int(v, 0, D9_KITS - 1)) s->kit = (int8_t)v;
    if (num(o, "bpm", &v) && v > 0) *bpm = (float)v;
    const cJSON *fx = cJSON_GetObjectItemCaseSensitive(o, "bus_fx");
    for (int b = 0; b < D9_BUSES && cJSON_IsArray(fx); b++) {
        const cJSON *x = cJSON_GetArrayItem(fx, b);
        if (!cJSON_IsObject(x)) continue;
        for (size_t k = 0; k < NFX; k++)
            if (num(x, FX_KEYS[k].key, &v)) *(float *)((char *)&s->fx[b] + FX_KEYS[k].off) = (float)v;
        if (num(x, "filter_type", &v) && is_int(v, 0, 2)) s->fx[b].filter = (int)v;
    }
    const cJSON *lanes = cJSON_GetObjectItemCaseSensitive(o, "lanes");
    if (cJSON_IsArray(lanes)) {
        const cJSON *x;
        cJSON_ArrayForEach(x, lanes) {
            const cJSON *nm = cJSON_GetObjectItemCaseSensitive(x, "name");
            int l = cJSON_IsString(nm) ? role_index(nm->valuestring) : -1;
            if (l < 0) continue;
            const cJSON *ov = cJSON_GetObjectItemCaseSensitive(x, "kit_override");
            if (cJSON_IsNumber(ov) && is_int(ov->valuedouble, 0, D9_KITS - 1)) s->kit_ovr[l] = (int8_t)ov->valuedouble;
            if (num(x, "bus", &v) && is_int(v, 0, D9_BUSES - 1)) s->bus[l] = (uint8_t)v;
            if (num(x, "vol", &v)) s->vol[l] = (float)v;
            s->muted[l] = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(x, "muted"));
            s->solo[l] = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(x, "solo"));
            s->mask[l] = d9_steps_from_json(cJSON_GetObjectItemCaseSensitive(x, "steps"));
        }
    } else {
        // the oldest files: flat {"steps": {"Kick": [...]}}
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(o, "steps");
        for (int l = 0; l < D9_LANES; l++)
            s->mask[l] = d9_steps_from_json(cJSON_GetObjectItemCaseSensitive(st, D9_ROLE[l]));
    }
    const cJSON *bank = cJSON_GetObjectItemCaseSensitive(o, "bank");
    for (int i = 0; i < D9_BANKS && cJSON_IsArray(bank); i++) {
        const cJSON *x = cJSON_GetArrayItem(bank, i);
        if (!cJSON_IsObject(x)) continue;
        d9_slot_t *k = &s->bank[i];
        k->used = 1;
        k->kit = s->kit;
        if (num(x, "kit", &v) && is_int(v, 0, D9_KITS - 1)) k->kit = (int8_t)v;
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(x, "steps");
        for (int l = 0; l < D9_LANES; l++)
            k->m[l] = d9_steps_from_json(cJSON_GetObjectItemCaseSensitive(st, D9_ROLE[l]));
    }
    if (num(o, "bank_active", &v) && is_int(v, 0, D9_BANKS - 1)) s->bank_active = (int8_t)v;
    // the Python loaded the active slot onto the grid last
    if (s->bank[s->bank_active].used) {
        memcpy(s->mask, s->bank[s->bank_active].m, sizeof s->mask);
        s->kit = s->bank[s->bank_active].kit;
    } else {
        d9_slot_store(s);
    }
    d9_state_clamp(s);
}
