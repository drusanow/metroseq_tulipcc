// touch.h - touch types and calibration hooks shared with the UI.
// No ESP-IDF dependencies, so the UI code also builds on the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int16_t x, y;       // calibrated screen position
    int16_t rx, ry;     // raw controller coordinates (for calibration)
    bool pressed;
} touch_event_t;

// True once the touch controller has been found.
bool gt911_ready(void);

// The controller's configured output range (raw coordinate maxima).
void gt911_raw_max(int *x_max, int *y_max);

// Raw -> screen calibration (engine/touch_cal.h layout). Safe from any task.
void gt911_set_cal(const float m[6]);
void gt911_get_cal(float m[6]);

// While set, every touch event goes to `hook` first; if it returns true the
// event is consumed and LVGL sees no touch (used by touch calibration,
// which needs the raw coordinates whatever the current calibration is).
typedef bool (*touch_raw_hook_t)(const touch_event_t *ev);
void display_set_touch_hook(touch_raw_hook_t hook);
