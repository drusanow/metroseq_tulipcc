// wt_patch.h - Wavetron patches, in the sketch's own JSON format, so a
// wt_patches.json from the AMYboard loads as it is:
//
//   [ {"name": "PAD 1", "rec": {"p": {"a_pos": 0.35, ...},
//                               "mx": {"0:a_pos": 0.35, ...},
//                               "a_wt_path": null | "/sd/tables/x.wav",
//                               "b_wt_path": ...}}, ... ]
//
// A table is stored by PATH (null = a built-in, then p.a_wt / p.b_wt is its
// number), never by catalogue index. Keys a file doesn't have take their
// default - the sketch's _apply_patch rule, so an old patch never inherits
// whatever the previous one set.
//
// Pure C + cJSON: tested on the host.
#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "wt_params.h"

#define WT_PATCH_NAME_MAX 12
#define WT_MAX_PATCHES 24

typedef struct {
    wt_sound_t s;
    char a_path[160], b_path[160];   // "" = a built-in table ...
    int a_builtin, b_builtin;        // ... this one
} wt_patch_t;

cJSON *wt_patch_to_json(const wt_patch_t *p);
void wt_patch_from_json(const cJSON *rec, wt_patch_t *p);   // missing -> defaults

// the library: a cJSON array of {"name", "rec"}
cJSON *wt_lib_read(const char *path);          // never NULL (empty if absent)
bool wt_lib_write(const char *path, const cJSON *lib);
int wt_lib_count(const cJSON *lib);
const char *wt_lib_name(const cJSON *lib, int i);
int wt_lib_find(const cJSON *lib, const char *name);
const cJSON *wt_lib_rec(const cJSON *lib, int i);
// add or replace by name; false when full or the name is empty
bool wt_lib_put(cJSON *lib, const char *name, const wt_patch_t *p);
void wt_lib_delete(cJSON *lib, int i);
bool wt_lib_rename(cJSON *lib, int i, const char *name);   // false if taken / empty
