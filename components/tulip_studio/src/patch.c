// patch.c - Studio patches: the whole session in one JSON file.
//
//   {"studio_patch": 1, "name": "Live 1", "bpm": 120, "volume": 8,
//    "apps": {
//      "metroseq": {"open": true, "level": 80, "mute": false,
//                   "routes": [{"to": "sloe"}, {"to": "midi", "ch": 2}],
//                   "state": { ...metroseq's own patch_save()... }},
//      "gintonix": {...}, ...}}
//
// Apps are matched by name, so a patch made in one Studio image loads in
// another with a different set of apps: what's missing is skipped. The
// clock source and touch calibration belong to the device, not the patch.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "studio_priv.h"

static bool dir(char *out, int len) {
    const char *root = studio_files_root();
    if (!root) return false;
    char d[48];
    snprintf(d, sizeof d, "%s/studio", root);
    mkdir(d, 0777);
    snprintf(out, len, "%s/studio/patches", root);
    mkdir(out, 0777);
    return true;
}

// A name as a file name: letters, digits, space, - and _ only.
static bool path_of(const char *name, char *out, int len) {
    char d[64], nm[STUDIO_PATCH_NAME + 1];
    int n = 0;
    for (const char *p = name; *p && n < STUDIO_PATCH_NAME; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == ' ' || c == '-' || c == '_';
        nm[n++] = ok ? c : '_';
    }
    nm[n] = 0;
    if (!n || !dir(d, sizeof d)) return false;
    snprintf(out, len, "%s/%s.json", d, nm);
    return true;
}

static int cmp(const void *a, const void *b) { return strcasecmp(a, b); }

int studio_patch_list(char (*names)[STUDIO_PATCH_NAME + 1], int max) {
    char d[64];
    if (!dir(d, sizeof d)) return 0;
    DIR *dp = opendir(d);
    if (!dp) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dp)) && n < max) {
        const char *nm = e->d_name;
        size_t l = strlen(nm);
        if (l <= 5 || l - 5 > STUDIO_PATCH_NAME || strcasecmp(nm + l - 5, ".json")) continue;
        snprintf(names[n], STUDIO_PATCH_NAME + 1, "%.*s", (int)(l - 5), nm);
        n++;
    }
    closedir(dp);
    qsort(names, n, sizeof names[0], cmp);
    return n;
}

// --- to / from JSON ------------------------------------------------------------------------

static void dest_json(cJSON *o, int d, int ch) {
    if (d == ROUTE_MIDI_OUT) {
        cJSON_AddStringToObject(o, "to", "midi");
        cJSON_AddNumberToObject(o, "ch", ch);
    } else if (d >= 0 && d < g_nmods) {
        cJSON_AddStringToObject(o, "to", g_mods[d]->name);
    } else {
        cJSON_AddStringToObject(o, "to", "own");
    }
}

cJSON *studio_patch_to_json(const char *name) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "studio_patch", 1);
    cJSON_AddStringToObject(o, "name", name ? name : "");
    cJSON_AddNumberToObject(o, "bpm", studio_bpm_x100() / 100.0);
    cJSON_AddNumberToObject(o, "volume", studio_volume());
    cJSON *apps = cJSON_AddObjectToObject(o, "apps");
    for (int i = 0; i < g_nmods; i++) {
        const tmod_t *m = g_mods[i];
        cJSON *a = cJSON_AddObjectToObject(apps, m->name);
        cJSON_AddBoolToObject(a, "open", g_enabled[i]);
        if (m->flags & TMOD_F_AUDIO) {
            cJSON_AddNumberToObject(a, "level", studio_mix_level(i));
            cJSON_AddBoolToObject(a, "mute", studio_mix_muted(i));
        }
        if (m->n_note_outs) {
            cJSON *r = cJSON_AddArrayToObject(a, "routes");
            for (int k = 0; k < m->n_note_outs; k++) {
                cJSON *x = cJSON_CreateObject();
                dest_json(x, studio_route_get(i, k), studio_route_midi_ch(i, k));
                cJSON_AddItemToArray(r, x);
            }
        }
        // closed apps keep their settings too: they come back when opened
        if (m->patch_save) {
            cJSON *st = m->patch_save();
            if (st) cJSON_AddItemToObject(a, "state", st);
        }
    }
    return o;
}

