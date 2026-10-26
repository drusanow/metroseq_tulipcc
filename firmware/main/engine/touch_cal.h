// touch_cal.h - touchscreen calibration maths (pure C, host-testable).
//
// Raw GT911 coordinates map to screen pixels through a 6-coefficient affine
// transform, which covers offset, scale, rotation and skew in both axes:
//
//     x = m[0]*rx + m[1]*ry + m[2]
//     y = m[3]*rx + m[4]*ry + m[5]
//
// It is solved by least squares from N >= 3 touched targets (the method in
// TI's "Calibration in touch-screen systems", SLYT277). The UI uses 5
// targets - four corners and the centre - so one sloppy tap is averaged
// out, and the leftover error tells us whether the taps were consistent.
#pragma once

#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif
