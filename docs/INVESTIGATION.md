# Native C on the Tulip CC: investigation and design

The question was whether `metropolix_tulip.py` (a Metropolix-style CV sequencer
written for the Tulip CC's MicroPython/AMY firmware) can become bare C firmware
that runs on the Tulip hardware with no Tulip/AMY firmware. The goals: the same
UI and functionality, CV gate and pitch through the Mabee DAC(s), and, above
all, **stable timing with no stalls or halts in the sequence**.

**Short answer: yes.** This repo contains a working port. The engine is tested on
the host against the original Python, and the firmware builds for ESP-IDF 5.3
with zero warnings. It has **not been run on a Tulip yet**: see
[What still needs hardware](#what-still-needs-hardware).

---

## 1. Why the Python version can stall

All of these come from reading the sketch and Tulip's firmware source
(`shorepine/tulipcc`):

| Mechanism | Where | Effect on the sequence |
|---|---|---|
| The AMY sequencer hook queues the Python clock callback with `mp_sched_schedule` (`tulip/shared/tsequencer.c:33`). It runs on the **one MicroPython thread**, between bytecodes. | `_master_tick` | Any long Python work (a page `refresh()`, a JSON save, a big LVGL redraw) delays the clock. The sketch's own `MAX_CATCHUP` resync exists for this reason. |
| LVGL's `lv_task_handler` is queued onto the **same** thread (`modtulip.c:1243`). | every redraw | A redraw and a step can't run at the same time; a slow redraw pushes the step back. |
| Gate-offs, swing, ratchets and glides all use `tulip.defer(ms)`, with **1 ms resolution**, dispatched from that same thread. | `_cv_raise`, `_fire_pulse`, `cv_set_pitch` | Jitter is roughly whatever else the interpreter is doing at that moment. Glides are 8 coarse steps. |
| Python allocation in the hot path (closures for every defer, dict lookups, `%` formatting) | everywhere | Causes garbage-collector pauses, which stop the thread outright. |
| CV writes go through `machine.I2C` at 100 kHz, on the bus that Tulip's touch task polls with its own driver. | `_Mabee.write` | A 3-byte write takes about 0.4 ms of wire time plus interpreter overhead, and can queue behind a touch read. |
| Preset/project saves write FAT files in flash. | `_write_presets` | A flash erase can freeze both cores for tens of ms unless code runs from PSRAM. |

None of these is a bug in the sketch. They come from running a real-time job in an
interpreter that shares one thread with the UI. A native port can remove every
one of them.

## 2. The hardware (from the Tulip source, not guessed)

Taken from `tulip/esp32s3/pins.h` (MAKERFABS + TULIP4_R11), `boards/N32R8`,
`esp32s3_display.c` and `gt911_touchscreen.c`. All of it is in
`firmware/main/platform/board.h`.

* **MCU:** ESP32-S3 at 240 MHz, two cores, 32 MB octal flash, 8 MB octal PSRAM.
* **Display:** 1024×600 RGB panel. It is wired for **8-bit RGB332** (B6 B7 G5 G6 G7 R5 R6
  R7), pclk 28 MHz, and the backlight PWM on GPIO47 is inverted.
* **Touch:** GT911 at 0x5D or 0x14, reset on GPIO48 (it has to be toggled twice on R11).
  Tulip's calibration is x−2, (y−12)·0.8.
* **I2C:** SDA 17 / SCL 18, with external pull-ups. **The Mabee DAC(s) share this bus with the
  touch controller.** That is the most important hardware fact for timing.
* **Mabee DAC (GP8413):** 0x59 drives TRK1 and 0x58 (A0 link cut) drives TRK2. Register 0x02 is
  VOUT0 (gate) and 0x04 is VOUT1 (pitch). Values are 16-bit little endian with 15
  significant bits, and range register 0x01 = 0x11 selects 0–10 V. Tulip's own `mabeedac.py` drives this bus at
  400 kHz.
* **Audio:** I2S on LRCLK 2, BCLK 8, DOUT 5. That is enough to rebuild the audio preview without AMY.

## 3. Design for solid timing

```
 core 1 (nothing else pinned here)          core 0
 ┌───────────────────────────────┐   ┌──────────────────────────────────┐
 │ seq task  prio 23             │   │ RGB panel bounce-buffer ISR      │
 │  GPTimer alarm ─► notify      │   │ lvgl task  prio 4  (UI, flush)   │
 │  spin last 40 µs to deadline  │   │ audio task prio 6  (I2S preview) │
 │  engine.advance(now)          │   │ store task prio 2  (NVS commits) │
 │  I2C: DAC writes at deadline  │   └──────────────────────────────────┘
 │       touch reads only in gaps│        ▲ touch events (queue)
 └──────────────┬────────────────┘        │ g_status (playheads, stats)
                └─ seq_cmd() queue ◄──────┘ UI commands (non-blocking)
```

### 3.1 One clock, absolute time
The engine (`engine/seq_engine.c`) never says "N ms from now". Every event has an
**absolute timestamp in µs** on a free-running 1 MHz GPTimer. Master pulse *n* is
computed as `anchor + n·15 000 000 / bpm`, a product rather than a running sum, so
**it cannot drift**. The host test confirms zero µs grid error after one simulated hour at 133 BPM.
A tempo change re-anchors at the last pulse, so the grid continues without a jump.

### 3.2 A dedicated core and a precise wake
The sequencer task is pinned to core 1 at priority 23, and nothing else is pinned
there. It sleeps on a GPTimer alarm (IRAM-safe ISR → task notify) set 40 µs
before the next event, then spins on the counter to the exact deadline.
Interrupt and context-switch latency (a few µs) is absorbed by the spin instead
of turning into gate jitter.

### 3.3 The shared I2C bus is scheduled, not contended
The sequencer task is the **only** user of the bus. CV writes go out at their
deadline. A touch read (worst case ~4 ms at 100 kHz, ~1 ms at 400 kHz) starts only
when the gap to the next event is longer than that worst case. Otherwise it is
deferred until just after the event. A touch read can therefore **never** sit on the bus when a gate is
due. Touch events go to LVGL through a queue, so a quick tap is never lost
while the UI is busy.

Latency budget per write, at the defaults (DAC 400 kHz, touch 100 kHz):

| Write | Time | Notes |
|---|---|---|
| One gate or pitch write (3 bytes) | ~0.1 ms wire + ~0.03 ms driver | ~0.4 ms at 100 kHz |
| Note-on (pitch, then gate) | gate edge ~0.25 ms after nominal | **Constant**, so it is latency, not jitter. Pitch always settles before the gate rises. |
| Both tracks change note on the same 1/16 | TRK2 gate edge ~0.5 ms after nominal | Fixed order every time |

Repeated identical pitch writes are skipped (no bus time when nothing changes).

### 3.4 Lock-free hand-off between UI and engine
* The UI edits the pattern with single-field writes, which are atomic on Xtensa. Whole-pattern
  swaps (preset load, paste) use a spinlock that the engine holds only
  while it *computes* a batch (microseconds), never during I2C.
* The engine emits a list of actions, releases the lock, then the task executes the
  I/O. **The UI never waits on the sequencer, and the sequencer never waits on the UI.**
  Playhead LEDs are drawn by the UI polling published state, so a slow redraw
  delays an LED, never a gate.

### 3.5 Flash writes can't freeze the CPU
* `CONFIG_SPIRAM_XIP_FROM_PSRAM=y`: code and rodata run from PSRAM, so the IDF docs say
  flash erase/program **does not disable the caches**, and both cores keep running.
* As a second layer, `CONFIG_METRO_DEFER_FLASH_WHILE_PLAYING` (default on) commits
  a save to NVS only after STOP. The RAM bank updates immediately, so a saved
  preset can be loaded at once.
* Presets are versioned binary NVS blobs, which are power-fail safe and wear levelled. They are
  sanitised on load, so a corrupt blob can't push the engine out of range.

### 3.6 Graceful behaviour when something does go wrong
* **Stall resync** (same policy as the Python `MAX_CATCHUP`): more than 2 pulses behind means
  re-anchor and drop the backlog, with no flam.
* Swung or ratcheted gate-ons that come due >20 ms late are **dropped**. Gate-offs always run,
  so nothing is left stuck high.
* A glide that falls behind jumps to its present value instead of replaying missed steps.
* The engine stops a batch with headroom left and finishes it on the next pass, so actions
  can't be lost.
* A DAC that fails 5 times in a row is dropped with a header message (as in the Python), and
  the other track is unaffected.

### 3.7 Deliberate behaviour changes vs the Python
Each one removes a way the original could glitch. All are marked `FIX:` in the code.

1. **Minimum 1 ms gate low before a retrigger.** At 100% gate the Python retriggered with ~0 ms
   low time, which many modules miss.
2. **HOLD release is keyed on "a hold is open", not on the stage's *current* mode.** Editing a
   HOLD stage mid-hold could leave the gate stuck high.
3. **Swing uses the track's own pulse parity.** The Python used master-1/16 parity, so a track on
   /8 or slower (always on even pulses) could never swing. /16 tracks behave identically.
4. **STOP and reload flush every queued event**, so no swung hit fires after STOP and no HOLD
   survives a preset load.
5. **Glides are a smooth 500 Hz ramp** instead of 8 steps.

### 3.8 Display
The esp_lcd RGB driver scans an 8-bit framebuffer in PSRAM through two internal-RAM
bounce buffers (12 rows each, the same as Tulip). LVGL 9.2 renders RGB565 into an 80 KB
internal band buffer, and the flush packs it to RGB332. UI colours are defined as
the sketch's exact RGB332 values, so the screen matches what the Python app
showed. The bounce ISR and all rendering stay on core 0.

## 4. Feature parity

Everything in the sketch is ported:

* **SEQ:** 8 stages with pitch slider, note name, pulse count, gate type, SLD, SKP, two
  playhead LEDs, and a stage-detail popup (ratchet, probability, accumulate).
* **TRK:** order (FWD/REV/PEND/RAND/BRWN), length, clock div, swing, gate length, transpose,
  octave, invert, and audio mute per track.
* **MOD:** 8 lanes with bipolar sliders, order, length, div, depth, and 13 routing destinations.
* **SET:** root, scale (11 scales), base octave, CV on/off, rescan, MERGE T2>T1 with TRK1 priority,
  audio-preview toggle, 16 presets, copy/paste, and named projects with the on-screen keyboard.
* **Audio preview** without AMY: two saw voices with the same envelope and filter settings, on I2S.
* **New:** a timing panel on SET showing live gate lateness (avg/max/histogram), resyncs, late-drops,
  I2C errors and deferred touch reads, with a CLR button.

## 5. What was verified (without hardware)

| Check | How | Result |
|---|---|---|
| Engine matches the original | `tests/py_parity.py` runs **the original Python engine** (stubbed Tulip/AMY, virtual clock) and the C engine on 6 patterns: default, ratchet + swing + SING, pendulum/skip/accum/quantiser, merge priority, 3 routed mod lanes, slide + 173 BPM + dotted divs | Every gate edge matches in time (±1 ms, the Python's defer resolution) and pitch. The test is shown to fail when a difference is injected. |
| No drift | 1 h at 133 BPM | 0 µs grid error |
| Timing invariants | `tests/test_engine.c` covers pitch-before-gate, ratchet gaps at 300 BPM, stall resync, small-lateness catch-up, tempo re-anchor, merge priority, HOLD edit, STOP flush, /8 swing and glide | all pass under ASan/UBSan |
| Robustness | fuzz: 300 × 20 s random patterns with random live edits, commands and stalls | 0 queue overflows, 0 dropped actions, output always in time order |
| Firmware | `idf.py build` (ESP-IDF 5.3.2, esp32s3) | 0 warnings; 241 KB internal RAM free statically |
| UI layout | `tools/ui_preview` renders the **real UI code** on the host through the device's RGB565→RGB332 conversion | all pages and popups checked visually |

## 6. What still needs hardware

These can only be confirmed on a Tulip:

1. **Panel bring-up.** The timings and pins are Tulip's, but this uses the driver's own framebuffer and
   bounce path rather than Tulip's custom bounce callback. Expect it to work. If the image is
   shifted or flickers, check `pclk_active_neg` and the porches.
2. **Touch.** Check the reset sequence and calibration on your unit. If 100 kHz is reliable, try
   `METRO_TOUCH_I2C_HZ=400000`.
3. **DAC at 400 kHz** on your cable. Tulip's `mabeedac.py` uses 400 kHz on this bus, but fall back to
   `METRO_DAC_I2C_HZ=100000` if you see I2C errors on the timing panel.
4. **1 V/oct calibration.** Measure C2 and C6 on VOUT1 and enter gain/offset in `cv_dac.c` `CAL[]`.
   The GP8413 is 15-bit (0.3 mV/LSB ≈ 0.4 cent), so calibration, not resolution, sets accuracy.
5. **Timing numbers.** Watch the SET-page timing panel while hammering the UI (drag sliders,
   switch pages, save presets). The target is `>1ms 0`, with max lateness ≲0.6 ms at 400 kHz. The worst case is both tracks changing note on the same pulse, and that worst case is constant.
   For a scope check, set `METRO_TIMING_PROBE_GPIO=11`: the probe edge marks the scheduled
   instant and VOUT0 shows the I2C latency after it.

### Known limits / possible next steps
* A **CV RESCAN while playing** probes the bus synchronously (a few ms at most if a DAC is absent).
  It is user-initiated and rare, but could move into the gap scheduler like touch.
* LVGL draws into the live framebuffer, so minor tearing is possible during big redraws (Tulip has the same
  behaviour). A second PSRAM framebuffer swapped in vsync would remove it.
* Note-on latency could drop to a single write by pre-writing the next stage's pitch
  as soon as the previous gate falls (except for legato).
* The preview synth is intentionally simple. It's for monitoring, not a voice.
* External clock/reset in, MIDI clock and per-stage CV in are not in the sketch and are not added.
