// touch_cal.h - touchscreen calibration maths (pure C, host-testable).
//
// Raw GT911 coordinates map to screen pixels through a 6-coefficient affine
// transform, which covers offset, scale, rotation and skew in both axes:
//
//     x = m[0]*rx + m[1]*ry + m[2]
//     y = m[3]*rx + m[4]*ry + m[5]
//
// It is solved by least squares from N >= 3 touched targets (the method in
// TI's "Calibration in touch-screen systems", SLYT277). The UI uses 9
// targets on a 3 x 3 grid near the edges, so one sloppy tap is averaged
// out, the leftover error tells us whether the taps were consistent, and
// what's left at each target becomes the edge correction (below).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOUCH_CAL_MAX_POINTS 9

// Tulip's own R11 constants (gt911_touchscreen.c): x-2, (y-12)*0.8.
#define TOUCH_CAL_DEFAULT {1.0f, 0.0f, -2.0f, 0.0f, 0.8f, -9.6f}

typedef struct {
    float rms_px;    // root-mean-square error at the targets
    float max_px;    // worst target
} touch_cal_fit_t;

// rx/ry: raw readings, sx/sy: where the targets were drawn. Returns false
// if the points are degenerate or the result is implausible (e.g. a tap
// landed nowhere near its target).
bool touch_cal_solve(const float *rx, const float *ry, const float *sx, const float *sy, int n,
                     float m[6], touch_cal_fit_t *fit);

static inline void touch_cal_apply(const float m[6], float rx, float ry, float *x, float *y) {
    *x = m[0] * rx + m[1] * ry + m[2];
    *y = m[3] * rx + m[4] * ry + m[5];
}

// Rejection thresholds for a fit.
#define TOUCH_CAL_MAX_RMS_PX 10.0f
#define TOUCH_CAL_MAX_ERR_PX 25.0f

// --- the edge correction -----------------------------------------------------------
//
// Touch panels bend near their edges, which a straight-line (affine) map
// can't follow: the bottom row of buttons ends up off even after an
// affine calibration. So the calibration screen also takes 9 targets on a
// 3 x 3 grid close to the edges, and keeps what the affine map still gets
// wrong at each of them. Between the targets that correction is blended
// bilinearly; past the outer targets it carries on along the same slope
// (to the edge, at most a quarter of a cell).
#define TOUCH_GRID_N 3
#define TOUCH_GRID_INSET 40
// the targets, row by row: x = 40, 512, 984; y = 40, 300, 560 on 1024 x 600
#define TOUCH_GRID_X(c, w) ((c) == 0 ? TOUCH_GRID_INSET : (c) == 1 ? (w) / 2 : (w) - TOUCH_GRID_INSET)
#define TOUCH_GRID_Y(r, h) ((r) == 0 ? TOUCH_GRID_INSET : (r) == 1 ? (h) / 2 : (h) - TOUCH_GRID_INSET)
// the largest correction kept at a target: more than this is a bad tap
#define TOUCH_GRID_MAX_PX 40.0f
// the affine fit under a grid calibration may be looser (the edges are
// what the grid is for)
#define TOUCH_GRID_MAX_RMS_PX 15.0f
#define TOUCH_GRID_MAX_ERR_PX 40.0f

typedef struct {
    float m[6];                                   // the affine map
    uint8_t grid;                                 // dx / dy below are in use
    float dx[TOUCH_GRID_N * TOUCH_GRID_N];        // correction at each target, row by row
    float dy[TOUCH_GRID_N * TOUCH_GRID_N];
} touch_map_t;

// From 9 taps (rx / ry, row by row on the grid above, screen w x h): the
// affine fit through all of them, then the correction at each. False (and
// fit filled in as far as it got) if the taps are inconsistent.
bool touch_cal_solve_grid(const float *rx, const float *ry, int w, int h, touch_map_t *map,
                          touch_cal_fit_t *fit);

// raw -> screen through the affine map, then the grid correction if any
void touch_map_apply(const touch_map_t *map, int w, int h, float rx, float ry, float *x, float *y);

#ifdef __cplusplus
}
#endif
