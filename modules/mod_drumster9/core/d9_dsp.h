// d9_dsp.h - Drumster9's sound: 7 sample lanes into 4 FX buses.
//
// Each lane plays its kit's one-shot sample (16-bit, 22,050 Hz, played at
// 44,100 with linear interpolation). A retrigger fades the old hit out over
// 1.5 ms while the new one starts, so there's no click and nothing to choke
// (the Python's pre-hit note-offs worked round an AMY bug that isn't here).
//
// A lane goes to one of 4 buses. Per bus, as AMY did it in the Python:
//   drive (gain) -> filter (OFF / LPF / HPF, 12 dB, resonance)
//   -> EQ (first-order low shelf at 800 Hz, a Q 1 peak at 2.5 kHz,
//          first-order high shelf at 7 kHz, +-15 dB: AMY's bands)
//   -> dry, plus an echo (time, feedback, tone) and a stereo reverb
//      (liveness, damping), each at its level
// A bus with nothing playing and its tails gone costs nothing.
//
// Pure C, no allocation while running: tested on the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "d9_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define D9_SR          44100
#define D9_AUDITION    D9_LANES          // the KITS page's preview voice
#define D9_NVOICE      (D9_LANES + 1)
#define D9_ECHO_MAX    (D9_SR / 2)       // 500 ms
#define D9_FADE_FRAMES 64                // a retrigger's fade-out

// one hit: at `frame` of the block, lane plays pcm (22,050 Hz frames)
typedef struct {
    int frame;
    uint8_t lane;
    float vel;
    const int16_t *pcm;
    uint32_t len;
} d9_ev_t;

// What the engine needs from the state, per block (the glue builds it).
typedef struct {
    float lane_gain[D9_NVOICE];   // vol x drive x master, 0 = gate closed
    uint8_t lane_bus[D9_NVOICE];
    d9_bus_t bus[D9_BUSES];
    bool stop;                    // the transport just stopped: the lanes fade out (1.5 ms)
} d9_mix_t;

typedef struct {
    const int16_t *pcm;
    uint32_t len;     // source frames
    uint32_t pos;     // output frames since the hit (source frame = pos / 2)
    float gain;       // velocity
    int fade;         // > 0: frames left of a fade-out
} d9_voice_t;

typedef struct {
    // filter (TPT state variable)
    float ic1, ic2;
    float f_g, f_k, f_a1, f_a2, f_a3;
    float key_freq, key_reso;
    // EQ: three biquads (direct form 1)
    float eq_b[3][3], eq_a[3][2], eq_x[3][2], eq_y[3][2];
    float key_eq[3];
    bool eq_on[3];
    // echo
    int16_t *echo;
    int ew;
    float e_lp, e_prev;
    // reverb: a 4-line feedback delay network
    int16_t *rv[4];
    int rlen[4], ri[4];
    float r_lp[4];
    // idle: frames of silence in and out
    uint32_t quiet;
    bool active;
} d9_bus_rt_t;

typedef struct {
    d9_voice_t v[D9_NVOICE][2];   // the playing hit and the one fading out
    float g[D9_NVOICE];           // lane gain now (ramped to the target)
    d9_bus_rt_t bus[D9_BUSES];
    float peak;                   // the block's output peak (the MIX meter)
    float bus_peak[D9_BUSES];
} d9_engine_t;

// alloc: where the delay lines go (PSRAM on the Tulip). False if out of
// memory (the engine still plays, without echo / reverb).
bool d9_engine_init(d9_engine_t *e, void *(*alloc)(unsigned bytes));
void d9_engine_render(d9_engine_t *e, const d9_mix_t *m, const d9_ev_t *ev, int nev, float *l,
                      float *r, int n);
void d9_engine_silence(d9_engine_t *e);   // everything off, tails cleared

#ifdef __cplusplus
}
#endif
