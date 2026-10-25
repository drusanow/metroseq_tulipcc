# =====================================================================
# metropolix_tulip.py - touchscreen CV pitch/gate sequencer for Tulip CC
# =====================================================================
# Install: copy this file to /user, then:  run('metropolix_tulip.py')
#          (or  import metropolix_tulip; metropolix_tulip.run(None))
#
# A Metropolix-inspired CV sequencer for the Tulip Creative Computer,
# built in the same spirit as drumster8 and using the same Makerfabs
# Mabee DAC (GP8413) for eurorack output. UI conventions - LVGL colour
# cache, Button/strut/Spacer helpers, GC-safe defer - are lifted
# straight from drumster8 so the two apps feel like siblings.
#
# THE IDEA (from Intellijel's Metropolix):
#   There is ONE main 8-stage sequence, programmed with the PITCH sliders,
#   PULSE COUNT and GATE TYPE controls on the SEQ page. TWO tracks (TRK 1
#   and TRK 2) each read that same main sequence but reinterpret it with
#   their own playback ORDER, LENgth, clock DIVision, SWING, GATE length,
#   transposition and octave - so the two tracks generate counterpoint,
#   polyrhythms and movement from a single set of stages.
#
# CV OUTPUT (Makerfabs Mabee DAC, exactly as in drumster8):
#   TRK 1 ->  VOUT0 = GATE ,  VOUT1 = PITCH (1V/oct)     on DAC 0x59
#   TRK 2 ->  VOUT0 = GATE ,  VOUT1 = PITCH (1V/oct)     on DAC 0x58
#             (a SECOND Mabee with its A0 link cut; optional - if it
#              isn't there, TRK 2 still plays the audio preview.)
#
#   With one DAC you get TRK 1 on the rack and can monitor TRK 2 through
#   the built-in audio preview. Add the second DAC and both tracks drive
#   the modular independently.
#
# PAGES (touch-first - see the [SEQ] [TRK] [SET] buttons in the header):
#   SEQ   - the 8 shared stages: pitch sliders, pulse count, gate type,
#           slide and skip. Tap a stage number for the full per-stage
#           detail (ratchet, probability, accumulate).
#   TRK   - per-track transforms for TRK 1 and TRK 2 side by side.
#   SET   - scale/root quantiser, CV output config, preset bank.
# =====================================================================
# created by David Rusanow, in the drumster8 house style.
# =====================================================================

from tulip import UIScreen, UIElement, pal_to_lv, lv_depad, lv
import tulip
import amy
import sequencer
import random

try:
    import ujson as json
except ImportError:
    import json

try:
    import os
except ImportError:
    os = None

# The Tulip WEB app (browser build) has no `machine` module, so there is
# no I2C and no way to reach the DAC. Probe once here: when it's absent we
# run in SIMULATOR MODE - the whole sequencer and the audio preview work
# exactly as on hardware, there just isn't any CV output. This keeps the
# app fully testable in the browser instead of erroring out on startup.
try:
    import machine
    HAS_MACHINE = True
except Exception:
    machine = None
    HAS_MACHINE = False


# --- configuration -----------------------------------------------------

NUM_STAGES = 8                 # the main sequence is 8 stages (Metropolix)
NUM_TRACKS = 2                 # TRK 1 and TRK 2
MASTER_DIV = 16                # master clock resolution: 16 pulses / bar
#                                (a 1/16-note grid). Track DIV multiplies
#                                this, so tracks can run at 1/16, 1/8, ...
# If the main thread stalls (a screen redraw, a GC pause, a SAVE file
# write) the AMY clock runs on underneath and we come back several pulses
# behind. Catching all of them up at once fires a burst of notes that
# sounds like a stutter. We instead catch up at most this many pulses per
# callback and RESYNC beyond that - so ordinary jitter drops no notes, but
# a real stall just resumes cleanly in time instead of flamming.
MAX_CATCHUP = 2

PRESETS_FILE = "/user/metropolix_presets.json"
NUM_PRESETS = 16               # one bank of 16 (Metropolix stores 8x8; a
#                                single 16-slot bank suits the Tulip's RAM)

# Named projects: a full snapshot saved to disk under a name you type. The
# 16-slot preset bank above is the quick, numbered store; a project is the
# "save my whole setup under a name" store (like drumster8's user projects).
PROJECTS_FILE = "/user/metropolix_projects.json"
MAX_PROJECTS = 16
MAX_PROJECT_NAME_LEN = 24

MIN_BPM = 30
MAX_BPM = 300
BPM_STEP = 1
BPM_STEP_FAST = 10

# Pitch. Sliders run 0..PITCH_RANGE semitones; the value is then snapped
# to the active scale (see quantise). CV_BASE_MIDI is the MUSICAL note a
# slider value of 0 maps to (used for the SEQ note labels) - default C2.
PITCH_RANGE = 36               # slider span in semitones (3 octaves)
CV_BASE_MIDI = 36              # note that slider 0 names (C2) - labels only
# The Mabee GP8413 DAC is UNIPOLAR: it can only put out 0..5V or 0..10V,
# never negative. 1V/oct pitch references 0V to CV_V0_MIDI, following the
# eurorack standard: 0V = C0, 1V = C1, 2V = C2, ... So C0 (MIDI 12) sits
# at 0V, giving a 0..10V span from C0 up to C10 - the whole musical range
# stays positive and nothing clamps. (This is separate from CV_BASE_MIDI,
# which is only the display reference for the SEQ note labels.)
CV_V0_MIDI = 12                # MIDI note at 0V on the pitch CV (C0 = 0V)

# Gate types for a stage (Metropolix GATE MODE, applied across the
# stage's pulses):
GATE_MULT = 0   # a gate on EVERY pulse (ratchet-friendly)
GATE_SING = 1   # one gate on the first pulse only
GATE_HOLD = 2   # one long gate held across all the stage's pulses
GATE_REST = 3   # silent - no gate at all
GATE_NAMES = ["MULT", "SING", "HOLD", "REST"]

# Playback ORDER per track:
ORD_FWD = 0
ORD_REV = 1
ORD_PEND = 2
ORD_RAND = 3
ORD_BROWN = 4
ORDER_NAMES = ["FWD", "REV", "PEND", "RAND", "BRWN"]

# Track clock DIVision, in master-16th units, with musical labels.
DIV_VALUES = [1, 2, 3, 4, 6, 8]
DIV_NAMES = ["1/16", "1/8", "1/8.", "1/4", "1/4.", "1/2"]

# --- modulation lanes --------------------------------------------------
#
# Eight MOD lanes, each with 8 stages of bipolar values (-1..+1), its own
# playback ORDER, LENgth and clock DIV, and a routable internal
# DESTINATION. Each lane advances on its own clock and applies its current
# stage value (scaled by DEPTH) as a live offset to whatever it targets -
# this is Metropolix's self-modulation, where the sequencer modulates its
# own playback parameters.
NUM_MOD_LANES = 8

# Routing destinations. Each is (label, track_index, param). track_index
# None = OFF. The engine reads these through eff_* helpers, adding every
# lane routed to a destination on top of that parameter's base value, so
# a lane NEVER permanently mutates a track - unroute it and the base
# setting returns untouched.
DESTS = [
    ("OFF",          None, None),
    ("T1 Transpose", 0, "transpose"),
    ("T1 Octave",    0, "octave"),
    ("T1 Pitch",     0, "pitch"),
    ("T1 Pulses",    0, "pulses"),
    ("T1 Gate len",  0, "gate"),
    ("T1 Swing",     0, "swing"),
    ("T2 Transpose", 1, "transpose"),
    ("T2 Octave",    1, "octave"),
    ("T2 Pitch",     1, "pitch"),
    ("T2 Pulses",    1, "pulses"),
    ("T2 Gate len",  1, "gate"),
    ("T2 Swing",     1, "swing"),
]
DEST_NAMES = [d[0] for d in DESTS]

# How far a full-scale (+1.0) lane value pushes each parameter:
PARAM_SCALE = {
    "transpose": 12.0,   # +/- 12 semitones (added after quantise)
    "octave": 2.0,       # +/- 2 octaves
    "pitch": 12.0,       # +/- 12 slider-semitones (added BEFORE quantise,
    #                      so pitch mod stays inside the active scale)
    "pulses": 4.0,       # +/- 4 pulses on the played stage
    "gate": 0.5,         # +/- 0.5 of the gate fraction
    "swing": 0.4,        # +/- 0.4 of the swing amount
}


# --- scales / quantiser ------------------------------------------------

SCALES = [
    ("Chromatic", [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]),
    ("Major",     [0, 2, 4, 5, 7, 9, 11]),
    ("Minor",     [0, 2, 3, 5, 7, 8, 10]),
    ("Dorian",    [0, 2, 3, 5, 7, 9, 10]),
    ("Phrygian",  [0, 1, 3, 5, 7, 8, 10]),
    ("Lydian",    [0, 2, 4, 6, 7, 9, 11]),
    ("Mixolyd.",  [0, 2, 4, 5, 7, 9, 10]),
    ("Maj Pent",  [0, 2, 4, 7, 9]),
    ("Min Pent",  [0, 3, 5, 7, 10]),
    ("Harm Min",  [0, 2, 3, 5, 7, 8, 11]),
    ("Blues",     [0, 3, 5, 6, 7, 10]),
]
ROOT_NAMES = ["C", "C#", "D", "D#", "E", "F",
              "F#", "G", "G#", "A", "A#", "B"]


def quantise(semi, scale_ivals, root):
    """Snap a raw semitone offset to the nearest note of the active scale
    (relative to `root`), preserving octave. Returns a semitone offset."""
    if semi < 0:
        semi = 0
    s = semi - root
    octv = s // 12
    within = s % 12
    best = scale_ivals[0]
    bestd = 99
    for iv in scale_ivals:
        d = iv - within
        if d < 0:
            d = -d
        if d < bestd:
            bestd = d
            best = iv
    return octv * 12 + best + root


