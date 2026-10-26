// board.h - Tulip CC v4 (R11) hardware map.
//
// Taken from the Tulip firmware (tulip/esp32s3/pins.h, MAKERFABS +
// TULIP4_R11 sections, and shared/display.h) so this firmware drives the
// exact same panel, touch controller and I2C port.
#pragma once

// --- RGB LCD: 1024x600, 8-bit RGB332 bus ------------------------------------
#define LCD_H_RES            1024
#define LCD_V_RES            600
#define LCD_PCLK_HZ          (28 * 1000 * 1000)   // Tulip DEFAULT_PIXEL_CLOCK_MHZ
#define LCD_BOUNCE_ROWS      12                    // 600 / 12 = 50 bounces / frame

#define LCD_HSYNC_BACK_PORCH   16
#define LCD_HSYNC_FRONT_PORCH  210
#define LCD_HSYNC_PULSE_WIDTH  30
#define LCD_VSYNC_BACK_PORCH   10
#define LCD_VSYNC_FRONT_PORCH  22
#define LCD_VSYNC_PULSE_WIDTH  13

#define PIN_LCD_HSYNC   39
#define PIN_LCD_VSYNC   41
#define PIN_LCD_DE      40
#define PIN_LCD_PCLK    42
#define PIN_LCD_BK_PWM  47

// The 8 wired data lines, in RGB332 bit order (bit0 .. bit7):
// B6 B7 G5 G6 G7 R5 R6 R7
#define PIN_LCD_B6  9
#define PIN_LCD_B7  1
#define PIN_LCD_G5  15
#define PIN_LCD_G6  16
#define PIN_LCD_G7  4
#define PIN_LCD_R5  10
#define PIN_LCD_R6  21
#define PIN_LCD_R7  14
// Unused low colour bits on the panel connector; Tulip drives them low.
#define PIN_LCD_UNUSED_LOW {38 /*B4*/, 46 /*B5*/, 6 /*G3*/, 7 /*G4*/, 45 /*R3*/, 13 /*R4*/}

// --- I2C port: GT911 touch AND the Mabee DAC(s) share this bus -----------
#define PIN_I2C_SDA     17
#define PIN_I2C_SCL     18
#define PIN_TOUCH_RST   48     // active high reset on R11
#define TOUCH_ADDR_A    0x5D
#define TOUCH_ADDR_B    0x14
// Tulip's R11 touch calibration (gt911_touchscreen.c)
#define TOUCH_X_DELTA   (-2)
#define TOUCH_Y_DELTA   (-12)
#define TOUCH_Y_SCALE_NUM 4    // * 0.8
#define TOUCH_Y_SCALE_DEN 5

// --- I2S audio (preview synth) ----------------------------------------------
#define PIN_I2S_LRCLK   2
#define PIN_I2S_BCLK    8
#define PIN_I2S_DOUT    5

// --- MIDI (unused here; GPIO11 can double as a scope timing probe) --------
#define PIN_MIDI_OUT    11
#define PIN_MIDI_IN     12
