// gx_json.h - GinTonix files, in the Python version's JSON formats, so a
// Tulip /user/gintonix folder can be copied to /files/gintonix as it is:
//
//   gintonic_bank/001.json .. 128.json   {"name": "...", "state": {...}}
//   kits/*.json                          converted Microtonic presets
//                                        ({"state": ...} or raw Microtonic)
//   gintonix_drums.json                  [{"name", "a", "b"?}]  saved sounds
//   gintonic_settings.json               MIDI / clock / CC map settings
//
// Pure C on cJSON: tested on the host.
#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "gx_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// patch <-> {"mix": 0.5, ..., "wave": 0, ...}  (missing keys: defaults)
cJSON *gx_json_patch(const gx_patch_t *p);
void gx_json_to_patch(const cJSON *o, gx_patch_t *p);

// whole state <-> the Python's get_state() / set_state()
cJSON *gx_json_state(const gx_state_t *s);
// keep_tempo: leave out->tempo as it is (on MIDI clock)
void gx_json_to_state(const cJSON *o, gx_state_t *out, bool keep_tempo);

// A converted Microtonic preset (kits/*.json without "state") -> state.
void gx_json_mt_kit(const cJSON *k, gx_state_t *out);
void gx_json_mt_patch(const cJSON *d, gx_patch_t *p);

// Read / write a JSON file (NULL / false on any error).
cJSON *gx_json_load(const char *path);
bool gx_json_save(const char *path, const cJSON *o);

// A kit file: "state" form or raw Microtonic.
bool gx_load_kit_file(const char *path, gx_state_t *out, bool keep_tempo);

// Bank slot files.
bool gx_bank_load(const char *path, gx_state_t *out, char *name, int name_len, bool keep_tempo);
bool gx_bank_read_name(const char *path, char *name, int name_len);
bool gx_bank_save(const char *path, const char *name, const gx_state_t *s);

// Saved drum sounds (the DRUM list's own entries).
typedef struct {
    char name[GX_NAME_MAX + 1];
    gx_patch_t a, b;     // b == a when the sound has no morph
} gx_user_drum_t;

int gx_drums_load(const char *path, gx_user_drum_t *out, int max);
bool gx_drums_save(const char *path, const gx_user_drum_t *d, int n);

#ifdef __cplusplus
}
#endif