def note_name(midi):
    try:
        return "%s%d" % (ROOT_NAMES[midi % 12], (midi // 12) - 1)
    except Exception:
        return "?"


def eff_base_midi():
    """The MIDI note that a stage's pitch of 0 maps to, including the
    global BASE OCTAVE offset. Default base octave 0 -> CV_BASE_MIDI (C2),
    so the sliders span C2..C5; base octave -1 shifts them to C1..C4, etc.
    Used for both the SEQ note labels and the pitch actually output."""
    return CV_BASE_MIDI + 12 * app.base_octave


# --- layout (1024 x 600) -----------------------------------------------

SCREEN_TOP = 8
HDR_H = 52
HDR_BTN_H = 42
PAGE_Y = SCREEN_TOP + HDR_H + 6         # top of the page area
PAGE_H = 600 - PAGE_Y - 6

# SEQ page: 8 stage columns
SEQ_X0 = 34
COL_W = 122
COL_INNER = 108
SLIDER_H = 250
SLIDER_W = 20          # thin track (mockup); shared by SEQ and MOD sliders


def rgb332(r, g, b):
    return ((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6)


C_BG = 0
C_PANEL = rgb332(38, 38, 50)
C_PANEL2 = rgb332(28, 28, 38)
C_BTN = rgb332(60, 110, 200)
C_BTN_DK = rgb332(50, 55, 75)
C_PLAY = rgb332(40, 170, 90)
C_STOP = rgb332(200, 60, 50)
C_ACCENT = rgb332(150, 90, 200)
C_FLASH = rgb332(255, 140, 0)
C_LED_OFF = rgb332(40, 45, 55)
C_ON = rgb332(0, 210, 120)
C_TRK = [rgb332(70, 170, 255), rgb332(255, 150, 40)]   # TRK1 / TRK2 colours
# gate-type colours (MULT, SING, HOLD, REST)
C_GATE = [rgb332(0, 200, 110), rgb332(70, 150, 255),
          rgb332(230, 200, 40), rgb332(90, 90, 100)]
C_SKIP = rgb332(120, 60, 60)
C_SLIDE = rgb332(120, 90, 200)
C_SEL = rgb332(240, 190, 40)
C_PRESET_CUR = rgb332(255, 150, 40)    # current preset slot (orange)

# Bar sliders: a light-grey track with a small black handle that carries a
# coloured centre square - red on the SEQ pitch sliders, light-blue on the
# MOD sliders. Plus a blue 0-point marker for the bipolar MOD sliders.
C_SLIDER_TRACK = rgb332(205, 205, 205)   # light grey track
C_KNOB_FRAME = 0                         # black knob body
C_KNOB_SEQ = rgb332(225, 40, 40)         # red centre (SEQ pitch sliders)
C_KNOB_MOD = rgb332(70, 150, 255)        # light-blue centre (MOD sliders)
C_MOD_ZERO = rgb332(60, 120, 240)        # 0-point marker on MOD sliders


app = None


# Colour cache - pal_to_lv() allocates every call, and the playhead
# recolours run on the sequencer callback, so memoise (same reasoning as
# drumster8's _LVCOLOR).
_LVCOLOR = {}


def lv_color(c):
    v = _LVCOLOR.get(c)
    if v is None:
        v = pal_to_lv(c)
        _LVCOLOR[c] = v
    return v


def clampf(v, lo, hi):
    if v < lo:
        return lo
    if v > hi:
        return hi
    return v


def clampi(v, lo, hi):
    v = int(v)
    if v < lo:
        return lo
    if v > hi:
        return hi
    return v


def style_bar_slider(sl, center):
    """Give a slider the mockup look: a thin light-grey track with a small
    black handle carrying a coloured centre square (`center` = red for SEQ,
    light-blue for MOD). The knob is shortened and framed in black so the
    coloured centre reads as a square band."""
    try:
        sl.set_style_bg_opa(lv.OPA.COVER, lv.PART.MAIN)
        sl.set_style_bg_color(lv_color(C_SLIDER_TRACK), lv.PART.MAIN)
        # uniform track: indicator (filled part) matches the track colour
        sl.set_style_bg_color(lv_color(C_SLIDER_TRACK), lv.PART.INDICATOR)
        sl.set_style_bg_color(lv_color(center), lv.PART.KNOB)
        sl.set_style_radius(0, lv.PART.KNOB)
        sl.set_style_border_color(lv_color(C_KNOB_FRAME), lv.PART.KNOB)
        sl.set_style_border_width(4, lv.PART.KNOB)
        sl.set_style_border_opa(lv.OPA.COVER, lv.PART.KNOB)
        sl.set_style_pad_ver(-4, lv.PART.KNOB)   # short bar, not a square
        sl.set_style_pad_hor(3, lv.PART.KNOB)    # a touch wider than track
    except Exception:
        pass


# --- small UI helpers (from drumster8) ---------------------------------

def strut(parent, height, width=1):
    o = lv.obj(parent)
    o.set_size(width, height)
    o.align_to(parent, lv.ALIGN.LEFT_MID, 0, 0)
    try:
        o.set_style_bg_opa(0, 0)
        o.set_style_border_width(0, 0)
    except Exception:
        pass
    lv_depad(o)
    try:
        o.remove_flag(lv.obj.FLAG.SCROLLABLE)
        o.remove_flag(lv.obj.FLAG.CLICKABLE)
    except Exception:
        pass
    return o


# --- GC-safe deferred callbacks (verbatim from drumster8) --------------
#
# tulip.defer() stores the callback in a C array that the ESP32 GC does
# not scan, so a Python object whose only reference is that array can be
# freed mid-flight -> C-level crash. Keep a real Python reference until
# the callback fires. Always use defer_safe, never tulip.defer directly.

_pending_defers = {}
_defer_seq = 0


def defer_try(fn, arg, ms):
    global _defer_seq
    _defer_seq += 1
    key = _defer_seq

    def _wrapper(a=None, _key=key, _fn=fn):
        _pending_defers.pop(_key, None)
        try:
            _fn(a)
        except Exception as ex:
            print("deferred callback error:", ex)

    _pending_defers[key] = _wrapper
    try:
        tulip.defer(_wrapper, arg, ms)
        return True
    except Exception as ex:
        _pending_defers.pop(key, None)
        print("defer failed (no slot?):", ex)
        return False


def defer_safe(fn, arg, ms):
    if defer_try(fn, arg, ms):
        return
    try:
        fn(arg)
    except Exception as ex:
        print("inline callback error:", ex)


class Button:
    """Same Button as drumster8: optional hold-to-repeat via repeat_cb."""
    def __init__(self, parent, text, x, w, h, cb, color=C_BTN, y=0,
                  repeat_cb=None):
        self.cb = cb
        self.repeat_cb = repeat_cb
        self.color = color
        self.obj = lv.obj(parent)
        self.obj.set_size(w, h)
        self.obj.set_style_radius(5, 0)
        self.obj.set_style_bg_color(lv_color(color), 0)
        self.obj.align_to(parent, lv.ALIGN.TOP_LEFT, x, y)
        wired = False
        if repeat_cb is not None:
            try:
                self.obj.add_event_cb(self._cb, lv.EVENT.SHORT_CLICKED, None)
                self.obj.add_event_cb(self._on_repeat,
                                       lv.EVENT.LONG_PRESSED_REPEAT, None)
                wired = True
            except Exception:
                wired = False
        if not wired:
            self.obj.add_event_cb(self._cb, lv.EVENT.CLICKED, None)
        self.obj.remove_flag(lv.obj.FLAG.SCROLLABLE)
        lv_depad(self.obj)
        self.label = lv.label(self.obj)
        self.label.set_text(text)
        self.label.align_to(self.obj, lv.ALIGN.CENTER, 0, 0)

    def _cb(self, e):
        try:
            self.cb(e)
        except Exception as ex:
            print("button error:", ex)

    def _on_repeat(self, e):
        try:
            self.repeat_cb(e)
        except Exception as ex:
            print("button repeat error:", ex)

    def set_color(self, c):
        self.color = c
        try:
            self.obj.set_style_bg_color(lv_color(c), 0)
        except Exception:
            pass

    def set_text(self, t):
        try:
            self.label.set_text(t)
        except Exception:
            pass

    def flash(self, color=None, ms=500):
        if color is None:
            color = C_FLASH
        try:
            self.obj.set_style_bg_color(lv_color(color), 0)
        except Exception:
            pass

        def _revert(arg=None):
            try:
                self.obj.set_style_bg_color(lv_color(self.color), 0)
            except Exception:
                pass
        defer_safe(_revert, None, ms)


# =====================================================================
# DATA MODEL - shared stages + per-track transforms
# =====================================================================

class Stage:
    """One of the 8 shared stages. Both tracks read this same data."""
    def __init__(self):
        self.pitch = 0          # semitone offset, 0..PITCH_RANGE (slider)
        self.pulses = 1         # PULSE COUNT: how many clock pulses long
        self.gate = GATE_MULT   # GATE MODE across those pulses
        self.slide = False      # glide the pitch into this stage
        self.skip = False       # skip this stage in playback
        self.ratchet = 1        # sub-divide each gated pulse into N hits
        self.prob = 1.0         # probability the stage plays (0..1)
        self.accum = 0          # ACCUM: semitones added to the running
        #                         track transpose each time this stage plays

    def to_dict(self):
        return {"p": self.pitch, "n": self.pulses, "g": self.gate,
                "sl": self.slide, "sk": self.skip, "r": self.ratchet,
                "pr": self.prob, "ac": self.accum}

    def from_dict(self, d):
        self.pitch = clampi(d.get("p", 0), 0, PITCH_RANGE)
        self.pulses = clampi(d.get("n", 1), 1, 8)
        self.gate = clampi(d.get("g", GATE_MULT), 0, 3)
        self.slide = bool(d.get("sl", False))
        self.skip = bool(d.get("sk", False))
        self.ratchet = clampi(d.get("r", 1), 1, 8)
        self.prob = clampf(d.get("pr", 1.0), 0.0, 1.0)
        self.accum = clampi(d.get("ac", 0), -24, 24)


class Track:
    """A playback interpretation of the shared sequence: its own ORDER,
    LENgth, clock DIV, SWING, GATE length, transpose, octave, invert."""
    def __init__(self, idx):
        self.idx = idx
        self.order = ORD_FWD
        self.length = NUM_STAGES
        self.div_i = 0             # index into DIV_VALUES
        self.swing = 0.0           # 0..0.6, delays off-beat pulses
        self.gate_frac = 0.5       # gate length as a fraction of a pulse
        self.transpose = 0         # semitones
        self.octave = 0            # octaves
        self.invert = False        # mirror pitch sliders within the range
        self.mute_audio = False    # audio-preview mute (CV still outputs)

        # runtime playback state (not saved)
        self.cur_stage = -1
        self.pulse_i = 0
        self.pulse_left = 0
        self.pdir = 1              # pendulum direction
        self.accum_run = 0        # running accumulate transpose
        self.play_this = True
        self.cur_midi = CV_BASE_MIDI
        self.next16 = 0           # next master-16th at which this track fires
        self.disp_stage = 0       # stage the playhead should show

    @property
    def div(self):
        return DIV_VALUES[self.div_i]

    def reset_play(self):
        self.cur_stage = -1
        self.pulse_i = 0
        self.pulse_left = 0
        self.pdir = 1
        self.accum_run = 0
        self.next16 = 0
        self.disp_stage = 0

    def to_dict(self):
        return {"o": self.order, "l": self.length, "d": self.div_i,
                "sw": self.swing, "gf": self.gate_frac, "tr": self.transpose,
                "oc": self.octave, "inv": self.invert, "ma": self.mute_audio}

    def from_dict(self, d):
        self.order = clampi(d.get("o", ORD_FWD), 0, len(ORDER_NAMES) - 1)
        self.length = clampi(d.get("l", NUM_STAGES), 1, NUM_STAGES)
        self.div_i = clampi(d.get("d", 0), 0, len(DIV_VALUES) - 1)
        self.swing = clampf(d.get("sw", 0.0), 0.0, 0.6)
        self.gate_frac = clampf(d.get("gf", 0.5), 0.05, 1.0)
        self.transpose = clampi(d.get("tr", 0), -24, 24)
        self.octave = clampi(d.get("oc", 0), -3, 3)
        self.invert = bool(d.get("inv", False))
        self.mute_audio = bool(d.get("ma", False))


class ModLane:
    """One modulation lane: 8 bipolar stage values, its own ORDER/LEN/DIV,
    a DEPTH and a routable DESTination. Advances on its own clock and
    outputs cur_value, which the mod bus sums into its destination."""
    def __init__(self, idx):
        self.idx = idx
        self.values = [0.0] * NUM_STAGES   # -1.0 .. +1.0 per stage
        self.order = ORD_FWD
        self.length = NUM_STAGES
        self.div_i = 2                     # default 1/8. - a slow-ish LFO feel
        self.depth = 0.0                   # 0 = lane does nothing until raised
        self.dest = 0                      # index into DESTS (0 = OFF)

        # runtime (not saved)
        self.cur_stage = 0
        self.pdir = 1
        self.next16 = 0
        self.cur_value = 0.0
        self.disp_stage = 0

    @property
    def div(self):
        return DIV_VALUES[self.div_i]

    def reset_play(self):
        self.cur_stage = 0
        self.pdir = 1
        self.next16 = 0
        self.cur_value = self.values[0]
        self.disp_stage = 0

    def to_dict(self):
        return {"v": [round(x, 3) for x in self.values], "o": self.order,
                "l": self.length, "d": self.div_i, "dp": self.depth,
                "ds": self.dest}

    def from_dict(self, d):
        vals = d.get("v", [])
        for i in range(NUM_STAGES):
            self.values[i] = clampf(vals[i], -1.0, 1.0) if i < len(vals) else 0.0
        self.order = clampi(d.get("o", ORD_FWD), 0, len(ORDER_NAMES) - 1)
        self.length = clampi(d.get("l", NUM_STAGES), 1, NUM_STAGES)
        self.div_i = clampi(d.get("d", 2), 0, len(DIV_VALUES) - 1)
        self.depth = clampf(d.get("dp", 0.0), 0.0, 1.0)
        self.dest = clampi(d.get("ds", 0), 0, len(DESTS) - 1)


# =====================================================================
# AUDIO PREVIEW - one simple AMY synth voice per track
#
# So you can hear the sequence without a modular plugged in. Each track
# owns one oscillator; gate-on plays a note, gate-off releases it. CV
# output is independent of this and works whether or not audio is on.
# =====================================================================

TRACK_OSC = [0, 1]             # AMY oscillator per track
PREVIEW_ENABLED = True         # master audio-preview switch


def audio_init():
    """Configure the two preview voices. Cheap subtractive saw with a
    short envelope - just enough to hear pitch and rhythm."""
    for t in range(NUM_TRACKS):
        try:
            amy.send(osc=TRACK_OSC[t], wave=amy.SAW_DOWN,
                     filter_type=amy.FILTER_LPF, filter_freq=2500,
                     resonance=1.2,
                     bp0="0,1,30,0.4,120,0",   # amp env: quick attack/decay
                     amp=0.4)
        except Exception as ex:
            print("[audio] voice init failed:", ex)


def audio_note_on(t, midi):
    if not PREVIEW_ENABLED or app.tracks[t].mute_audio:
        return
    try:
        amy.send(osc=TRACK_OSC[t], note=midi, vel=0.5)
    except Exception:
        pass


def audio_note_off(t):
    try:
        amy.send(osc=TRACK_OSC[t], vel=0)
    except Exception:
        pass


# =====================================================================
# CV OUTPUT  ->  EURORACK   (Makerfabs Mabee DAC, GP8413)
#
# Adapted from drumster8's proven CV layer. Same GP8413 protocol (16-bit
# value, low byte first, reg 0x02 = VOUT0, reg 0x04 = VOUT1), same 100kHz
# bus so we never reconfigure the peripheral out from under Tulip's
# touchscreen driver, same fail-soft behaviour if a board is unplugged.
#
# The difference here: TWO independent DAC handles, one per track, each
# with its own presence flag and failure budget - so a missing second
# DAC (TRK 2) can never take TRK 1's output down with it.
#
#   Track 0 (TRK 1) -> DAC at 0x59 : VOUT0 gate, VOUT1 pitch
#   Track 1 (TRK 2) -> DAC at 0x58 : VOUT0 gate, VOUT1 pitch   (optional)
#
# POWER NOTE (same as drumster8): the Mabee's boost converter draws
# inrush at power-on. If Tulip browns out at boot with a DAC attached,
# power the DAC from its own 5V supply sharing ground, and leave the
# connector VCC disconnected.
# =====================================================================

CV_ENABLED = True
CV_AUTOSTART = True
CV_INIT_DELAY_MS = 2500        # keep DAC bringup off the critical boot path
CV_I2C_FREQ = 100000           # match the touchscreen bus; do not raise
CV_HIGH_V = 5.0                # gate high level (eurorack standard)
CV_LOW_V = 0.0
CV_MAX_V = 10.0                # DAC full scale
CV_MAX_FAILS = 5
CV_ADDR = [89, 88]             # 0x59 (TRK1), 0x58 (TRK2, A0 link cut)
GATE_REG = 0x02                # VOUT0
PITCH_REG = 0x04               # VOUT1

# Soft start: ease VOUT0 up and back down once at init instead of slamming
# the boost converter with a 0->5V step while the board is still settling.
CV_RAMP_STEPS = 16
CV_RAMP_MS = 40

# OUTPUT RANGE: the GP8413 range register decides whether our value is a
# fraction of 0-5V or 0-10V. Makerfabs ship the board in 0-10V, which is
# what drumster8 assumes too. MEASURE with cv_test() - if you read half
# what you asked for, set CV_SET_RANGE_10V = True.
CV_SET_RANGE_10V = False
CV_RANGE_REG = 0x01
CV_RANGE_10V = 0x11

_pitch_glide = {}              # track -> glide token (for slide ramps)
_gate_tokens = {}             # track -> gate pulse token


class _Mabee:
    """One GP8413, at the bus's existing clock."""
    def __init__(self, addr):
        self.addr = addr
        self.bus = machine.I2C(0, freq=CV_I2C_FREQ)
        if CV_SET_RANGE_10V:
            self.bus.writeto_mem(addr, CV_RANGE_REG, bytes([CV_RANGE_10V]))

    def write(self, volts, reg):
        v = int((volts / CV_MAX_V) * 65535.0)
        if v > 65535:
            v = 65535
        if v < 0:
            v = 0
        self.bus.writeto_mem(self.addr, reg, bytes([v & 0xff, (v >> 8) & 0xff]))


class CVOut:
    """Up to two DACs, one per track. Every method fails soft."""
    def __init__(self):
        self.dacs = [None] * NUM_TRACKS
        self.fails = [0] * NUM_TRACKS
        self.ready = False

    def init(self):
        self.dacs = [None] * NUM_TRACKS
        self.fails = [0] * NUM_TRACKS
        self.ready = False
        if not CV_ENABLED:
            return False
        if not HAS_MACHINE:
            print("[cv] SIMULATOR MODE - no 'machine' module (Tulip web "
                  "app). Sequencer + audio preview run normally; no CV out.")
            return False
        any_ok = False
        for t in range(NUM_TRACKS):
            try:
                dac = _Mabee(CV_ADDR[t])
                dac.write(CV_LOW_V, GATE_REG)      # a real write is the probe
                dac.write(CV_LOW_V, PITCH_REG)
                self.dacs[t] = dac
                any_ok = True
                print("[cv] DAC 0x%02x ready -> TRK %d (VOUT0 gate, "
                      "VOUT1 pitch)" % (CV_ADDR[t], t + 1))
            except Exception as ex:
                if t == 0:
                    print("[cv] no DAC at 0x%02x (TRK1):" % CV_ADDR[t],
                          repr(ex))
                # TRK2's DAC is optional - silence when absent
        if any_ok:
            self._ramp_start()
        return any_ok

    def present(self, t=None):
        if t is None:
            return any(d is not None for d in self.dacs)
        return self.dacs[t] is not None

    def _write(self, t, volts, reg):
        dac = self.dacs[t]
        if dac is None:
            return
        try:
            dac.write(volts, reg)
            self.fails[t] = 0
        except Exception as ex:
            self.fails[t] += 1
            if self.fails[t] >= CV_MAX_FAILS:
                self.dacs[t] = None
                print("[cv] TRK %d DAC lost - output disabled:" % (t + 1),
                      repr(ex))
                if app is not None and app.header is not None:
                    app.header.set_info("CV TRK%d lost" % (t + 1))

    def gate(self, t, high):
        self._write(t, CV_HIGH_V if high else CV_LOW_V, GATE_REG)

    def pitch(self, t, midi):
        # 1V/oct referenced to CV_V0_MIDI (0V), so the whole note range is
        # POSITIVE - the GP8413 is unipolar and cannot output below 0V.
        volts = (midi - CV_V0_MIDI) / 12.0
        self._write(t, clampf(volts, 0.0, CV_MAX_V), PITCH_REG)

    def all_low(self):
        for t in range(NUM_TRACKS):
            if self.dacs[t] is not None:
                try:
                    self._write(t, CV_LOW_V, GATE_REG)
                except Exception:
                    pass

    # -- soft start on VOUT0 of each present DAC --
    def _ramp_start(self):
        self.ready = False
        if CV_RAMP_STEPS <= 0:
            self.ready = True
            return
        self._ramp_step(0)

    def _ramp_step(self, i=0):
        if not isinstance(i, int):
            i = 0
        n = CV_RAMP_STEPS
        if i > 2 * n:
            for t in range(NUM_TRACKS):
                if self.dacs[t] is not None:
                    self._write(t, CV_LOW_V, GATE_REG)
            self.ready = True
            print("[cv] soft start done")
            return
        frac = (i if i <= n else (2 * n - i)) / float(n)
        for t in range(NUM_TRACKS):
            if self.dacs[t] is not None:
                self._write(t, CV_HIGH_V * frac, GATE_REG)
        defer_safe(self._ramp_step, i + 1, CV_RAMP_MS)


cv = CVOut()


# Merge/priority ("TRK2 -> TRK1 CV" mode). When app.merge_cv is on, both
# logical tracks drive the ONE physical output (DAC 0: VOUT0 gate, VOUT1
# pitch) so you can build dense, interleaved lines on a single voice. The
# eurorack CV is monophonic, so overlaps are resolved by PRIORITY: TRK 1
# always wins, and TRK 2 is suppressed for as long as TRK 1 holds the
# gate. Audio preview stays polyphonic (its own oscillator per track), so
# you still hear both while monitoring - only the CV is merged.
_audio_tokens = {}


def _t2_blocked(t):
    """True when this is TRK 2 and TRK 1 currently owns the shared out."""
    return (app.merge_cv and t == 1 and app._shared_high
            and app._shared_owner == 0)


def _audio_pulse(t, ms):
    tok = _audio_tokens.get(t, 0) + 1
    _audio_tokens[t] = tok
    audio_note_on(t, app.tracks[t].cur_midi)

    def _off(a=None, _t=t, _tok=tok):
        if _audio_tokens.get(_t) == _tok:
            audio_note_off(_t)
    defer_safe(_off, None, ms)


def _cv_raise(t, ms):
    """Raise the CV gate for logical track t for `ms`, honouring merge +
    TRK1 priority. A token drops a stale low so a fast retrigger isn't cut
    short (same guard as drumster8's _cv_pulse)."""
    if app.merge_cv:
        if _t2_blocked(t):
            return                            # TRK 1 has the shared out
        app._shared_owner = t
        app._shared_high = True
        cv.pitch(0, app.tracks[t].cur_midi)   # winner's pitch, applied now
        cv.gate(0, True)
        ph = 0
    else:
        cv.gate(t, True)
        ph = t
    tok = _gate_tokens.get(ph, 0) + 1
    _gate_tokens[ph] = tok

    def _low(a=None, _ph=ph, _tok=tok, _t=t):
        if _gate_tokens.get(_ph) != _tok:
            return
        if app.merge_cv and _ph == 0:
            if app._shared_owner == _t:
                cv.gate(0, False)
                app._shared_high = False
                app._shared_owner = None
        else:
            cv.gate(_ph, False)
    defer_safe(_low, None, ms)


def cv_gate_pulse(t, ms):
    """Gate high now, low `ms` later, plus the audio-preview note."""
    _audio_pulse(t, ms)
    _cv_raise(t, ms)


def cv_gate_hold_on(t):
    """Gate high and keep it high (HOLD mode) until the stage ends."""
    _audio_tokens[t] = _audio_tokens.get(t, 0) + 1
    audio_note_on(t, app.tracks[t].cur_midi)
    if app.merge_cv:
        if _t2_blocked(t):
            return
        app._shared_owner = t
        app._shared_high = True
        _gate_tokens[0] = _gate_tokens.get(0, 0) + 1
        cv.pitch(0, app.tracks[t].cur_midi)
        cv.gate(0, True)
    else:
        _gate_tokens[t] = _gate_tokens.get(t, 0) + 1
        cv.gate(t, True)


def cv_gate_off(t):
    _audio_tokens[t] = _audio_tokens.get(t, 0) + 1
    audio_note_off(t)
    if app.merge_cv:
        _gate_tokens[0] = _gate_tokens.get(0, 0) + 1
        if app._shared_owner == t or app._shared_owner is None:
            cv.gate(0, False)
            app._shared_high = False
            app._shared_owner = None
    else:
        _gate_tokens[t] = _gate_tokens.get(t, 0) + 1
        cv.gate(t, False)


def cv_set_pitch(t, midi, slide, ms):
    """Set the pitch CV. In merge mode the shared-out pitch is applied at
    gate time by whichever track wins, so here we only stash the note.
    Otherwise write this track's own pitch output now, ramping linearly
    for a SLIDE stage so the modular hears a glide."""
    tr = app.tracks[t]
    start = tr.cur_midi
    tr.cur_midi = midi
    if app.merge_cv:
        return
    if not slide or ms <= 0:
        cv.pitch(t, midi)
        return
    tok = _pitch_glide.get(t, 0) + 1
    _pitch_glide[t] = tok
    steps = 8

    def _ramp(i=0, _t=t, _tok=tok, _s=start, _e=midi, _n=steps):
        if _pitch_glide.get(_t) != _tok:
            return
        f = i / float(_n)
        cv.pitch(_t, _s + (_e - _s) * f)
        if i < _n:
            defer_safe(lambda a=None: _ramp(i + 1), None, max(1, int(ms / _n)))
    _ramp(0)


def cv_transport(playing):
    if not playing:
        cv.all_low()
        app._shared_high = False
        app._shared_owner = None
        for t in range(NUM_TRACKS):
            audio_note_off(t)


def cv_autostart(arg=None):
    if not CV_ENABLED:
        return
    if cv.init():
        if app.header is not None:
            app.header.set_info("CV ready")
    if app.header is not None:
        app.header.refresh_cv()


def toggle_cv(e=None):
    global CV_ENABLED
    CV_ENABLED = not CV_ENABLED
    if CV_ENABLED:
        if cv.init():
            app.header.set_info("CV on")
        else:
            CV_ENABLED = False
            app.header.set_info("sim - no CV" if not HAS_MACHINE
                                else "no DAC found")
    else:
        cv.all_low()
        cv.dacs = [None] * NUM_TRACKS
        app.header.set_info("CV off")
    app.header.refresh_cv()


def cv_scan():
    """REPL helper: import metropolix_tulip as m; m.cv_scan()"""
    if not HAS_MACHINE:
        print("[cv] no 'machine' module - simulator/web build, no I2C bus")
        return []
    try:
        found = machine.I2C(0, freq=400000).scan()
    except Exception as ex:
        print("[cv] scan failed:", repr(ex))
        return []
    if not found:
        print("[cv] nothing on I2C(0) - check VCC/GND and pull-ups")
        return found
    for a in found:
        who = ""
        if a == 89:
            who = "  <- Mabee (default) -> TRK 1"
        elif a == 88:
            who = "  <- Mabee (A0 cut) -> TRK 2"
        print("[cv] 0x%02x (%d)%s" % (a, a, who))
    return found


def cv_test(volts=5.0, track=0, gate=True):
    """Hold a steady voltage so you can meter it. Press STOP first.
        import metropolix_tulip as m; m.cv_test(5.0)   # VOUT0 TRK1
        m.cv_test(1.0, gate=False)                     # VOUT1 pitch out"""
    if not cv.present(track):
        print("[cv] no DAC for TRK %d - run cv_scan()" % (track + 1))
        return
    reg = GATE_REG if gate else PITCH_REG
    cv._write(track, volts, reg)
    print("[cv] TRK%d VOUT%d <- %.3fV (expect %.2fV on 0-10V range, "
          "%.2fV on 0-5V)" % (track + 1, 0 if gate else 1, volts,
                              volts, volts / 2.0))


def cv_note_test(midi=48, track=0):
    """Output a specific note on the pitch CV so you can meter the 1V/oct
    mapping. STOP first, or the sequencer overwrites it next step.
        import metropolix_tulip as m
        m.cv_note_test(48)   # C3 -> expect (48-CV_V0_MIDI)/12 volts
        m.cv_note_test(60)   # C4 -> one volt higher, etc."""
    if not cv.present(track):
        print("[cv] no DAC for TRK %d - run cv_scan()" % (track + 1))
        return
    v = (midi - CV_V0_MIDI) / 12.0
    cv.pitch(track, midi)
    print("[cv] TRK%d pitch <- %s (midi %d) = %.3fV (0V ref = midi %d). "
          "Adjacent octaves must read exactly 1.000V apart."
          % (track + 1, note_name(midi), midi, clampf(v, 0.0, CV_MAX_V),
             CV_V0_MIDI))


# =====================================================================
# SEQUENCER ENGINE
#
# One master TulipSequence clock ticks at MASTER_DIV pulses per bar (a
# 1/16 grid), driven by AMY's own sequencer clock - so it follows tempo
# exactly and stays sample-locked to the audio. Each track schedules its
# own pulses at multiples of that grid (its clock DIV), so the two tracks
# run at independent rates and phases: that is where the polyrhythm comes
# from. No audio work happens on this callback beyond firing gates.
# =====================================================================

def ticks_per_16th():
    try:
        n = int(amy.AMY_SEQUENCER_PPQ * 4 / MASTER_DIV)
        return n if n > 0 else 6
    except Exception:
        return 6


def sixteenth_ms():
    """Wall-clock length of one master pulse (a 1/16 note) at the current
    tempo. Used to time gate lengths, ratchets and slides."""
    return 60000.0 / app.bpm / (MASTER_DIV / 4.0)


def make_clock():
    drop_clock()
    app.clock = sequencer.TulipSequence(MASTER_DIV, _master_tick)


def drop_clock():
    old = getattr(app, "clock", None)
    app.clock = None
    if old is not None:
        try:
            old.clear()
        except Exception:
            pass


# --- modulation bus ----------------------------------------------------
#
# app.mod_bus maps (track_index, param) -> the summed live offset from
# every lane routed there. Recomputed whenever any lane advances. The
# eff_* helpers below add it on top of a track's base setting.

def recompute_mod_bus():
    bus = {}
    for lane in app.mod_lanes:
        d = DESTS[lane.dest]
        if d[1] is None:
            continue
        key = (d[1], d[2])
        scale = PARAM_SCALE.get(d[2], 1.0)
        bus[key] = bus.get(key, 0.0) + lane.cur_value * lane.depth * scale
    app.mod_bus = bus


def _mod_get(t, param):
    return app.mod_bus.get((t, param), 0.0)


def eff_transpose(t):
    return app.tracks[t].transpose + int(round(_mod_get(t, "transpose")))


def eff_octave(t):
    return app.tracks[t].octave + int(round(_mod_get(t, "octave")))


def eff_gate(t):
    return clampf(app.tracks[t].gate_frac + _mod_get(t, "gate"), 0.05, 1.0)


def eff_swing(t):
    return clampf(app.tracks[t].swing + _mod_get(t, "swing"), 0.0, 0.6)


def _next_mod_index(lane):
    """Next stage for a mod lane - same ORDER logic as tracks, but mod
    lanes have no SKIP so it always returns a valid stage."""
    L = lane.length
    cur = lane.cur_stage
    if lane.order == ORD_FWD:
        return (cur + 1) % L
    if lane.order == ORD_REV:
        return (cur - 1) % L
    if lane.order == ORD_PEND:
        nxt = cur + lane.pdir
        if nxt >= L:
            lane.pdir = -1
            return L - 2 if L > 1 else 0
        if nxt < 0:
            lane.pdir = 1
            return 1 if L > 1 else 0
        return nxt
    if lane.order == ORD_RAND:
        return random.randint(0, L - 1)
    return clampi(cur + random.choice((-1, 1)), 0, L - 1)   # ORD_BROWN


def _mod_tick(lane):
    lane.cur_stage = _next_mod_index(lane)
    lane.disp_stage = lane.cur_stage
    lane.cur_value = lane.values[lane.cur_stage]


def _next_stage_index(t, tr):
    """Pick the next playable stage for track `tr` honouring ORDER,
    LENgth and per-stage SKIP. Returns -1 if every stage in range is
    skipped (the track then rests for one pulse)."""
    L = tr.length
    cur = tr.cur_stage
    for _guard in range(L + 1):
        if cur < 0:
            nxt = (L - 1) if tr.order == ORD_REV else 0
        elif tr.order == ORD_FWD:
            nxt = (cur + 1) % L
        elif tr.order == ORD_REV:
            nxt = (cur - 1) % L
        elif tr.order == ORD_PEND:
            nxt = cur + tr.pdir
            if nxt >= L:
                nxt = L - 2 if L > 1 else 0
                tr.pdir = -1
            elif nxt < 0:
                nxt = 1 if L > 1 else 0
                tr.pdir = 1
        elif tr.order == ORD_RAND:
            nxt = random.randint(0, L - 1)
        else:   # ORD_BROWN
            nxt = clampi(cur + random.choice((-1, 1)), 0, L - 1)
        cur = nxt
        if not app.stages[nxt].skip:
            return nxt
    return -1


def _stage_enter(t, tr):
    """Begin a new stage on this track: choose it, roll probability,
    apply accumulate, compute and output the (quantised, transposed)
    pitch. Called when the previous stage's pulses are used up."""
    # end any held gate from the previous stage
    if tr.cur_stage >= 0 and app.stages[tr.cur_stage].gate == GATE_HOLD:
        cv_gate_off(t)

    idx = _next_stage_index(t, tr)
    if idx < 0:
        tr.cur_stage = -1
        tr.pulse_left = 1        # rest one pulse, then try again
        tr.pulse_i = 0
        tr.play_this = False
        return
    tr.cur_stage = idx
    tr.disp_stage = idx
    st = app.stages[idx]
    # PULSE COUNT, with any mod routed to this track's pulses
    tr.pulse_left = clampi(st.pulses + int(round(_mod_get(t, "pulses"))), 1, 8)
    tr.pulse_i = 0
    tr.play_this = (st.prob >= 1.0) or (random.random() < st.prob)

    # accumulate: add this stage's amount to the running transpose
    tr.accum_run += st.accum

    # pitch: slider -> optional invert -> pitch mod -> quantise ->
    # transpose/octave/accum. Pitch mod is added to the slider value
    # BEFORE quantising, so it moves within the active scale.
    semi = st.pitch
    if tr.invert:
        semi = PITCH_RANGE - semi
    semi += int(round(_mod_get(t, "pitch")))
    if semi < 0:
        semi = 0
    semi = quantise(semi, app.scale_ivals, app.root)
    midi = (eff_base_midi() + semi + eff_transpose(t)
            + 12 * eff_octave(t) + tr.accum_run)
    midi = clampi(midi, 0, 120)

    slide_ms = int(sixteenth_ms() * tr.div * 0.6)
    if tr.play_this:
        cv_set_pitch(t, midi, st.slide, slide_ms)
    else:
        tr.cur_midi = midi       # keep pitch in sync even on a skipped roll


def _fire_pulse(t, tr):
    """Fire (or not) the gate for the current pulse of the current stage,
    honouring GATE MODE, ratchet and swing."""
    if tr.cur_stage < 0 or not tr.play_this:
        return
    st = app.stages[tr.cur_stage]
    mode = st.gate
    if mode == GATE_REST:
        return

    gate_frac = eff_gate(t)
    swing = eff_swing(t)
    pulse_ms = sixteenth_ms() * tr.div
    gate_ms = max(4, int(pulse_ms * gate_frac))

    # swing: delay the gate on odd pulses of the master grid
    swing_ms = 0
    if swing > 0 and (app.clk // 1) % 2 == 1:
        swing_ms = int(pulse_ms * swing * 0.5)

    if mode == GATE_HOLD:
        # one long gate, opened on the first pulse, closed at stage end
        if tr.pulse_i == 0:
            if swing_ms > 0:
                defer_safe(lambda a=None: cv_gate_hold_on(t), None, swing_ms)
            else:
                cv_gate_hold_on(t)
        return

    if mode == GATE_SING and tr.pulse_i > 0:
        return

    # MULT (every pulse) or SING (first pulse): retrigger, with ratchet
    r = st.ratchet
    if r <= 1:
        if swing_ms > 0:
            defer_safe(lambda a=None: cv_gate_pulse(t, gate_ms), None,
                       swing_ms)
        else:
            cv_gate_pulse(t, gate_ms)
    else:
        sub = pulse_ms / r
        gsub = max(3, int(sub * gate_frac))
        for k in range(r):
            delay = swing_ms + int(k * sub)
            defer_safe(lambda a=None, _g=gsub: cv_gate_pulse(t, _g),
                       None, delay)


def _track_tick(t, tr):
    """One pulse event for track `tr`."""
    if tr.pulse_left <= 0:
        _stage_enter(t, tr)
    _fire_pulse(t, tr)
    tr.pulse_i += 1
    tr.pulse_left -= 1


def _master_tick(x):
    """AMY sequencer callback. x is a FREE-RUNNING raw tick count (it does
    not reset when we press play), so we anchor to its value at play-start
    and count 1/16 pulses from there, then drive each track's own pulse
    schedule off that monotonic counter."""
    try:
        if app.rebuilding or not app.playing:
            return
        tps = app.ticks_per_16th
        if app._base_tick is None:
            # first callback of this play run: anchor here, fire nothing
            # (start_playback already sounded the downbeat)
            app._base_tick = x
            app.clk = 0
            app._last16 = 0
            return
        g16 = int((x - app._base_tick) / tps)
        if g16 == app._last16:
            return
        # A stall left us more than MAX_CATCHUP pulses behind: resync the
        # clock to "now" and drop the backlog instead of firing it as a
        # burst. Re-anchor base_tick so g16 == clk again; each track's
        # next16 counter is relative to clk, so phase is preserved and
        # playback resumes on the next pulse. This is what keeps setting
        # changes from ever interrupting the groove.
        if g16 - app.clk > MAX_CATCHUP:
            app._base_tick = x - app.clk * tps
            app._last16 = app.clk
            return
        # normal path: advance one 16th at a time (bounded by MAX_CATCHUP)
        while app.clk < g16:
            app.clk += 1
            # mod lanes advance first, so a lane's new value is already on
            # the bus when the tracks it modulates fire this same pulse
            mod_changed = False
            for lane in app.mod_lanes:
                if app.clk >= lane.next16:
                    lane.next16 = app.clk + lane.div
                    _mod_tick(lane)
                    mod_changed = True
            if mod_changed:
                recompute_mod_bus()
            for t in range(NUM_TRACKS):
                tr = app.tracks[t]
                if app.clk >= tr.next16:
                    tr.next16 = app.clk + tr.div
                    _track_tick(t, tr)
        app._last16 = g16
        _update_playheads()
    except Exception as ex:
        print("master tick error (ignored):", ex)


def start_playback():
    app.clk = 0
    app._last16 = 0
    app._base_tick = None        # re-anchored on the next clock callback
    for lane in app.mod_lanes:
        lane.reset_play()
        lane.next16 = lane.div
    recompute_mod_bus()          # stage-0 mod values live before the downbeat
    for t in range(NUM_TRACKS):
        app.tracks[t].reset_play()
    # sound stage 0 on both tracks right now so the downbeat isn't late;
    # the next pulse for each track then lands at clk == its div
    for t in range(NUM_TRACKS):
        _track_tick(t, app.tracks[t])
        app.tracks[t].next16 = app.tracks[t].div
    _update_playheads()


def reset_sequence(e=None):
    """RESET: rewind both tracks and clear accumulate. Takes effect now."""
    for t in range(NUM_TRACKS):
        if app.tracks[t].cur_stage >= 0 and \
                app.stages[app.tracks[t].cur_stage].gate == GATE_HOLD:
            cv_gate_off(t)
    if app.playing:
        start_playback()
    else:
        for tr in app.tracks:
            tr.reset_play()
        _update_playheads()
    if app.header is not None:
        app.header.set_info("reset")


# =====================================================================
# TRANSPORT / TEMPO
# =====================================================================

def toggle_play(e=None):
    app.playing = not app.playing
    if app.playing:
        start_playback()
    else:
        cv_transport(False)
    _update_play_btn()


def _update_play_btn():
    if app.header is None:
        return
    app.header.play_btn.set_text("STOP" if app.playing else "PLAY")
    app.header.play_btn.set_color(C_STOP if app.playing else C_PLAY)


def set_bpm(v):
    app.bpm = clampi(v, MIN_BPM, MAX_BPM)
    try:
        sequencer.tempo(app.bpm)
    except Exception:
        pass
    if app.header is not None:
        app.header.set_bpm(app.bpm)


def bpm_up(e=None):
    set_bpm(app.bpm + BPM_STEP)


def bpm_down(e=None):
    set_bpm(app.bpm - BPM_STEP)


def bpm_up_fast(e=None):
    set_bpm(app.bpm + BPM_STEP_FAST)


def bpm_down_fast(e=None):
    set_bpm(app.bpm - BPM_STEP_FAST)


# =====================================================================
# UI  -  HEADER (always visible)
# =====================================================================

class Header:
    def __init__(self, parent):
        self.panel = lv.obj(parent)
        self.panel.set_size(1024, HDR_H)
        self.panel.set_style_bg_color(lv_color(C_PANEL2), 0)
        self.panel.set_style_radius(0, 0)
        self.panel.align_to(parent, lv.ALIGN.TOP_LEFT, 0, SCREEN_TOP)
        self.panel.remove_flag(lv.obj.FLAG.SCROLLABLE)
        lv_depad(self.panel)
        y = (HDR_H - HDR_BTN_H) // 2

        self.play_btn = Button(self.panel, "PLAY", 8, 92, HDR_BTN_H,
                                toggle_play, C_PLAY, y)
        Button(self.panel, "RST", 108, 56, HDR_BTN_H, reset_sequence,
               C_BTN_DK, y)

        Button(self.panel, "-", 180, 40, HDR_BTN_H, bpm_down, C_BTN, y,
               repeat_cb=bpm_down_fast)
        self.bpm_label = lv.label(self.panel)
        self.bpm_label.set_text("120 BPM")
        self.bpm_label.align_to(self.panel, lv.ALIGN.TOP_LEFT, 226, 18)
        Button(self.panel, "+", 300, 40, HDR_BTN_H, bpm_up, C_BTN, y,
               repeat_cb=bpm_up_fast)

        # page switcher
        self.page_btns = []
        names = ["SEQ", "TRK", "MOD", "SET"]
        for i, nm in enumerate(names):
            b = Button(self.panel, nm, 356 + i * 66, 60, HDR_BTN_H,
                       self._make_page(i), C_BTN_DK, y)
            self.page_btns.append(b)

        # CV indicator/toggle - green when a DAC is live (refresh_cv)
        self._cv_button = Button(self.panel, "CV", 624, 54, HDR_BTN_H,
                                 toggle_cv, C_BTN_DK, y)

        self.info = lv.label(self.panel)
        self.info.set_text("")
        self.info.align_to(self.panel, lv.ALIGN.TOP_LEFT, 688, 18)

    def _make_page(self, i):
        def _cb(e=None):
            show_page(i)
        return _cb

    def refresh_pages(self, active):
        for i, b in enumerate(self.page_btns):
            b.set_color(C_ACCENT if i == active else C_BTN_DK)

    def refresh_cv(self):
        try:
            on = CV_ENABLED and cv.present()
            self._cv_button.set_color(C_PLAY if on else C_BTN_DK)
        except Exception:
            pass

    def set_bpm(self, v):
        self.bpm_label.set_text("%d BPM" % int(v))

    def set_info(self, t):
        try:
            self.info.set_text(t)
        except Exception:
            pass


# =====================================================================
# UI  -  SEQ PAGE  (the 8 shared stages)
# =====================================================================

class StageColumn:
    """One stage's controls: number (opens detail), pitch slider, pulse
    count, gate type, slide + skip toggles, and two playhead LEDs."""
    def __init__(self, parent, i):
        self.i = i
        x = SEQ_X0 + i * COL_W
        yv = 4

        # playhead LEDs (one per track) at the very top of the column
        self.leds = []
        for t in range(NUM_TRACKS):
            o = lv.obj(parent)
            o.set_size(COL_INNER // 2 - 4, 8)
            o.set_style_radius(2, 0)
            o.align_to(parent, lv.ALIGN.TOP_LEFT,
                       x + t * (COL_INNER // 2 + 2), yv)
            o.remove_flag(lv.obj.FLAG.SCROLLABLE)
            o.remove_flag(lv.obj.FLAG.CLICKABLE)
            lv_depad(o)
            o.set_style_bg_color(lv_color(C_LED_OFF), 0)
            self.leds.append(o)

        # stage-number button -> detail popup
        self.num_btn = Button(parent, str(i + 1), x, COL_INNER, 30,
                              self._open_detail, C_BTN_DK, yv + 14)

        # pitch note readout
        self.note_lbl = lv.label(parent)
        self.note_lbl.set_text("--")
        self.note_lbl.align_to(parent, lv.ALIGN.TOP_LEFT, x + 4, yv + 50)

        # vertical pitch slider
        self.slider = lv.slider(parent)
        self.slider.set_size(SLIDER_W, SLIDER_H)
        self.slider.align_to(parent, lv.ALIGN.TOP_LEFT,
                             x + (COL_INNER - SLIDER_W) // 2, yv + 74)
        try:
            self.slider.set_range(0, PITCH_RANGE)
        except Exception:
            pass
        style_bar_slider(self.slider, C_KNOB_SEQ)
        self.slider.add_event_cb(self._slider_cb, lv.EVENT.VALUE_CHANGED,
                                 None)
        self.slider.remove_flag(lv.obj.FLAG.SCROLLABLE)

        # controls sit ~30px below the slider, giving the note sliders more
        # breathing room above the pulse/gate row
        by = yv + 74 + SLIDER_H + 38
        # pulse count
        self.pulse_btn = Button(parent, "x1", x, COL_INNER, 30,
                               self._cycle_pulse, C_BTN, by)
        # gate type
        self.gate_btn = Button(parent, "MULT", x, COL_INNER, 30,
                              self._cycle_gate, C_GATE[0], by + 34)
        # slide + skip
        half = COL_INNER // 2 - 3
        self.slide_btn = Button(parent, "SLD", x, half, 30,
                               self._toggle_slide, C_BTN_DK, by + 68)
        self.skip_btn = Button(parent, "SKP", x + half + 6, half, 30,
                              self._toggle_skip, C_BTN_DK, by + 68)

        self.refresh()

    def _stage(self):
        return app.stages[self.i]

    def _slider_cb(self, e=None):
        try:
            v = e.get_target_obj().get_value()
        except Exception:
            v = self.slider.get_value()
        self._stage().pitch = clampi(v, 0, PITCH_RANGE)
        self._refresh_note()

    def _refresh_note(self):
        st = self._stage()
        semi = quantise(st.pitch, app.scale_ivals, app.root)
        self.note_lbl.set_text(note_name(eff_base_midi() + semi))

    def _cycle_pulse(self, e=None):
        st = self._stage()
        st.pulses = st.pulses % 8 + 1
        self.pulse_btn.set_text("x%d" % st.pulses)

    def _cycle_gate(self, e=None):
        st = self._stage()
        st.gate = (st.gate + 1) % 4
        self.gate_btn.set_text(GATE_NAMES[st.gate])
        self.gate_btn.set_color(C_GATE[st.gate])

    def _toggle_slide(self, e=None):
        st = self._stage()
        st.slide = not st.slide
        self.slide_btn.set_color(C_SLIDE if st.slide else C_BTN_DK)

    def _toggle_skip(self, e=None):
        st = self._stage()
        st.skip = not st.skip
        self.skip_btn.set_color(C_SKIP if st.skip else C_BTN_DK)

    def _open_detail(self, e=None):
        app.detail_popup.show(self.i)

    def light(self, track, on):
        try:
            self.leds[track].set_style_bg_color(
                lv_color(C_TRK[track] if on else C_LED_OFF), 0)
        except Exception:
            pass

    def refresh(self):
        st = self._stage()
        try:
            self.slider.set_value(st.pitch, 0)
        except Exception:
            pass
        self._refresh_note()
        self.pulse_btn.set_text("x%d" % st.pulses)
        self.gate_btn.set_text(GATE_NAMES[st.gate])
        self.gate_btn.set_color(C_GATE[st.gate])
        self.slide_btn.set_color(C_SLIDE if st.slide else C_BTN_DK)
        self.skip_btn.set_color(C_SKIP if st.skip else C_BTN_DK)


class SeqPage:
    def __init__(self, parent):
        self.panel = lv.obj(parent)
        self.panel.set_size(1024, PAGE_H)
        self.panel.set_style_bg_color(lv_color(C_PANEL), 0)
        self.panel.set_style_radius(0, 0)
        self.panel.align_to(parent, lv.ALIGN.TOP_LEFT, 0, PAGE_Y)
        self.panel.remove_flag(lv.obj.FLAG.SCROLLABLE)
        lv_depad(self.panel)

        self.cols = [StageColumn(self.panel, i) for i in range(NUM_STAGES)]

    def refresh(self):
        for c in self.cols:
            c.refresh()

    def show(self):
        self.panel.remove_flag(lv.obj.FLAG.HIDDEN)

    def hide(self):
        self.panel.add_flag(lv.obj.FLAG.HIDDEN)


# =====================================================================
# UI  -  STAGE DETAIL POPUP  (ratchet / probability / accumulate)
# =====================================================================

class DetailPopup:
    def __init__(self, parent):
        self.stage_i = None
        self.panel = lv.obj(parent)
        self.panel.set_size(560, 320)
        self.panel.set_style_bg_color(lv_color(C_PANEL2), 0)
        self.panel.set_style_radius(10, 0)
        self.panel.align_to(parent, lv.ALIGN.CENTER, 0, 0)
        self.panel.remove_flag(lv.obj.FLAG.SCROLLABLE)

        self.title = lv.label(self.panel)
        self.title.set_text("Stage")
        self.title.align_to(self.panel, lv.ALIGN.TOP_LEFT, 20, 16)
        Button(self.panel, "Close", 440, 100, 40, self._close, C_BTN, 12)

        rows = [
            ("Pulse count", self._pulse_v, self._pulse_d, self._pulse_u),
            ("Gate type", self._gate_v, self._gate_d, self._gate_u),
            ("Ratchet", self._rat_v, self._rat_d, self._rat_u),
            ("Probability", self._prob_v, self._prob_d, self._prob_u),
            ("Accumulate", self._acc_v, self._acc_d, self._acc_u),
        ]
        self.value_lbls = []
        y0 = 64
        ys = 48
        for r, (name, _vf, df, uf) in enumerate(rows):
            y = y0 + r * ys
            lbl = lv.label(self.panel)
            lbl.set_text(name)
            lbl.align_to(self.panel, lv.ALIGN.TOP_LEFT, 24, y + 8)
            Button(self.panel, "-", 250, 50, 38, df, C_BTN, y)
            vlbl = lv.label(self.panel)
            vlbl.set_text("")
            vlbl.align_to(self.panel, lv.ALIGN.TOP_LEFT, 330, y + 8)
            self.value_lbls.append(vlbl)
            Button(self.panel, "+", 420, 50, 38, uf, C_BTN, y)
        self.hide()

    def _st(self):
        if self.stage_i is None:
            return None
        return app.stages[self.stage_i]

    # value getters (as strings)
    def _pulse_v(self):
        return "x%d" % self._st().pulses

    def _gate_v(self):
        return GATE_NAMES[self._st().gate]

    def _rat_v(self):
        return "x%d" % self._st().ratchet

    def _prob_v(self):
        return "%d%%" % int(self._st().prob * 100 + 0.5)

    def _acc_v(self):
        return "%+d st" % self._st().accum

    # +/- handlers
    def _pulse_d(self, e=None):
        st = self._st(); st.pulses = clampi(st.pulses - 1, 1, 8); self._refresh()

    def _pulse_u(self, e=None):
        st = self._st(); st.pulses = clampi(st.pulses + 1, 1, 8); self._refresh()

    def _gate_d(self, e=None):
        st = self._st(); st.gate = (st.gate - 1) % 4; self._refresh()

    def _gate_u(self, e=None):
        st = self._st(); st.gate = (st.gate + 1) % 4; self._refresh()

    def _rat_d(self, e=None):
        st = self._st(); st.ratchet = clampi(st.ratchet - 1, 1, 8); self._refresh()

    def _rat_u(self, e=None):
        st = self._st(); st.ratchet = clampi(st.ratchet + 1, 1, 8); self._refresh()

    def _prob_d(self, e=None):
        st = self._st(); st.prob = clampf(st.prob - 0.1, 0.0, 1.0); self._refresh()

    def _prob_u(self, e=None):
        st = self._st(); st.prob = clampf(st.prob + 0.1, 0.0, 1.0); self._refresh()

    def _acc_d(self, e=None):
        st = self._st(); st.accum = clampi(st.accum - 1, -24, 24); self._refresh()

    def _acc_u(self, e=None):
        st = self._st(); st.accum = clampi(st.accum + 1, -24, 24); self._refresh()

    def _refresh(self):
        st = self._st()
        if st is None:
            return
        getters = [self._pulse_v, self._gate_v, self._rat_v,
                   self._prob_v, self._acc_v]
        for i, g in enumerate(getters):
            self.value_lbls[i].set_text(g())
        # keep the SEQ column in sync
        if app.seq_page is not None:
            app.seq_page.cols[self.stage_i].refresh()

    def show(self, stage_i):
        self.stage_i = stage_i
        self.title.set_text("Stage %d detail" % (stage_i + 1))
        self._refresh()
        self.panel.remove_flag(lv.obj.FLAG.HIDDEN)
        self.panel.move_foreground()

    def _close(self, e=None):
        self.hide()

    def hide(self):
        self.stage_i = None
        self.panel.add_flag(lv.obj.FLAG.HIDDEN)


# =====================================================================
# UI  -  TRACK PAGE  (per-track transforms, TRK 1 & TRK 2 side by side)
# =====================================================================

class TrackColumn:
    """All of one track's transform controls in a vertical column."""
    def __init__(self, parent, t, x0):
        self.t = t
        w = 460
        head = lv.label(parent)
        head.set_text("TRACK %d" % (t + 1))
        head.align_to(parent, lv.ALIGN.TOP_LEFT, x0 + 8, 10)
        try:
            head.set_style_text_color(lv_color(C_TRK[t]), 0)
        except Exception:
            pass

        self.rows = []
        specs = [
            ("Order", self._ord_v, self._ord_d, self._ord_u),
            ("Length", self._len_v, self._len_d, self._len_u),
            ("Clock div", self._div_v, self._div_d, self._div_u),
            ("Swing", self._sw_v, self._sw_d, self._sw_u),
            ("Gate len", self._gl_v, self._gl_d, self._gl_u),
            ("Transpose", self._tr_v, self._tr_d, self._tr_u),
            ("Octave", self._oc_v, self._oc_d, self._oc_u),
            ("Invert", self._inv_v, self._inv_t, self._inv_t),
            ("Audio", self._aud_v, self._aud_t, self._aud_t),
        ]
        self.value_lbls = []
        self.getters = []
        y0 = 46
        ys = 44
        for r, (name, vf, df, uf) in enumerate(specs):
            y = y0 + r * ys
            lbl = lv.label(parent)
            lbl.set_text(name)
            lbl.align_to(parent, lv.ALIGN.TOP_LEFT, x0 + 12, y + 8)
            Button(parent, "-", x0 + 190, 46, 34, df, C_BTN, y)
            v = lv.label(parent)
            v.set_text("")
            v.align_to(parent, lv.ALIGN.TOP_LEFT, x0 + 250, y + 8)
            self.value_lbls.append(v)
            self.getters.append(vf)
            Button(parent, "+", x0 + 360, 46, 34, uf, C_BTN, y)
        self.refresh()

    def _tr(self):
        return app.tracks[self.t]

    # getters
    def _ord_v(self):
        return ORDER_NAMES[self._tr().order]

    def _len_v(self):
        return str(self._tr().length)

    def _div_v(self):
        return DIV_NAMES[self._tr().div_i]

    def _sw_v(self):
        return "%d%%" % int(self._tr().swing * 100 + 0.5)

    def _gl_v(self):
        return "%d%%" % int(self._tr().gate_frac * 100 + 0.5)

    def _tr_v(self):
        return "%+d" % self._tr().transpose

    def _oc_v(self):
        return "%+d" % self._tr().octave

    def _inv_v(self):
        return "ON" if self._tr().invert else "off"

    def _aud_v(self):
        return "off" if self._tr().mute_audio else "ON"

    # handlers
    def _ord_d(self, e=None):
        tr = self._tr(); tr.order = (tr.order - 1) % len(ORDER_NAMES); self.refresh()

    def _ord_u(self, e=None):
        tr = self._tr(); tr.order = (tr.order + 1) % len(ORDER_NAMES); self.refresh()

    def _len_d(self, e=None):
        tr = self._tr(); tr.length = clampi(tr.length - 1, 1, NUM_STAGES); self.refresh()

    def _len_u(self, e=None):
        tr = self._tr(); tr.length = clampi(tr.length + 1, 1, NUM_STAGES); self.refresh()

    def _div_d(self, e=None):
        tr = self._tr(); tr.div_i = clampi(tr.div_i - 1, 0, len(DIV_VALUES) - 1); self.refresh()

    def _div_u(self, e=None):
        tr = self._tr(); tr.div_i = clampi(tr.div_i + 1, 0, len(DIV_VALUES) - 1); self.refresh()

    def _sw_d(self, e=None):
        tr = self._tr(); tr.swing = clampf(tr.swing - 0.05, 0.0, 0.6); self.refresh()

    def _sw_u(self, e=None):
        tr = self._tr(); tr.swing = clampf(tr.swing + 0.05, 0.0, 0.6); self.refresh()

    def _gl_d(self, e=None):
        tr = self._tr(); tr.gate_frac = clampf(tr.gate_frac - 0.05, 0.05, 1.0); self.refresh()

    def _gl_u(self, e=None):
        tr = self._tr(); tr.gate_frac = clampf(tr.gate_frac + 0.05, 0.05, 1.0); self.refresh()

    def _tr_d(self, e=None):
        tr = self._tr(); tr.transpose = clampi(tr.transpose - 1, -24, 24); self.refresh()

    def _tr_u(self, e=None):
        tr = self._tr(); tr.transpose = clampi(tr.transpose + 1, -24, 24); self.refresh()

    def _oc_d(self, e=None):
        tr = self._tr(); tr.octave = clampi(tr.octave - 1, -3, 3); self.refresh()

    def _oc_u(self, e=None):
        tr = self._tr(); tr.octave = clampi(tr.octave + 1, -3, 3); self.refresh()

    def _inv_t(self, e=None):
        tr = self._tr(); tr.invert = not tr.invert; self.refresh()

    def _aud_t(self, e=None):
        tr = self._tr(); tr.mute_audio = not tr.mute_audio; self.refresh()

    def refresh(self):
        for i, g in enumerate(self.getters):
            try:
                self.value_lbls[i].set_text(g())
            except Exception:
                pass


class TrackPage:
    def __init__(self, parent):
        self.panel = lv.obj(parent)
        self.panel.set_size(1024, PAGE_H)
        self.panel.set_style_bg_color(lv_color(C_PANEL), 0)
        self.panel.set_style_radius(0, 0)
        self.panel.align_to(parent, lv.ALIGN.TOP_LEFT, 0, PAGE_Y)
        self.panel.remove_flag(lv.obj.FLAG.SCROLLABLE)
        lv_depad(self.panel)

        # a divider between the two track columns
        div = lv.obj(self.panel)
        div.set_size(2, PAGE_H - 20)
        div.align_to(self.panel, lv.ALIGN.TOP_MID, 0, 10)
        div.set_style_bg_color(lv_color(C_PANEL2), 0)
        div.remove_flag(lv.obj.FLAG.SCROLLABLE)
        lv_depad(div)

        self.cols = [TrackColumn(self.panel, 0, 30),
                     TrackColumn(self.panel, 1, 530)]

    def refresh(self):
        for c in self.cols:
            c.refresh()

    def show(self):
        self.panel.remove_flag(lv.obj.FLAG.HIDDEN)

    def hide(self):
        self.panel.add_flag(lv.obj.FLAG.HIDDEN)


# =====================================================================
# UI  -  MOD PAGE  (8 modulation lanes with internal routing)
#
# A lane selector across the top picks which of the 8 lanes is on screen.
# The eight bipolar sliders are that lane's per-stage values; the controls
# below set its ORDER, LENgth, clock DIV, DEPTH and routing DESTINATION.
# =====================================================================

MOD_SLIDER_H = SLIDER_H     # MOD sliders are the same size as the SEQ ones


class ModPage:
    def __init__(self, parent):
        self.panel = lv.obj(parent)
        self.panel.set_size(1024, PAGE_H)
        self.panel.set_style_bg_color(lv_color(C_PANEL), 0)
        self.panel.set_style_radius(0, 0)
        self.panel.align_to(parent, lv.ALIGN.TOP_LEFT, 0, PAGE_Y)
        self.panel.remove_flag(lv.obj.FLAG.SCROLLABLE)
        lv_depad(self.panel)

        # --- lane selector ---
        self.lane_btns = []
        for i in range(NUM_MOD_LANES):
            b = Button(self.panel, "M%d" % (i + 1), 24 + i * 122, 112, 34,
                       self._make_select(i), C_BTN_DK, 6)
            self.lane_btns.append(b)

        # --- 8 bipolar value sliders with a playhead LED above each ---
        self.leds = []
        self.sliders = []
        self.val_lbls = []
        sy = 50
        for i in range(NUM_STAGES):
            x = SEQ_X0 + i * COL_W
            led = lv.obj(self.panel)
            led.set_size(COL_INNER, 8)
            led.set_style_radius(2, 0)
            led.align_to(self.panel, lv.ALIGN.TOP_LEFT, x, sy)
            led.remove_flag(lv.obj.FLAG.SCROLLABLE)
            led.remove_flag(lv.obj.FLAG.CLICKABLE)
            lv_depad(led)
            led.set_style_bg_color(lv_color(C_LED_OFF), 0)
            self.leds.append(led)

            slx = x + (COL_INNER - SLIDER_W) // 2
            sl = lv.slider(self.panel)
            sl.set_size(SLIDER_W, MOD_SLIDER_H)
            sl.align_to(self.panel, lv.ALIGN.TOP_LEFT, slx, sy + 16)
            try:
                sl.set_range(-100, 100)      # bipolar; 0 = centre
            except Exception:
                pass
            style_bar_slider(sl, C_KNOB_MOD)
            sl.add_event_cb(self._make_slider_cb(i), lv.EVENT.VALUE_CHANGED,
                            None)
            sl.remove_flag(lv.obj.FLAG.SCROLLABLE)
            self.sliders.append(sl)

            # blue 0-point marker: a short filled dash at the slider's
            # vertical centre (the bipolar zero), just left of the track.
            # Border off + full opacity so the thin bar actually shows -
            # a bare lv.obj otherwise paints the theme's border over it.
            zero = lv.obj(self.panel)
            zero.set_size(18, 4)
            zero.set_style_radius(0, 0)
            zero.set_style_border_width(0, 0)
            zero.set_style_pad_all(0, 0)
            zero.set_style_bg_opa(lv.OPA.COVER, 0)
            zero.set_style_bg_color(lv_color(C_MOD_ZERO), 0)
            zero.align_to(self.panel, lv.ALIGN.TOP_LEFT, slx - 20,
                          sy + 16 + MOD_SLIDER_H // 2 - 2)
            zero.remove_flag(lv.obj.FLAG.SCROLLABLE)
            zero.remove_flag(lv.obj.FLAG.CLICKABLE)

            vl = lv.label(self.panel)
            vl.set_text("0")
            vl.align_to(self.panel, lv.ALIGN.TOP_LEFT, slx - 6,
                        sy + 16 + MOD_SLIDER_H + 4)
            self.val_lbls.append(vl)

        # --- lane controls (two rows) ---
        cy = sy + 16 + MOD_SLIDER_H + 40
        self._ctrl(20, cy, "Order", self._ord_v, self._ord_d, self._ord_u)
        self._ctrl(270, cy, "Length", self._len_v, self._len_d, self._len_u)
        self._ctrl(520, cy, "Clock div", self._div_v, self._div_d,
                   self._div_u)
        cy2 = cy + 54
        self._ctrl(20, cy2, "Depth", self._dep_v, self._dep_d, self._dep_u)
        # destination gets a wider value field
        self.dest_lbl = None
        d = lv.label(self.panel)
        d.set_text("Route to")
        d.align_to(self.panel, lv.ALIGN.TOP_LEFT, 270, cy2 + 8)
        Button(self.panel, "-", 400, 46, 40, self._dst_d, C_BTN, cy2)
        self.dest_lbl = lv.label(self.panel)
        self.dest_lbl.set_text("OFF")
        self.dest_lbl.align_to(self.panel, lv.ALIGN.TOP_LEFT, 460, cy2 + 8)
        Button(self.panel, "+", 640, 46, 40, self._dst_u, C_BTN, cy2)

        self._val_getters = {}
        self.refresh()

    def _ctrl(self, x, y, name, vf, df, uf):
        """A label / - / value / + group; stashes the value getter+label
        so refresh() can update it."""
        lbl = lv.label(self.panel)
        lbl.set_text(name)
        lbl.align_to(self.panel, lv.ALIGN.TOP_LEFT, x, y + 8)
        Button(self.panel, "-", x + 110, 40, 40, df, C_BTN, y)
        v = lv.label(self.panel)
        v.set_text("")
        v.align_to(self.panel, lv.ALIGN.TOP_LEFT, x + 162, y + 8)
        Button(self.panel, "+", x + 200, 40, 40, uf, C_BTN, y)
        if not hasattr(self, "_ctrl_pairs"):
            self._ctrl_pairs = []
        self._ctrl_pairs.append((v, vf))

    def _lane(self):
        return app.mod_lanes[app.mod_sel]

    def _make_select(self, i):
        def _cb(e=None):
            app.mod_sel = i
            app._last_mod_disp = None
            self.refresh()
            _update_playheads()
        return _cb

    def _make_slider_cb(self, i):
        def _cb(e=None):
            try:
                v = e.get_target_obj().get_value()
            except Exception:
                v = self.sliders[i].get_value()
            self._lane().values[i] = v / 100.0
            self.val_lbls[i].set_text("%+d" % v)
            # if this stage is the one currently playing, update the bus now
            if self._lane().cur_stage == i:
                self._lane().cur_value = self._lane().values[i]
                recompute_mod_bus()
        return _cb

    # control getters
    def _ord_v(self):
        return ORDER_NAMES[self._lane().order]

    def _len_v(self):
        return str(self._lane().length)

    def _div_v(self):
        return DIV_NAMES[self._lane().div_i]

    def _dep_v(self):
        return "%d%%" % int(self._lane().depth * 100 + 0.5)

    # control handlers
    def _ord_d(self, e=None):
        ln = self._lane(); ln.order = (ln.order - 1) % len(ORDER_NAMES); self.refresh()

    def _ord_u(self, e=None):
        ln = self._lane(); ln.order = (ln.order + 1) % len(ORDER_NAMES); self.refresh()

    def _len_d(self, e=None):
        ln = self._lane(); ln.length = clampi(ln.length - 1, 1, NUM_STAGES); self.refresh()

    def _len_u(self, e=None):
        ln = self._lane(); ln.length = clampi(ln.length + 1, 1, NUM_STAGES); self.refresh()

    def _div_d(self, e=None):
        ln = self._lane(); ln.div_i = clampi(ln.div_i - 1, 0, len(DIV_VALUES) - 1); self.refresh()

    def _div_u(self, e=None):
        ln = self._lane(); ln.div_i = clampi(ln.div_i + 1, 0, len(DIV_VALUES) - 1); self.refresh()

    def _dep_d(self, e=None):
        ln = self._lane(); ln.depth = clampf(ln.depth - 0.05, 0.0, 1.0); recompute_mod_bus(); self.refresh()

    def _dep_u(self, e=None):
        ln = self._lane(); ln.depth = clampf(ln.depth + 0.05, 0.0, 1.0); recompute_mod_bus(); self.refresh()

    def _dst_d(self, e=None):
        ln = self._lane(); ln.dest = (ln.dest - 1) % len(DESTS); recompute_mod_bus(); self.refresh()

    def _dst_u(self, e=None):
        ln = self._lane(); ln.dest = (ln.dest + 1) % len(DESTS); recompute_mod_bus(); self.refresh()

    def light(self, stage, on):
        try:
            self.leds[stage].set_style_bg_color(
                lv_color(C_ACCENT if on else C_LED_OFF), 0)
        except Exception:
            pass

    def refresh(self):
        ln = self._lane()
        # lane selector highlight - amber if the lane is actively routed
        for i, b in enumerate(self.lane_btns):
            la = app.mod_lanes[i]
            if i == app.mod_sel:
                b.set_color(C_SEL)
            elif la.dest != 0 and la.depth > 0:
                b.set_color(C_BTN)
            else:
                b.set_color(C_BTN_DK)
        # sliders + value labels
        for i in range(NUM_STAGES):
            v = int(round(ln.values[i] * 100))
            try:
                self.sliders[i].set_value(v, 0)
            except Exception:
                pass
            self.val_lbls[i].set_text("%+d" % v)
        # controls
        for v, getter in getattr(self, "_ctrl_pairs", []):
            try:
                v.set_text(getter())
            except Exception:
                pass
        if self.dest_lbl is not None:
            self.dest_lbl.set_text(DEST_NAMES[ln.dest])

    def show(self):
        self.refresh()
        self.panel.remove_flag(lv.obj.FLAG.HIDDEN)

    def hide(self):
        self.panel.add_flag(lv.obj.FLAG.HIDDEN)


# =====================================================================
# UI  -  SAVE-PROJECT POPUP  (name entry via Tulip's touch keyboard)
#
# Adapted from drumster8's SaveProjectPopup: a real lv.keyboard bound to a
# textarea via set_textarea (the keyboard must be told where to type).
# =====================================================================

class SaveProjectPopup:
    def __init__(self, parent):
        self.panel = lv.obj(parent)
        self.panel.set_size(560, 150)
        self.panel.set_style_bg_color(lv_color(C_PANEL2), 0)
        self.panel.set_style_radius(8, 0)
        self.panel.align_to(parent, lv.ALIGN.CENTER, 0, -180)
        self.panel.remove_flag(lv.obj.FLAG.SCROLLABLE)

        self.title = lv.label(self.panel)
        self.title.set_text("Save project as:")
        self.title.align_to(self.panel, lv.ALIGN.TOP_LEFT, 16, 16)

        self.ta = lv.textarea(self.panel)
        self.ta.set_size(528, 48)
        self.ta.align_to(self.panel, lv.ALIGN.TOP_LEFT, 16, 46)
        self.ta.set_one_line(True)
        try:
            self.ta.set_max_length(MAX_PROJECT_NAME_LEN)
        except Exception:
            pass
        self.ta.set_placeholder_text("Project name")
        self.ta.add_event_cb(self._on_ready, lv.EVENT.READY, None)

        Button(self.panel, "Save", 316, 110, 40, self._on_save, C_PLAY, 102)
        Button(self.panel, "Cancel", 434, 110, 40, self.hide, C_BTN_DK, 102)
        self.panel.add_flag(lv.obj.FLAG.HIDDEN)

        # on-screen keyboard, bound to our textarea, docked at the bottom
        self.kb = lv.keyboard(parent)
        self.kb.set_size(1024, 260)
        self.kb.align_to(parent, lv.ALIGN.BOTTOM_MID, 0, 0)
        try:
            self.kb.set_textarea(self.ta)
        except Exception:
            pass
        self.kb.add_flag(lv.obj.FLAG.HIDDEN)

    def show(self):
        default = "Project %d" % (len(app.projects) + 1)
        try:
            self.ta.set_text(default)
        except Exception:
            pass
        self.panel.remove_flag(lv.obj.FLAG.HIDDEN)
        self.panel.move_foreground()
        self.kb.remove_flag(lv.obj.FLAG.HIDDEN)
        self.kb.move_foreground()
        try:
            lv.group_focus_obj(self.ta)
        except Exception:
            pass

    def hide(self, e=None):
        self.panel.add_flag(lv.obj.FLAG.HIDDEN)
        self.kb.add_flag(lv.obj.FLAG.HIDDEN)

    def _on_ready(self, e=None):
        self._on_save()

    def _on_save(self, e=None):
        name = ""
        try:
            name = self.ta.get_text()
        except Exception:
            pass
        if do_save_project(name):
            self.hide()


# =====================================================================
# UI  -  SETUP PAGE  (scale / root / CV / presets)
# =====================================================================

class SetupPage:
    def __init__(self, parent):
        self.panel = lv.obj(parent)
        self.panel.set_size(1024, PAGE_H)
        self.panel.set_style_bg_color(lv_color(C_PANEL), 0)
        self.panel.set_style_radius(0, 0)
        self.panel.align_to(parent, lv.ALIGN.TOP_LEFT, 0, PAGE_Y)
        self.panel.remove_flag(lv.obj.FLAG.SCROLLABLE)
        lv_depad(self.panel)

        # --- quantiser ---
        t = lv.label(self.panel)
        t.set_text("QUANTISER")
        t.align_to(self.panel, lv.ALIGN.TOP_LEFT, 30, 14)

        r = lv.label(self.panel)
        r.set_text("Root")
        r.align_to(self.panel, lv.ALIGN.TOP_LEFT, 30, 56)
        Button(self.panel, "-", 120, 44, 38, self._root_d, C_BTN, 50)
        self.root_lbl = lv.label(self.panel)
        self.root_lbl.set_text("C")
        self.root_lbl.align_to(self.panel, lv.ALIGN.TOP_LEFT, 178, 58)
        Button(self.panel, "+", 230, 44, 38, self._root_u, C_BTN, 50)

        s = lv.label(self.panel)
        s.set_text("Scale")
        s.align_to(self.panel, lv.ALIGN.TOP_LEFT, 30, 106)
        Button(self.panel, "-", 120, 44, 38, self._scale_d, C_BTN, 100)
        self.scale_lbl = lv.label(self.panel)
        self.scale_lbl.set_text("Chromatic")
        self.scale_lbl.align_to(self.panel, lv.ALIGN.TOP_LEFT, 178, 108)
        Button(self.panel, "+", 320, 44, 38, self._scale_u, C_BTN, 100)

        # base octave (right column) - shifts the whole SEQ note range and
        # updates the note labels live
        bo = lv.label(self.panel)
        bo.set_text("Base oct")
        bo.align_to(self.panel, lv.ALIGN.TOP_LEFT, 430, 56)
        Button(self.panel, "-", 540, 44, 38, self._boct_d, C_BTN, 50)
        self.boct_lbl = lv.label(self.panel)
        self.boct_lbl.set_text("0")
        self.boct_lbl.align_to(self.panel, lv.ALIGN.TOP_LEFT, 600, 58)
        Button(self.panel, "+", 650, 44, 38, self._boct_u, C_BTN, 50)
        self.range_lbl = lv.label(self.panel)
        self.range_lbl.set_text("")
        self.range_lbl.align_to(self.panel, lv.ALIGN.TOP_LEFT, 430, 108)

        # --- CV output ---
        c = lv.label(self.panel)
        c.set_text("CV OUTPUT  (TRK1 -> 0x59, TRK2 -> 0x58; "
                   "VOUT0 gate, VOUT1 pitch, 1V/oct)")
        c.align_to(self.panel, lv.ALIGN.TOP_LEFT, 30, 166)
        Button(self.panel, "CV ON/OFF", 30, 130, 42, toggle_cv, C_BTN_DK, 196)
        Button(self.panel, "RESCAN DAC", 172, 150, 42, self._rescan,
               C_BTN_DK, 196)
        self.merge_btn = Button(self.panel, "MERGE T2>T1", 340, 180,
                                42, self._toggle_merge, C_BTN_DK, 196)
        self.cv_status = lv.label(self.panel)
        self.cv_status.set_text("")
        self.cv_status.align_to(self.panel, lv.ALIGN.TOP_LEFT, 30, 246)

        a = lv.label(self.panel)
        a.set_text("Audio preview")
        a.align_to(self.panel, lv.ALIGN.TOP_LEFT, 560, 246)
        Button(self.panel, "TOGGLE", 680, 110, 42, self._toggle_audio,
               C_BTN_DK, 240)
        self.audio_lbl = lv.label(self.panel)
        self.audio_lbl.set_text("")
        self.audio_lbl.align_to(self.panel, lv.ALIGN.TOP_LEFT, 800, 246)

        # --- presets (16 slots, one even row) ---
        p = lv.label(self.panel)
        p.set_text("PRESETS  (tap to load; current slot lit orange; "
                   "SAVE writes the chosen slot; COPY/PASTE the live seq)")
        p.align_to(self.panel, lv.ALIGN.TOP_LEFT, 30, 296)
        self.slot_btns = []
        ps_start = 24
        ps_pitch = 61
        for i in range(NUM_PRESETS):
            b = Button(self.panel, str(i + 1), ps_start + i * ps_pitch, 55,
                       40, self._make_load(i), C_BTN_DK, 324)
            self.slot_btns.append(b)
        Button(self.panel, "SAVE ->", 24, 104, 40, self._save, C_PLAY, 372)
        self.save_slot = 0
        Button(self.panel, "<", 138, 38, 40, self._slot_prev, C_BTN, 372)
        self.save_lbl = lv.label(self.panel)
        self.save_lbl.set_text("slot 1")
        self.save_lbl.align_to(self.panel, lv.ALIGN.TOP_LEFT, 186, 382)
        Button(self.panel, ">", 244, 38, 40, self._slot_next, C_BTN, 372)
        self.copy_btn = Button(self.panel, "COPY", 300, 84, 40, clip_copy,
                               C_BTN_DK, 372)
        self.paste_btn = Button(self.panel, "PASTE", 392, 84, 40, clip_paste,
                                C_BTN_DK, 372)
        self.info = lv.label(self.panel)
        self.info.set_text("")
        self.info.align_to(self.panel, lv.ALIGN.TOP_LEFT, 496, 382)

        # --- named projects (custom name, saved to disk) ---
        pj = lv.label(self.panel)
        pj.set_text("PROJECT")
        pj.align_to(self.panel, lv.ALIGN.TOP_LEFT, 24, 436)
        Button(self.panel, "SAVE AS...", 110, 120, 40, open_save_project,
               C_PLAY, 428)
        Button(self.panel, "<", 250, 38, 40, project_prev, C_BTN, 428)
        self.proj_lbl = lv.label(self.panel)
        self.proj_lbl.set_text("-- no project --")
        self.proj_lbl.align_to(self.panel, lv.ALIGN.TOP_LEFT, 300, 438)
        Button(self.panel, ">", 470, 38, 40, project_next, C_BTN, 428)
        Button(self.panel, "LOAD", 520, 100, 40, project_load, C_BTN, 428)

    def _refresh_quant(self):
        """Update only the quantiser labels - a light touch so root/scale/
        octave tweaks never redraw the whole page while the sequence runs."""
        self.root_lbl.set_text(ROOT_NAMES[app.root])
        self.scale_lbl.set_text(SCALES[app.scale_idx][0])
        self.boct_lbl.set_text("%+d" % app.base_octave)
        self.range_lbl.set_text("%s .. %s" %
                                (note_name(eff_base_midi()),
                                 note_name(eff_base_midi() + PITCH_RANGE)))

    def _root_d(self, e=None):
        app.root = (app.root - 1) % 12
        self._refresh_quant()
        _refresh_notes()

    def _root_u(self, e=None):
        app.root = (app.root + 1) % 12
        self._refresh_quant()
        _refresh_notes()

    def _scale_d(self, e=None):
        app.scale_idx = (app.scale_idx - 1) % len(SCALES)
        app.scale_ivals = SCALES[app.scale_idx][1]
        self._refresh_quant()
        _refresh_notes()

    def _scale_u(self, e=None):
        app.scale_idx = (app.scale_idx + 1) % len(SCALES)
        app.scale_ivals = SCALES[app.scale_idx][1]
        self._refresh_quant()
        _refresh_notes()

    def _boct_d(self, e=None):
        app.base_octave = clampi(app.base_octave - 1, -3, 5)
        self._refresh_quant()
        _refresh_notes()

    def _boct_u(self, e=None):
        app.base_octave = clampi(app.base_octave + 1, -3, 5)
        self._refresh_quant()
        _refresh_notes()

    def _toggle_merge(self, e=None):
        app.merge_cv = not app.merge_cv
        # drop everything low so no gate is left stuck when the routing
        # changes, and clear the shared-out ownership
        try:
            cv.all_low()
        except Exception:
            pass
        app._shared_high = False
        app._shared_owner = None
        self.refresh()
        app.header.set_info("TRK2->TRK1 CV" if app.merge_cv
                            else "TRK1/TRK2 split")

    def _rescan(self, e=None):
        cv.init()
        self.refresh()
        app.header.refresh_cv()

    def _toggle_audio(self, e=None):
        global PREVIEW_ENABLED
        PREVIEW_ENABLED = not PREVIEW_ENABLED
        if not PREVIEW_ENABLED:
            for t in range(NUM_TRACKS):
                audio_note_off(t)
        self.refresh()

    def _make_load(self, i):
        def _cb(e=None):
            load_preset(i)
        return _cb

    def _slot_prev(self, e=None):
        self.save_slot = (self.save_slot - 1) % NUM_PRESETS
        self.save_lbl.set_text("slot %d" % (self.save_slot + 1))

    def _slot_next(self, e=None):
        self.save_slot = (self.save_slot + 1) % NUM_PRESETS
        self.save_lbl.set_text("slot %d" % (self.save_slot + 1))

    def _save(self, e=None):
        save_preset(self.save_slot)
        self.refresh()

    def refresh(self):
        self.root_lbl.set_text(ROOT_NAMES[app.root])
        self.scale_lbl.set_text(SCALES[app.scale_idx][0])
        self.boct_lbl.set_text("%+d" % app.base_octave)
        lo = note_name(eff_base_midi())
        hi = note_name(eff_base_midi() + PITCH_RANGE)
        self.range_lbl.set_text("%s .. %s" % (lo, hi))
        self.merge_btn.set_color(C_PLAY if app.merge_cv else C_BTN_DK)
        if not HAS_MACHINE:
            self.cv_status.set_text("SIMULATOR (web app) - audio preview "
                                    "only, no CV out")
        else:
            on = CV_ENABLED and cv.present()
            n = sum(1 for d in cv.dacs if d is not None)
            self.cv_status.set_text("CV %s - %d DAC(s) found" %
                                    ("ON" if on else "off", n))
        self.audio_lbl.set_text("ON" if PREVIEW_ENABLED else "off")
        for i, b in enumerate(self.slot_btns):
            if i == app.cur_preset:
                b.set_color(C_PRESET_CUR)
            elif app.presets[i] is not None:
                b.set_color(C_BTN)
            else:
                b.set_color(C_BTN_DK)
        self.refresh_projects()

    def refresh_projects(self):
        if not app.projects or app.project_idx < 0:
            self.proj_lbl.set_text("-- no project --")
        else:
            n = len(app.projects)
            name = app.projects[app.project_idx].get("name", "?")
            self.proj_lbl.set_text("%d/%d  %s" %
                                   (app.project_idx + 1, n, name))

    def show(self):
        self.refresh()
        self.panel.remove_flag(lv.obj.FLAG.HIDDEN)

    def hide(self):
        self.panel.add_flag(lv.obj.FLAG.HIDDEN)


# =====================================================================
# PAGE SWITCHING & PLAYHEADS
# =====================================================================

def show_page(i):
    app.page = i
    pages = (app.seq_page, app.track_page, app.mod_page, app.setup_page)
    for p, page in enumerate(pages):
        if page is None:
            continue
        if p == i:
            page.show()
        else:
            page.hide()
    app.header.refresh_pages(i)
    # force a repaint of whichever playhead the newly shown page owns
    app._last_disp = [None] * NUM_TRACKS
    app._last_mod_disp = None
    _update_playheads()


def _update_playheads():
    """Move the playhead LEDs on whichever page is showing - the two
    per-track heads on SEQ, or the single lane head on MOD - touching only
    the columns that changed."""
    if app.page == 0 and app.seq_page is not None:
        for t in range(NUM_TRACKS):
            cur = app.tracks[t].disp_stage
            prev = app._last_disp[t]
            if cur == prev:
                continue
            if prev is not None and 0 <= prev < NUM_STAGES:
                app.seq_page.cols[prev].light(t, False)
            if app.playing and 0 <= cur < NUM_STAGES:
                app.seq_page.cols[cur].light(t, True)
            app._last_disp[t] = cur
    elif app.page == 2 and app.mod_page is not None:
        cur = app.mod_lanes[app.mod_sel].disp_stage
        prev = app._last_mod_disp
        if cur != prev:
            if prev is not None and 0 <= prev < NUM_STAGES:
                app.mod_page.light(prev, False)
            if app.playing and 0 <= cur < NUM_STAGES:
                app.mod_page.light(cur, True)
            app._last_mod_disp = cur


def _refresh_notes():
    if app.seq_page is not None:
        for c in app.seq_page.cols:
            c._refresh_note()


# =====================================================================
# PRESETS  (bank of 8, saved to /user as JSON)
# =====================================================================

def _snapshot():
    return {
        "bpm": app.bpm,
        "root": app.root,
        "scale": app.scale_idx,
        "boct": app.base_octave,
        "merge": app.merge_cv,
        "stages": [s.to_dict() for s in app.stages],
        "tracks": [t.to_dict() for t in app.tracks],
        "mods": [m.to_dict() for m in app.mod_lanes],
    }


def _apply_snapshot(d):
    set_bpm(d.get("bpm", 120))
    app.root = clampi(d.get("root", 0), 0, 11)
    app.scale_idx = clampi(d.get("scale", 0), 0, len(SCALES) - 1)
    app.scale_ivals = SCALES[app.scale_idx][1]
    app.base_octave = clampi(d.get("boct", 0), -3, 5)
    app.merge_cv = bool(d.get("merge", False))
    sd = d.get("stages", [])
    for i in range(NUM_STAGES):
        if i < len(sd):
            app.stages[i].from_dict(sd[i])
    td = d.get("tracks", [])
    for i in range(NUM_TRACKS):
        if i < len(td):
            app.tracks[i].from_dict(td[i])
    md = d.get("mods", [])
    for i in range(NUM_MOD_LANES):
        if i < len(md):
            app.mod_lanes[i].from_dict(md[i])
    recompute_mod_bus()
    if app.seq_page is not None:
        app.seq_page.refresh()
    if app.track_page is not None:
        app.track_page.refresh()
    if app.mod_page is not None:
        app.mod_page.refresh()
    if app.setup_page is not None:
        app.setup_page.refresh()


def save_preset(slot):
    app.presets[slot] = _snapshot()
    app.cur_preset = slot
    _write_presets()
    app.setup_page.refresh()
    app.setup_page.info.set_text("saved slot %d" % (slot + 1))


def load_preset(slot):
    d = app.presets[slot]
    if d is None:
        app.setup_page.info.set_text("slot %d empty" % (slot + 1))
        return
    _apply_snapshot(d)
    app.cur_preset = slot
    if app.playing:
        start_playback()
    app.setup_page.refresh()
    app.setup_page.info.set_text("loaded slot %d" % (slot + 1))


def _write_presets():
    if os is None:
        return
    data = {"v": 1, "presets": app.presets}
    tmp = PRESETS_FILE + ".tmp"
    try:
        with open(tmp, "w") as f:
            json.dump(data, f)
        try:
            os.remove(PRESETS_FILE)
        except Exception:
            pass
        os.rename(tmp, PRESETS_FILE)
    except Exception as ex:
        print("[preset] write failed:", ex)


def _load_presets():
    slots = [None] * NUM_PRESETS
    if os is None:
        return slots
    try:
        with open(PRESETS_FILE) as f:
            data = json.load(f)
        arr = data.get("presets", [])
        for i in range(min(NUM_PRESETS, len(arr))):
            slots[i] = arr[i]
    except Exception:
        pass
    return slots


# --- copy / paste (a clipboard, like drumster8's bank copy/paste) -------
#
# COPY snapshots the LIVE sequence to a clipboard; PASTE writes it back
# onto the live sequence. Combined with the preset slots this is the
# quickest way to duplicate a pattern: COPY, pick another SAVE slot,
# PASTE, then SAVE - or just PASTE onto the live sequence to audition it.

def clip_copy(e=None):
    app.clipboard = _snapshot()
    if app.setup_page is not None:
        app.setup_page.copy_btn.flash()
        app.setup_page.info.set_text("copied")


def clip_paste(e=None):
    if app.clipboard is None:
        if app.setup_page is not None:
            app.setup_page.info.set_text("clipboard empty")
        return
    _apply_snapshot(app.clipboard)
    if app.playing:
        start_playback()
    if app.setup_page is not None:
        app.setup_page.paste_btn.flash()
        app.setup_page.info.set_text("pasted")


# --- named projects (saved to disk under a typed name) ------------------

def _load_projects():
    if os is None:
        return []
    try:
        with open(PROJECTS_FILE) as f:
            data = json.load(f)
        projs = data.get("projects", [])
        return [p for p in projs if isinstance(p, dict) and "data" in p]
    except Exception:
        return []


def _write_projects():
    if os is None:
        return
    data = {"v": 1, "projects": app.projects}
    tmp = PROJECTS_FILE + ".tmp"
    try:
        with open(tmp, "w") as f:
            json.dump(data, f)
        try:
            os.remove(PROJECTS_FILE)
        except Exception:
            pass
        os.rename(tmp, PROJECTS_FILE)
    except Exception as ex:
        print("[project] write failed:", ex)


def do_save_project(name):
    name = (name or "").strip()
    if not name:
        name = "Project %d" % (len(app.projects) + 1)
    entry = {"name": name[:MAX_PROJECT_NAME_LEN], "data": _snapshot()}
    # replace a same-named project, else append (capped)
    for i, p in enumerate(app.projects):
        if p.get("name") == entry["name"]:
            app.projects[i] = entry
            app.project_idx = i
            break
    else:
        if len(app.projects) >= MAX_PROJECTS:
            app.projects.pop(0)
        app.projects.append(entry)
        app.project_idx = len(app.projects) - 1
    _write_projects()
    if app.setup_page is not None:
        app.setup_page.refresh_projects()
        app.setup_page.info.set_text("saved '%s'" % entry["name"])
    return True


def project_prev(e=None):
    if not app.projects:
        return
    app.project_idx = (app.project_idx - 1) % len(app.projects)
    app.setup_page.refresh_projects()


def project_next(e=None):
    if not app.projects:
        return
    app.project_idx = (app.project_idx + 1) % len(app.projects)
    app.setup_page.refresh_projects()


def project_load(e=None):
    if not app.projects or app.project_idx < 0:
        app.setup_page.info.set_text("no project selected")
        return
    p = app.projects[app.project_idx]
    _apply_snapshot(p["data"])
    if app.playing:
        start_playback()
    app.setup_page.refresh()
    app.setup_page.info.set_text("loaded '%s'" % p.get("name", "?"))


def open_save_project(e=None):
    app.save_popup.show()


# =====================================================================
# DEFAULT PATTERN  (something musical on first boot)
# =====================================================================

def _load_default():
    # a gentle rising line, quantised by the active scale
    pitches = [0, 3, 5, 7, 10, 7, 5, 3]
    pulses = [1, 1, 2, 1, 1, 2, 1, 1]
    gates = [GATE_MULT, GATE_MULT, GATE_SING, GATE_MULT,
             GATE_MULT, GATE_HOLD, GATE_MULT, GATE_SING]
    for i in range(NUM_STAGES):
        s = app.stages[i]
        s.pitch = pitches[i]
        s.pulses = pulses[i]
        s.gate = gates[i]
        s.slide = False
        s.skip = False
        s.ratchet = 1
        s.prob = 1.0
        s.accum = 0
    # TRK 1: straight forward. TRK 2: reverse, slower, up an octave -
    # instant counterpoint from the same stages.
    app.tracks[0].order = ORD_FWD
    app.tracks[0].length = 8
    app.tracks[0].div_i = 0
    app.tracks[1].order = ORD_REV
    app.tracks[1].length = 8
    app.tracks[1].div_i = 1     # half speed
    app.tracks[1].octave = 1

    # a ready-to-use MOD lane 1: a slow triangle shape routed to TRK 1
    # transpose, but with DEPTH at 0 so it stays inert until you raise it
    # on the MOD page (nudge Depth up to hear TRK 1 drift).
    tri = [-1.0, -0.6, -0.2, 0.2, 0.6, 1.0, 0.4, -0.4]
    lane = app.mod_lanes[0]
    for i in range(NUM_STAGES):
        lane.values[i] = tri[i]
    lane.dest = 1               # T1 Transpose
    lane.depth = 0.0
    lane.div_i = 3              # 1/4-note LFO


# =====================================================================
# LIFECYCLE
# =====================================================================

def quit(screen):
    try:
        cv.all_low()
    except Exception:
        pass
    try:
        for t in range(NUM_TRACKS):
            audio_note_off(t)
    except Exception:
        pass
    try:
        if getattr(screen, "clock", None) is not None:
            screen.clock.clear()
            screen.clock = None
    except Exception:
        pass


def run(screen):
    global app

    if app is not None:
        try:
            quit(app)
        except Exception:
            pass

    # When Tulip's app launcher runs a packaged app it hands us a live,
    # already-presented UIScreen. When this file is run on its own
    # (run('metro_seq.py') -> the __main__ block below -> run(None)), there
    # is no such screen, so we make one AND must present() it ourselves -
    # otherwise the UI is built but never shown and you're left at the REPL.
    created_screen = not hasattr(screen, "add")
    if created_screen:
        screen = tulip.UIScreen()

    app = screen
    app.set_bg_color(C_BG)
    app.quit_callback = quit

    # --- state ---
    app.bpm = 120
    app.playing = False
    app.page = 0
    app.rebuilding = False
    app.root = 0
    app.scale_idx = 0
    app.scale_ivals = SCALES[0][1]
    app.base_octave = 0            # global SEQ pitch-range offset (octaves)
    app.merge_cv = False           # TRK2 -> TRK1 shared CV out (priority)
    app._shared_high = False       # is the shared out gate high (merge)
    app._shared_owner = None       # which track owns the shared out
    app.cur_preset = None          # last loaded/saved preset slot
    app.clipboard = None           # COPY/PASTE snapshot
    app.project_idx = -1           # selected named project
    app.ticks_per_16th = ticks_per_16th()
    app.clk = 0
    app._last16 = 0
    app._base_tick = None
    app._last_disp = [None] * NUM_TRACKS
    app._last_mod_disp = None
    app.clock = None

    app.stages = [Stage() for _ in range(NUM_STAGES)]
    app.tracks = [Track(i) for i in range(NUM_TRACKS)]
    app.mod_lanes = [ModLane(i) for i in range(NUM_MOD_LANES)]
    app.mod_sel = 0
    app.mod_bus = {}
    app.mod_page = None
    app.presets = _load_presets()
    app.projects = _load_projects()

    _load_default()

    # --- engine ---
    try:
        amy.reset()
    except Exception as ex:
        print("amy.reset failed:", ex)
    audio_init()
    try:
        sequencer.tempo(app.bpm)
    except Exception:
        pass

    # --- UI ---
    app.header = Header(app.group)
    app.seq_page = SeqPage(app.group)
    app.track_page = TrackPage(app.group)
    app.mod_page = ModPage(app.group)
    app.setup_page = SetupPage(app.group)
    app.detail_popup = DetailPopup(app.group)
    app.save_popup = SaveProjectPopup(app.group)

    recompute_mod_bus()

    app.header.set_bpm(app.bpm)
    app.header.refresh_cv()
    show_page(0)
    _update_play_btn()

    make_clock()

    if CV_AUTOSTART:
        defer_safe(cv_autostart, None, CV_INIT_DELAY_MS)

    # Show it. This is THE step that was missing - a UIScreen is built off
    # screen but stays invisible until present() is called (Tulip's app
    # protocol: `def run(screen): ...; screen.present()`). Called
    # unconditionally: presenting an already-presented screen is a no-op /
    # harmless, and this covers both launch paths (the __main__ self-run
    # below, and a launcher that hands us a screen).
    try:
        app.present()
    except Exception as ex:
        print("screen present() failed:", ex)

    print("metropolix_tulip ready - press PLAY")


# Allow  run('metropolix_tulip.py')  and  import; run(None)
if __name__ == "__main__":
    run(None)
