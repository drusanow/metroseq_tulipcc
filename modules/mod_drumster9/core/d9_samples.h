// d9_samples.h - the Gamma 9001 sample image (tools/make_d9_samples.py).
//
// The 155 samples live in their own flash partition ("samples", 4 MB), as
// 16-bit mono PCM at 22,050 Hz, with an index in front. A sample is found by
// its AMY preset number - TR-808 0-18, TR-909 256-272, ... Extras 378-391 -
// so the Python Drumster9's kits, patterns and projects name them the same
// way. Pure C: the index is parsed from bytes, tested on the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define D9_SR_SAMPLES   22050
#define D9_MAX_BANKS    12
#define D9_MAX_SAMPLES  200
#define D9_NAME_LEN     27
#define D9_NO_SAMPLE    (-1)

typedef struct {
    char key[12];       // "tr808"
    char label[16];     // "TR-808"
    uint16_t first;     // first preset number
    uint16_t count;
} d9_bank_t;

typedef struct {
    char name[D9_NAME_LEN + 1];
    uint16_t preset;
    uint8_t bank;
    uint8_t root;
    uint32_t offset;    // bytes from the start of the image
    uint32_t frames;
} d9_sample_t;

typedef struct {
    int nbanks, nsamples;
    uint32_t total;     // image size in bytes
    d9_bank_t banks[D9_MAX_BANKS];
    d9_sample_t samples[D9_MAX_SAMPLES];
} d9_index_t;

// Read the index from the image's first `len` bytes (it needs about 7 KB).
// False if it isn't a sample image.
bool d9_index_parse(d9_index_t *ix, const uint8_t *buf, uint32_t len);
// Bytes of the image the index takes (from its header), 0 if not one.
uint32_t d9_index_bytes(const uint8_t *hdr32);

// The sample with this preset number, or NULL.
const d9_sample_t *d9_sample(const d9_index_t *ix, int preset);
// Its name, "OFF" for D9_NO_SAMPLE, "preset N" if unknown.
const char *d9_sample_name(const d9_index_t *ix, int preset, char *tmp, int len);
// Length in ms (0 if unknown).
int d9_sample_ms(const d9_index_t *ix, int preset);
// The bank with this key, -1 if none.
int d9_bank_find(const d9_index_t *ix, const char *key);

#ifdef __cplusplus
}
#endif
