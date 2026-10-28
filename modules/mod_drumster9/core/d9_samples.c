// d9_samples.c - the sample image's index (see d9_samples.h).
#include "d9_samples.h"

#include <stdio.h>
#include <string.h>

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint32_t d9_index_bytes(const uint8_t *h) {
    if (memcmp(h, "D9SM", 4) || u16(h + 4) != 1) return 0;
    return 32 + 32 * (uint32_t)u16(h + 6) + 40 * (uint32_t)u16(h + 8);
}

bool d9_index_parse(d9_index_t *ix, const uint8_t *b, uint32_t len) {
    memset(ix, 0, sizeof *ix);
    if (len < 32) return false;
    uint32_t need = d9_index_bytes(b);
    int nb = u16(b + 6), ns = u16(b + 8);
    if (!need || need > len || nb > D9_MAX_BANKS || ns > D9_MAX_SAMPLES || u16(b + 10) != D9_SR_SAMPLES)
        return false;
    ix->total = u32(b + 20);
    const uint8_t *p = b + 32;
    for (int i = 0; i < nb; i++, p += 32) {
        d9_bank_t *k = &ix->banks[i];
        memcpy(k->key, p, 11);
        memcpy(k->label, p + 12, 15);
        k->first = u16(p + 28);
        k->count = u16(p + 30);
    }
    for (int i = 0; i < ns; i++, p += 40) {
        d9_sample_t *s = &ix->samples[i];
        memcpy(s->name, p, D9_NAME_LEN);
        s->preset = u16(p + 28);
        s->bank = p[30];
        s->root = p[31];
        s->offset = u32(p + 32);
        s->frames = u32(p + 36);
        // never point outside the image
        if (s->bank >= nb || s->offset > ix->total || s->frames > (ix->total - s->offset) / 2)
            return false;
    }
    ix->nbanks = nb;
    ix->nsamples = ns;
    return true;
}

const d9_sample_t *d9_sample(const d9_index_t *ix, int preset) {
    if (preset < 0) return NULL;
    for (int i = 0; i < ix->nsamples; i++)
        if (ix->samples[i].preset == preset) return &ix->samples[i];
    return NULL;
}

const char *d9_sample_name(const d9_index_t *ix, int preset, char *tmp, int len) {
    if (preset == D9_NO_SAMPLE) return "OFF";
    const d9_sample_t *s = d9_sample(ix, preset);
    if (s) return s->name;
    snprintf(tmp, len, "preset %d", preset);
    return tmp;
}

int d9_sample_ms(const d9_index_t *ix, int preset) {
    const d9_sample_t *s = d9_sample(ix, preset);
    return s ? (int)((uint64_t)s->frames * 1000 / D9_SR_SAMPLES) : 0;
}

int d9_bank_find(const d9_index_t *ix, const char *key) {
    for (int i = 0; i < ix->nbanks; i++)
        if (!strcmp(ix->banks[i].key, key)) return i;
    return -1;
}
