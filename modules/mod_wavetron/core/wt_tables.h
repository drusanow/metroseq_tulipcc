// wt_tables.h - Wavetron's wavetables: the six built-in tables, WAV files
// from /files, the catalogue the TABLE selectors scroll through, and the
// small preview grid the 3-D waterfall display draws.
//
// A wavetable is a run of 256-sample cycles (16-bit); POSITION crossfades
// through them, exactly as AMY's wave=WAVETABLE did for the sketch. The
// built-ins are the sketch's own generators (SAW, SQUARE, HARM, VOX, FOLD,
// BELL; 16 cycles each). Any WAV of at least two cycles loads - 8/16/24/32
// bit PCM, 32-bit float, WAVE_FORMAT_EXTENSIBLE, stereo downmixed - as the
// sketch's own decoder did.
//
// Pure C: tested on the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WT_CYCLE        256
#define WT_MIN_FRAMES   (2 * WT_CYCLE)
#define WT_MAX_FRAMES   (1 << 16)      // 256 cycles (128 KB): the sketch's cap
#define WT_GEN_CYCLES   16
#define WT_NBUILTIN     6
#define WT_USER_SLOTS   4              // tables from files held in RAM at once
#define WT_CAT_MAX      (WT_NBUILTIN + WT_USER_SLOTS)
#define WT_PREV_LINES   24             // the waterfall: frames drawn ...
#define WT_PREV_PTS     96             // ... and points per frame

typedef struct {
    int16_t *data;        // ncyc * WT_CYCLE samples
    int ncyc;
} wt_wave_t;

typedef struct {
    char name[24];
    char path[160];       // "" for a built-in
    wt_wave_t w;
    int8_t prev[WT_PREV_LINES][WT_PREV_PTS];   // each row normalised to +-127
    uint32_t used;        // LRU stamp (files only)
} wt_table_t;

// Memory for the tables (PSRAM on the Tulip), and for the small buffers
// read every sample (internal RAM). Set before wt_cat_init() / wt_init().
extern void *(*wt_alloc)(unsigned bytes);
extern void *(*wt_alloc_fast)(unsigned bytes);
extern void (*wt_free)(void *p);

extern const char *const WT_BUILTIN_NAMES[WT_NBUILTIN];

typedef struct {
    wt_table_t t[WT_CAT_MAX];
    int n;                // entries in use: the built-ins first, then files
    uint32_t clock;
    // A table leaving RAM may still be playing for one more audio block:
    // its memory is handed to retire(), which frees it once that's safe.
    void (*retire)(void *data);
} wt_catalog_t;

// Builds the built-in tables (into wt_alloc()'d memory).
bool wt_cat_init(wt_catalog_t *c);
// The entry for a file: already loaded, or loaded now (evicting the least
// recently used file that keep_a / keep_b don't hold). -1 + err on failure.
int wt_cat_load(wt_catalog_t *c, const char *path, int keep_a, int keep_b, char *err, int errlen);
int wt_cat_find(const wt_catalog_t *c, const char *path);
void wt_cat_free(wt_catalog_t *c);   // every table's memory
void wt_cat_touch(wt_catalog_t *c, int i);

// Decode a WAV to mono 16-bit, trimmed to whole cycles. *out is
// wt_alloc()'d. false + a short reason ("not a WAV", "too short") else.
bool wt_wav_decode(const char *path, int16_t **out, int *nframes, char *err, int errlen);

// Fill a table's preview grid from its samples.
void wt_make_preview(wt_table_t *t);

// One sample of a wavetable: phase 0..1, position 0..1 (crossfading
// cycles), linear interpolation in both directions.
static inline float wt_read(const wt_wave_t *w, float ph, float pos) {
    float fc = pos * (float)(w->ncyc - 1);
    int c0 = (int)fc;
    if (c0 < 0) c0 = 0;
    if (c0 > w->ncyc - 2) c0 = w->ncyc - 2;
    float cf = fc - (float)c0;
    if (cf < 0) cf = 0;
    if (cf > 1) cf = 1;
    float fi = ph * WT_CYCLE;
    int i0 = (int)fi;
    float f = fi - (float)i0;
    i0 &= WT_CYCLE - 1;
    int i1 = (i0 + 1) & (WT_CYCLE - 1);
    const int16_t *a = w->data + c0 * WT_CYCLE, *b = a + WT_CYCLE;
    float va = a[i0] + f * (float)(a[i1] - a[i0]);
    float vb = b[i0] + f * (float)(b[i1] - b[i0]);
    return (va + cf * (vb - va)) * (1.0f / 32768.0f);
}
