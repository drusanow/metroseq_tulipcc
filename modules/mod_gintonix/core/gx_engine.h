// gx_engine.h - the 8 GinTonix voices: hits in, samples out.
//
// One audio block: the hits that fall inside it (from the pattern player,
// already converted to frame offsets) and any played live (pads, MIDI) are
// started at their exact frame; every voice renders into the mix. A hit
// on a CHOKE channel fades out the other choke channels (hi-hats).
// Pure C: the device module and the host tests run the same code.
#pragma once

#include <stdint.h>

#include "gx_dsp.h"

typedef struct {
    int frame;      // 0 .. n-1
    uint8_t ch;
    float vel;
} gx_ev_t;

typedef struct {
    gx_vp_t vp[GX_NCH];      // coefficients in use (copied from the UI's)
    gx_voice_t v[GX_NCH];
} gx_engine_t;

void gx_engine_init(gx_engine_t *e);
// evs must be in frame order.
void gx_engine_render(gx_engine_t *e, const gx_ev_t *evs, int nev, float *l, float *r, int n);
void gx_engine_silence(gx_engine_t *e);   // cut everything (kit change)
