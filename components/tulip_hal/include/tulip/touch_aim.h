// touch_aim.h - help a touch land on a button (pure C, host-tested).
//
// Sits between the touch controller and LVGL. A touch that lands on
// something you can press goes straight through, as before. One that hits
// nothing - a gap between buttons, or just beside one, which is where an
// off calibration or the bent edge of a panel puts the finger - gets help:
//
//   SNAP   a button within AIM_SNAP_PX of the touch: it's pressed at once.
//   AIM    nothing near: the touch is held back while the finger stays put.
//          After AIM_HOLD_MS the position is averaged over the samples so
//          far (a held finger's readings settle) and looked up again, with
//          a wider snap. If a button is there, it's pressed; the finger
//          then drives it from where it is (sliders follow it).
//
// A finger that moves more than AIM_MOVE_PX while aiming is dragging (a
// scroll), and goes through as it is from there. A quick tap on nothing
// still arrives, as a press and a release where it was.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AIM_SNAP_PX 24       // a button this close to a touch on nothing is meant
#define AIM_HOLD_SNAP_PX 32  // ... and this close to a held, averaged touch
#define AIM_HOLD_MS 150      // a touch on nothing, held this long: look again
#define AIM_MOVE_PX 30       // moved further than this while aiming: a drag
#define AIM_SAMPLES 16

typedef struct {
    int16_t x, y;
    bool pressed;
} aim_ev_t;

// Is there something to press at x, y?
typedef bool (*aim_hit_fn)(int x, int y, void *ctx);

typedef enum { AIM_IDLE = 0, AIM_PASS, AIM_WAIT } aim_state_t;

typedef struct {
    aim_hit_fn hit;
    void *ctx;
    int w, h;              // the screen
    aim_state_t st;
    int16_t dx, dy;        // PASS: added to the finger's position
    int16_t x0, y0;        // WAIT: where it touched
    int16_t lx, ly;        // the last position given
    uint32_t t0;
    int n;                 // WAIT: samples taken
    int32_t sx, sy;
    bool tried;            // WAIT: looked again after the hold, found nothing
} touch_aim_t;

void touch_aim_init(touch_aim_t *a, aim_hit_fn hit, void *ctx, int w, int h);

// One touch event in, at t_ms: up to 2 events out (returns how many).
int touch_aim_feed(touch_aim_t *a, const aim_ev_t *in, uint32_t t_ms, aim_ev_t out[2]);

// Call often while nothing new arrives (every UI pass): a held aim may now
// find its button. Up to 1 event out.
int touch_aim_tick(touch_aim_t *a, uint32_t t_ms, aim_ev_t out[1]);

// The nearest point within r of x, y where hit() is true, spiralling out
// (x, y itself first). False if none.
bool touch_aim_snap(const touch_aim_t *a, int x, int y, int r, int *ox, int *oy);
