# metroseq for Tulip CC (native C)

A port of [`reference/metropolix_tulip.py`](reference/metropolix_tulip.py), a
Metropolix-style two-track CV sequencer for the Tulip Creative Computer, to
**native ESP32-S3 firmware**. It runs on the Tulip CC v4 hardware without the
Tulip / MicroPython / AMY firmware. It keeps the same touch UI and features, and
outputs gate and 1 V/oct pitch through Makerfabs Mabee (GP8413) DACs. The design
priority is **rock-solid timing**: the sequencer has a CPU core to itself and the
UI can never stall it.

How and why it's built this way, what was verified and what still needs a real
Tulip: **[docs/INVESTIGATION.md](docs/INVESTIGATION.md)**.

| SEQ: 8 shared stages | TRK: per-track transforms |
|---|---|
| ![SEQ](docs/img/1_seq.png) | ![TRK](docs/img/3_trk.png) |
| **MOD: 8 routable lanes** | **SET: quantiser, CV, presets, projects, timing** |
| ![MOD](docs/img/4_mod.png) | ![SET](docs/img/5_set.png) |

*(Rendered from the firmware's own UI code by `tools/ui_preview`, through the panel's RGB332 conversion.)*

## Layout

```
firmware/                 ESP-IDF 5.3 project (target esp32s3)
  main/engine/            sequencer engine: pure C, no ESP deps, host-testable
  main/platform/          seq task (timer, I2C, DACs, touch), display, audio, storage
  main/ui/                LVGL port of every page in the sketch
  components/lvgl/        LVGL v9.2.2 (git submodule)
tests/                    engine unit tests, fuzz, and parity vs the Python original
tools/ui_preview/         renders the real UI code to PNGs on a PC
reference/                the original MicroPython sketch
```

## Build and flash

You need [ESP-IDF v5.3.x](https://docs.espressif.com/projects/esp-idf/en/v5.3.2/esp32s3/get-started/).

```sh
git clone --recursive <this repo>          # or: git submodule update --init
cd firmware
idf.py set-target esp32s3
idf.py menuconfig                           # optional: "Metroseq" menu
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor        # top (UART) USB port on the Tulip CC
```

This **replaces the Tulip firmware**. `idf.py flash` writes only below 0x350000;
Tulip keeps `/user` at 0xA30000, so your files should survive. Back them up anyway.
To go back to Tulip, flash a Tulip release with
`esptool.py write_flash 0x0 tulip-full-XXX.bin`
(see [Tulip's flashing guide](https://github.com/shorepine/tulipcc/blob/main/docs/tulip_flashing.md)).

### Options (`idf.py menuconfig` → Metroseq)

| Option | Default | |
|---|---|---|
| `METRO_DAC_I2C_HZ` | 400000 | DAC write speed. Lower to 100000 if the timing panel shows I2C errors |
| `METRO_TOUCH_I2C_HZ` | 100000 | GT911 speed (Tulip's value) |
| `METRO_CV_SET_RANGE_10V` | y | Force each GP8413 into its 0–10 V range at boot |
| `METRO_DEFER_FLASH_WHILE_PLAYING` | y | Preset/project flash commits wait for STOP |
| `METRO_TIMING_PROBE_GPIO` | -1 | Mirror TRK1 gate timing onto a pin for a scope (GPIO11 = MIDI OUT) |

## CV wiring (same as the sketch)

* **TRK 1:** Mabee at 0x59 (default). VOUT0 = gate (0/5 V), VOUT1 = pitch (1 V/oct, 0 V = C0).
* **TRK 2:** a second Mabee with its A0 link cut (0x58). It is optional.
* **MERGE T2>T1** (SET page): both tracks share DAC 0x59, and TRK 1 has priority.
* If the Tulip browns out at power-on with a DAC attached, power the DAC from its own 5 V
  supply with a shared ground.

Calibrate 1 V/oct by measuring VOUT1 at two notes and editing `CAL[]` in
`firmware/main/platform/cv_dac.c`.

## Tests (no hardware needed)

```sh
make -C tests test          # engine unit tests + fuzz (ASan/UBSan) + Python parity
make -C tools/ui_preview    # needs Pillow; writes tools/ui_preview/build/*.png
```

The parity test runs the **original Python engine** with stubbed Tulip/AMY
modules on a virtual clock, then checks the C engine produces the same gate edges
at the same times with the same pitches.
