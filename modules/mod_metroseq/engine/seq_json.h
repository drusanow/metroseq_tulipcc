// seq_json.h - a metroseq pattern as JSON (Studio patches).
//
//   {"v": 2, "bpm": 120, "root": 0, "scale": 1, "base_oct": 0, "range": 3,
//    "merge_cv": false,
//    "stages": [{"pitch": 0, "pulses": 1, "gate": 0, "slide": false, ...}, x8],
//    "tracks": [{"order": 0, "length": 8, "div": 0, ...}, x2],
//    "mods":   [{"values": [..8..], "order": 0, ..., "cc": 0, "cc_ch": 1}, x8]}
//
// Reading starts from the default pattern, so a key that's missing keeps
// its default, and the result is sanitised. Pure C on cJSON: tested on the
// host.
#pragma once

#include "cJSON.h"
#include "seq_model.h"

cJSON *pattern_to_json(const pattern_t *p);
void pattern_from_json(const cJSON *o, pattern_t *p);