static int num(const cJSON *o, const char *k, int def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

void studio_patch_from_json(const cJSON *o) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "bpm");
    if (cJSON_IsNumber(v)) studio_set_bpm_x100((int32_t)(v->valuedouble * 100 + 0.5));
    v = cJSON_GetObjectItemCaseSensitive(o, "volume");
    if (cJSON_IsNumber(v)) studio_set_volume(v->valueint);
    const cJSON *apps = cJSON_GetObjectItemCaseSensitive(o, "apps");
    for (int i = 0; i < g_nmods; i++) {
        const tmod_t *m = g_mods[i];
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(apps, m->name);
        if (!cJSON_IsObject(a)) continue;
        // the app's own state first, so it's ready the moment it opens
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(a, "state");
        if (m->patch_load && cJSON_IsObject(st)) m->patch_load(st);
        if (studio_multi()) {
            v = cJSON_GetObjectItemCaseSensitive(a, "open");
            if (cJSON_IsBool(v)) shell_set_open(i, cJSON_IsTrue(v));
        }
        if (m->flags & TMOD_F_AUDIO) {
            studio_mix_set_level(i, num(a, "level", studio_mix_level(i)));
            v = cJSON_GetObjectItemCaseSensitive(a, "mute");
            if (cJSON_IsBool(v)) studio_mix_set_mute(i, cJSON_IsTrue(v));
        }
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(a, "routes");
        for (int k = 0; k < m->n_note_outs && cJSON_IsArray(r); k++) {
            const cJSON *x = cJSON_GetArrayItem(r, k);
            const cJSON *to = cJSON_GetObjectItemCaseSensitive(x, "to");
            if (!cJSON_IsString(to)) continue;
            int d = ROUTE_NONE;
            if (!strcmp(to->valuestring, "midi")) {
                d = ROUTE_MIDI_OUT;
                studio_route_set_midi_ch(i, k, num(x, "ch", studio_route_midi_ch(i, k)));
            } else {
                for (int j = 0; j < g_nmods; j++)
                    if (!strcmp(to->valuestring, g_mods[j]->name) &&
                        (g_mods[j]->flags & TMOD_F_NOTES_IN))
                        d = j;
            }
            studio_route_set(i, k, d);
        }
    }
}

// --- files -------------------------------------------------------------------------------------

bool studio_patch_save(const char *name) {
    char path[128];
    if (!path_of(name, path, sizeof path)) return false;
    cJSON *o = studio_patch_to_json(name);
    char *txt = cJSON_Print(o);   // indented: it's meant to be read
    cJSON_Delete(o);
    if (!txt) return false;
    FILE *f = fopen(path, "w");
    bool ok = f && fputs(txt, f) >= 0;
    if (f) ok = (fclose(f) == 0) && ok;
    cJSON_free(txt);
    return ok;
}

bool studio_patch_load(const char *name, char *err, int errlen) {
    char path[128];
    if (!path_of(name, path, sizeof path)) {
        snprintf(err, errlen, "no storage");
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "can't open %s", name);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n > 0 && n < 512 * 1024 ? malloc(n + 1) : NULL;
    bool ok = buf && fread(buf, 1, n, f) == (size_t)n;
    fclose(f);
    cJSON *o = NULL;
    if (ok) {
        buf[n] = 0;
        o = cJSON_Parse(buf);
    }
    free(buf);
    if (!cJSON_IsObject(o) || !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(o, "apps"))) {
        cJSON_Delete(o);
        snprintf(err, errlen, "%s isn't a Studio patch", name);
        return false;
    }
    studio_patch_from_json(o);
    cJSON_Delete(o);
    return true;
}

bool studio_patch_delete(const char *name) {
    char path[128];
    return path_of(name, path, sizeof path) && remove(path) == 0;
}
