// wt_tables.c - see wt_tables.h.
#include "wt_tables.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *def_alloc(unsigned n) { return malloc(n); }
void *(*wt_alloc)(unsigned bytes) = def_alloc;
void (*wt_free)(void *p) = free;

#define TAU 6.2831853f

// --- the sketch's built-in tables ---------------------------------------------------------

// sine at position 0, bright saw at 1 (the harmonics fill in)
static float gen_saw(float ph, float m) { return sinf(TAU * ph) * (1 - m) + (2 * ph - 1) * m; }

// pulse width sweeping from a thin spike to a full square
static float gen_square(float ph, float m) { return ph < (0.5f - 0.45f * (1 - m)) ? 1.0f : -1.0f; }

// additive: one more harmonic joins per cycle, an organ-like build
static float gen_harm(float ph, float m) {
    int n = 1 + (int)(m * 15);
    float v = 0;
    for (int h = 1; h <= n; h++) v += sinf(TAU * h * ph) / h;
    return v;
}

// a vocal-ish pair of formants sliding up the table
static float gen_vox(float ph, float m) {
    float f1 = 1 + m * 5, f2 = 4 + m * 14;
    return sinf(TAU * ph) * 0.6f + sinf(TAU * f1 * ph) * 0.3f + sinf(TAU * f2 * ph) * 0.25f * m;
}

// a sine driven progressively into a triangle wavefolder
static float gen_fold(float ph, float m) {
    float v = sinf(TAU * ph) * (1 + m * 5);
    for (int g = 0; g < 4; g++) {
        if (v > 1)
            v = 2 - v;
        else if (v < -1)
            v = -2 - v;
    }
    return v;
}

// inharmonic partials fading in: metallic, very responsive to FM
static float gen_bell(float ph, float m) {
    static const float r[5] = {1.0f, 2.41f, 3.83f, 5.17f, 7.61f};
    float v = 0;
    for (int i = 0; i < 5; i++) v += sinf(TAU * r[i] * ph) * (1.0f / (i + 1)) * (i ? m : 1.0f);
    return v;
}

const char *const WT_BUILTIN_NAMES[WT_NBUILTIN] = {"SAW", "SQUARE", "HARM", "VOX", "FOLD", "BELL"};
static float (*const GEN[WT_NBUILTIN])(float, float) = {gen_saw, gen_square, gen_harm,
                                                        gen_vox, gen_fold, gen_bell};

// Each cycle is scaled to just under full scale on its own, so POSITION
// reads as a change of timbre rather than of volume (the sketch's
// _wt_norm_cycle).
static bool build_gen(wt_table_t *t, int k) {
    t->w.ncyc = WT_GEN_CYCLES;
    t->w.data = wt_alloc(WT_GEN_CYCLES * WT_CYCLE * sizeof(int16_t));
    if (!t->w.data) return false;
    float cyc[WT_CYCLE];
    for (int c = 0; c < WT_GEN_CYCLES; c++) {
        float m = (float)c / (WT_GEN_CYCLES - 1), pk = 1e-9f;
        for (int i = 0; i < WT_CYCLE; i++) {
            cyc[i] = GEN[k]((float)i / WT_CYCLE, m);
            if (fabsf(cyc[i]) > pk) pk = fabsf(cyc[i]);
        }
        float g = 0.95f * 32767.0f / pk;
        for (int i = 0; i < WT_CYCLE; i++) t->w.data[c * WT_CYCLE + i] = (int16_t)lrintf(cyc[i] * g);
    }
    snprintf(t->name, sizeof t->name, "%s", WT_BUILTIN_NAMES[k]);
    t->path[0] = 0;
    wt_make_preview(t);
    return true;
}

void wt_make_preview(wt_table_t *t) {
    const wt_wave_t *w = &t->w;
    for (int li = 0; li < WT_PREV_LINES; li++) {
        int c = (int)((float)li * (w->ncyc - 1) / (WT_PREV_LINES - 1) + 0.5f);
        const int16_t *cy = w->data + c * WT_CYCLE;
        int pk = 1;
        for (int i = 0; i < WT_CYCLE; i++)
            if (abs(cy[i]) > pk) pk = abs(cy[i]);
        for (int i = 0; i < WT_PREV_PTS; i++) {
            int s = cy[i * (WT_CYCLE - 1) / (WT_PREV_PTS - 1)];
            t->prev[li][i] = (int8_t)(s * 127 / pk);
        }
    }
}

bool wt_cat_init(wt_catalog_t *c) {
    memset(c->t, 0, sizeof c->t);
    c->n = 0;
    for (int k = 0; k < WT_NBUILTIN; k++) {
        if (!build_gen(&c->t[k], k)) return false;
        c->n++;
    }
    return true;
}

// --- WAV files --------------------------------------------------------------------------------

#define FMT_PCM 0x0001
#define FMT_FLOAT 0x0003
#define FMT_EXT 0xFFFE

static uint32_t le32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static uint16_t le16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }

#define FAIL(msg)                                   \
    do {                                            \
        snprintf(err, errlen, "%s", msg);           \
        goto fail;                                  \
    } while (0)

