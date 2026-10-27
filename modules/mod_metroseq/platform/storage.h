// storage.h - PRESETS (10 numbered slots) and named PROJECTS.
//
// The Python kept these as JSON in /user; here they are versioned binary
// blobs in NVS (power-fail safe, wear levelled). The bank lives in RAM -
// saving updates RAM at once and a low-priority task on core 0 commits it
// to flash. With CONFIG_METRO_DEFER_FLASH_WHILE_PLAYING the commit waits
// for STOP, so a flash erase can never coincide with the running sequence.
#pragma once

#include <stdbool.h>

#include "engine/seq_model.h"

// Loads presets and projects. (The clock source is the Studio's now.)
void storage_init(void);

// Presets (UI task only)
bool storage_preset_used(int slot);
bool storage_preset_get(int slot, pattern_t *out);
void storage_preset_save(int slot, const pattern_t *p);
void storage_preset_clear(int slot);
// The slot being played and edited (-1 = none yet); kept across power-off.
int storage_cur_preset(void);
void storage_set_cur_preset(int slot);

// Projects (UI task only)
int storage_project_count(void);
const char *storage_project_name(int i);
bool storage_project_get(int i, pattern_t *out);
// Replaces a same-named project, else appends (oldest dropped when full).
// Returns the project's index.
int storage_project_save(const char *name, const pattern_t *p);

// true while a write is waiting for flash (e.g. deferred until STOP)
bool storage_write_pending(void);
