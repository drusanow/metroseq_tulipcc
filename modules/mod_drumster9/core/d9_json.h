// d9_json.h - Drumster9 projects, in the Python sketch's own JSON format,
// so its library (/user/tulipdrums_projects.json on the old Tulip) loads as
// it is from /files/drumster9/tulipdrums_projects.json:
//
//   [ {"name": "...", "bpm": 120, "kit": 0,
//      "lanes": [{"name": "Kick", "steps": [0, 8], "vol": 0.9, "muted": false,
//                 "solo": false, "kit_override": null, "bus": 0}, ...],
//      "bus_fx": [{"rev_level": 0, "rev_liveness": 0.75, ...}, x4],
//      "bank": [null | {"steps": {"Kick": [...], ...}, "kit": 0, "bpm": 120}, x10],
//      "bank_active": 0, "master_vol": 15.0,
//      "kit_overrides": {"1": {"Snare": 269}}}, ... ]
//
// Reading is defensive field by field, as the Python's was: anything
// missing or malformed keeps its default. Pure C on cJSON: tested on the host.
#pragma once

#include "cJSON.h"
#include "d9_model.h"

#define D9_MAX_PROJECTS 16
#define D9_PROJECT_NAME 20

cJSON *d9_project_to_json(const d9_state_t *s, const char *name, float bpm);
// bpm: the project's tempo (0 if it has none)
void d9_project_from_json(const cJSON *o, d9_state_t *s, float *bpm);

// steps <-> [0, 8, 16, 24]
cJSON *d9_steps_json(uint32_t mask);
uint32_t d9_steps_from_json(const cJSON *a);
