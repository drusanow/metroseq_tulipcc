// wt_patch.c - see wt_patch.h.
#include "wt_patch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wt_tables.h"

cJSON *wt_patch_to_json(const wt_patch_t *p) {
    cJSON *rec = cJSON_CreateObject();
    cJSON *o = cJSON_AddObjectToObject(rec, "p");
    for (int i = 0; i < WP_COUNT; i++) cJSON_AddNumberToObject(o, WT_PARAMS[i].key, p->s.p[i]);
    cJSON_AddNumberToObject(o, "a_wt", p->a_path[0] ? 0 : p->a_builtin);
    cJSON_AddNumberToObject(o, "b_wt", p->b_path[0] ? 0 : p->b_builtin);
    cJSON *mx = cJSON_AddObjectToObject(rec, "mx");
    for (int m = 0; m < WT_NMOD; m++)
        for (int d = 0; d < WT_NDEST; d++) {
            if (m == 3 && d == D_CUT) continue;   // that one is p.fenv
            if (p->s.mx[m][d] == 0) continue;
            char k[16];
            snprintf(k, sizeof k, "%d:%s", m, WT_DEST_IDS[d]);
            cJSON_AddNumberToObject(mx, k, p->s.mx[m][d]);
        }
    if (p->a_path[0])
        cJSON_AddStringToObject(rec, "a_wt_path", p->a_path);
    else
        cJSON_AddNullToObject(rec, "a_wt_path");
    if (p->b_path[0])
        cJSON_AddStringToObject(rec, "b_wt_path", p->b_path);
    else
        cJSON_AddNullToObject(rec, "b_wt_path");
    return rec;
}

void wt_patch_from_json(const cJSON *rec, wt_patch_t *p) {
    memset(p, 0, sizeof *p);
    wt_sound_default(&p->s);
    memset(p->s.mx, 0, sizeof p->s.mx);   // a patch's matrix is all it has
    p->a_builtin = 0;
    p->b_builtin = 1;
    const cJSON *o = cJSON_GetObjectItem(rec, "p");
    if (cJSON_IsObject(o)) {
        for (int i = 0; i < WP_COUNT; i++) {
            const cJSON *v = cJSON_GetObjectItem(o, WT_PARAMS[i].key);
            if (cJSON_IsNumber(v)) p->s.p[i] = wt_param_fix(i, (float)v->valuedouble);
        }
        const cJSON *a = cJSON_GetObjectItem(o, "a_wt"), *b = cJSON_GetObjectItem(o, "b_wt");
        if (cJSON_IsNumber(a) && a->valueint >= 0 && a->valueint < WT_NBUILTIN) p->a_builtin = a->valueint;
        if (cJSON_IsNumber(b) && b->valueint >= 0 && b->valueint < WT_NBUILTIN) p->b_builtin = b->valueint;
    }
    const cJSON *mx = cJSON_GetObjectItem(rec, "mx"), *e;
    if (cJSON_IsObject(mx)) {
        cJSON_ArrayForEach(e, mx) {
            if (!cJSON_IsNumber(e) || !e->string) continue;
            int m = atoi(e->string);
            const char *colon = strchr(e->string, ':');
            if (!colon || m < 0 || m >= WT_NMOD) continue;
            for (int d = 0; d < WT_NDEST; d++)
                if (!strcmp(colon + 1, WT_DEST_IDS[d])) wt_mx_set(&p->s, m, d, (float)e->valuedouble);
        }
    }
    const cJSON *ap = cJSON_GetObjectItem(rec, "a_wt_path"), *bp = cJSON_GetObjectItem(rec, "b_wt_path");
    if (cJSON_IsString(ap)) snprintf(p->a_path, sizeof p->a_path, "%s", ap->valuestring);
    if (cJSON_IsString(bp)) snprintf(p->b_path, sizeof p->b_path, "%s", bp->valuestring);
}

// --- the library -------------------------------------------------------------------------------

cJSON *wt_lib_read(const char *path) {
    cJSON *lib = NULL;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = n > 0 && n < 512 * 1024 ? malloc(n + 1) : NULL;
        if (buf && fread(buf, 1, n, f) == (size_t)n) {
            buf[n] = 0;
            lib = cJSON_Parse(buf);
        }
        free(buf);
        fclose(f);
    }
    if (!cJSON_IsArray(lib)) {
        cJSON_Delete(lib);
        return cJSON_CreateArray();
    }
    // keep only well-formed entries (the sketch's _read_patches)
    for (int i = cJSON_GetArraySize(lib) - 1; i >= 0; i--) {
        const cJSON *q = cJSON_GetArrayItem(lib, i);
        if (!cJSON_IsString(cJSON_GetObjectItem(q, "name")) || !cJSON_IsObject(cJSON_GetObjectItem(q, "rec")))
            cJSON_DeleteItemFromArray(lib, i);
    }
    return lib;
}

bool wt_lib_write(const char *path, const cJSON *lib) {
    char *txt = cJSON_PrintUnformatted(lib);
    if (!txt) return false;
    char tmp[200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fputs(txt, f) >= 0;
    if (f && fclose(f)) ok = false;
    cJSON_free(txt);
    if (!ok) return false;
    remove(path);
    return rename(tmp, path) == 0;
}

int wt_lib_count(const cJSON *lib) { return cJSON_GetArraySize(lib); }

const char *wt_lib_name(const cJSON *lib, int i) {
    const cJSON *n = cJSON_GetObjectItem(cJSON_GetArrayItem(lib, i), "name");
    return cJSON_IsString(n) ? n->valuestring : "";
}

int wt_lib_find(const cJSON *lib, const char *name) {
    for (int i = 0; i < wt_lib_count(lib); i++)
        if (!strcmp(wt_lib_name(lib, i), name)) return i;
    return -1;
}

const cJSON *wt_lib_rec(const cJSON *lib, int i) { return cJSON_GetObjectItem(cJSON_GetArrayItem(lib, i), "rec"); }

static void clean_name(const char *in, char *out) {
    while (*in == ' ') in++;
    snprintf(out, WT_PATCH_NAME_MAX + 1, "%s", in);
    for (int k = (int)strlen(out) - 1; k >= 0 && out[k] == ' '; k--) out[k] = 0;
}

bool wt_lib_put(cJSON *lib, const char *name, const wt_patch_t *p) {
    char nm[WT_PATCH_NAME_MAX + 1];
    clean_name(name, nm);
    if (!nm[0]) return false;
    int i = wt_lib_find(lib, nm);
    if (i < 0 && wt_lib_count(lib) >= WT_MAX_PATCHES) return false;
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "name", nm);
    cJSON_AddItemToObject(e, "rec", wt_patch_to_json(p));
    if (i >= 0)
        cJSON_ReplaceItemInArray(lib, i, e);
    else
        cJSON_AddItemToArray(lib, e);
    return true;
}

void wt_lib_delete(cJSON *lib, int i) { cJSON_DeleteItemFromArray(lib, i); }

bool wt_lib_rename(cJSON *lib, int i, const char *name) {
    char nm[WT_PATCH_NAME_MAX + 1];
    clean_name(name, nm);
    if (!nm[0] || i < 0 || i >= wt_lib_count(lib)) return false;
    int j = wt_lib_find(lib, nm);
    if (j >= 0 && j != i) return false;
    cJSON *e = cJSON_GetArrayItem(lib, i);
    cJSON_ReplaceItemInObject(e, "name", cJSON_CreateString(nm));
    return true;
}