bool wt_wav_decode(const char *path, int16_t **out, int *nframes, char *err, int errlen) {
    *out = NULL;
    uint8_t *raw = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "can't open");
        return false;
    }
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        FAIL("not a WAV");
    int fmt = -1, nch = 1, bits = 16;
    uint32_t data_size = 0;
    bool have_data = false;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) break;
        uint32_t sz = le32(ch + 4);
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fb[40] = {0};
            uint32_t n = sz < sizeof fb ? sz : sizeof fb;
            if (sz < 16 || fread(fb, 1, n, f) != n) FAIL("bad fmt chunk");
            fmt = le16(fb);
            nch = le16(fb + 2);
            bits = le16(fb + 14);
            // EXTENSIBLE: the real format is the SubFormat GUID's first word
            if (fmt == FMT_EXT && sz >= 26) fmt = le16(fb + 24);
            if (fseek(f, (long)(sz - n + (sz & 1)), SEEK_CUR)) FAIL("bad fmt chunk");
        } else if (!memcmp(ch, "data", 4)) {
            data_size = sz;
            have_data = true;
            break;
        } else if (fseek(f, (long)(sz + (sz & 1)), SEEK_CUR)) {
            break;   // LIST / cue / smpl ...: skipped
        }
    }
    if (fmt < 0) FAIL("no fmt chunk");
    if (!have_data) FAIL("no data chunk");
    int bytes = (bits + 7) / 8;
    if (fmt != FMT_PCM && fmt != FMT_FLOAT) FAIL("format unsupported");
    if (fmt == FMT_FLOAT && bytes != 4) FAIL("float64 unsupported");
    if (bytes < 1 || bytes > 4 || nch < 1 || nch > 8) FAIL("bad WAV header");
    int fb = nch * bytes;
    uint32_t frames = data_size / fb;
    if (frames > WT_MAX_FRAMES) frames = WT_MAX_FRAMES;
    frames = frames / WT_CYCLE * WT_CYCLE;   // whole cycles only
    if (frames < WT_MIN_FRAMES) FAIL("too short (<512 smp)");

    *out = wt_alloc(frames * sizeof(int16_t));
    if (!*out) FAIL("out of memory");
    enum { BLK = 512 };
    raw = malloc(BLK * fb);
    if (!raw) FAIL("out of memory");
    uint32_t done = 0;
    while (done < frames) {
        uint32_t want = frames - done < BLK ? frames - done : BLK;
        uint32_t got = (uint32_t)fread(raw, fb, want, f);
        for (uint32_t i = 0; i < got; i++) {
            int32_t acc = 0;
            for (int c = 0; c < nch; c++) {
                const uint8_t *s = raw + i * fb + c * bytes;
                int32_t v;
                if (fmt == FMT_FLOAT) {
                    float x;
                    uint32_t u = le32(s);
                    memcpy(&x, &u, 4);
                    x = x < -1 ? -1 : (x > 1 ? 1 : x);
                    v = (int32_t)(x * 32767.0f);
                } else if (bytes == 1) {
                    v = ((int32_t)s[0] - 128) * 256;   // 8-bit WAV is unsigned
                } else if (bytes == 2) {
                    v = (int16_t)le16(s);
                } else if (bytes == 3) {
                    int32_t x = s[0] | s[1] << 8 | s[2] << 16;
                    if (x & 0x800000) x -= 1 << 24;
                    v = x >> 8;
                } else {
                    v = (int32_t)le32(s) >> 16;
                }
                acc += v;
            }
            acc /= nch;
            (*out)[done + i] = (int16_t)(acc > 32767 ? 32767 : (acc < -32768 ? -32768 : acc));
        }
        done += got;
        if (got < want) break;
    }
    done = done / WT_CYCLE * WT_CYCLE;
    if (done < WT_MIN_FRAMES) FAIL("too short (<512 smp)");
    *nframes = (int)done;
    free(raw);
    fclose(f);
    return true;
fail:
    free(raw);
    if (*out) wt_free(*out);
    *out = NULL;
    fclose(f);
    return false;
}

// --- the catalogue ------------------------------------------------------------------------------

int wt_cat_find(const wt_catalog_t *c, const char *path) {
    for (int i = WT_NBUILTIN; i < c->n; i++)
        if (!strcmp(c->t[i].path, path)) return i;
    return -1;
}

void wt_cat_touch(wt_catalog_t *c, int i) {
    if (i >= WT_NBUILTIN && i < c->n) c->t[i].used = ++c->clock;
}

static void basename_of(const char *path, char *out, int len) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    snprintf(out, len, "%s", b);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

int wt_cat_load(wt_catalog_t *c, const char *path, int keep_a, int keep_b, char *err, int errlen) {
    int i = wt_cat_find(c, path);
    if (i >= 0) {
        wt_cat_touch(c, i);
        return i;
    }
    int16_t *data;
    int n;
    if (!wt_wav_decode(path, &data, &n, err, errlen)) return -1;
    if (c->n < WT_CAT_MAX) {
        i = c->n++;
    } else {
        // evict the least recently used file nobody is playing
        i = -1;
        for (int k = WT_NBUILTIN; k < c->n; k++) {
            if (k == keep_a || k == keep_b) continue;
            if (i < 0 || c->t[k].used < c->t[i].used) i = k;
        }
        if (i < 0) {
            wt_free(data);
            snprintf(err, errlen, "no free table slot");
            return -1;
        }
        if (c->retire)
            c->retire(c->t[i].w.data);
        else
            wt_free(c->t[i].w.data);
    }
    wt_table_t *t = &c->t[i];
    t->w.data = data;
    t->w.ncyc = n / WT_CYCLE;
    snprintf(t->path, sizeof t->path, "%s", path);
    basename_of(path, t->name, sizeof t->name);
    wt_make_preview(t);
    wt_cat_touch(c, i);
    return i;
}

void wt_cat_free(wt_catalog_t *c) {
    for (int i = 0; i < c->n; i++) wt_free(c->t[i].w.data);
    c->n = 0;
}
