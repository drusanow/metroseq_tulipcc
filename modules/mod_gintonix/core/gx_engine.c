// gx_engine.c - see gx_engine.h.
#include "gx_engine.h"

#include <string.h>

void gx_engine_init(gx_engine_t *e) {
    memset(e, 0, sizeof *e);
    gx_patch_t p;
    gx_patch_default(&p);
    for (int c = 0; c < GX_NCH; c++) {
        gx_vp_from_patch(&e->vp[c], &p, 1.0f);
        e->v[c].seed = 0x9E3779B9u * (uint32_t)(c + 1);
    }
}

void gx_engine_silence(gx_engine_t *e) {
    for (int c = 0; c < GX_NCH; c++) gx_voice_choke(&e->v[c]);
}

static void hit(gx_engine_t *e, const gx_ev_t *ev) {
    int c = ev->ch;
    if (e->vp[c].choke)
        for (int j = 0; j < GX_NCH; j++)
            if (j != c && e->vp[j].choke) gx_voice_choke(&e->v[j]);
    gx_voice_trigger(&e->v[c], &e->vp[c], ev->vel);
}

void gx_engine_render(gx_engine_t *e, const gx_ev_t *evs, int nev, float *l, float *r, int n) {
    int pos = 0, k = 0;
    while (pos < n) {
        // start what's due at this frame, then render up to the next hit
        while (k < nev && evs[k].frame <= pos) hit(e, &evs[k++]);
        int end = k < nev ? evs[k].frame : n;
        if (end > n) end = n;
        if (end <= pos) end = pos + 1;
        for (int c = 0; c < GX_NCH; c++)
            if (e->v[c].active) gx_voice_render(&e->v[c], &e->vp[c], l + pos, r + pos, end - pos);
        pos = end;
    }
    while (k < nev) hit(e, &evs[k++]);   // (none: frames are < n)
}
