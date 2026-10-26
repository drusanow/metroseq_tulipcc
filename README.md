# Tulip CC in C: launcher + metroseq

Native ESP32-S3 firmware for the [Tulip Creative Computer](https://github.com/shorepine/tulipcc),
with no MicroPython or AMY:

* **[The launcher](docs/LAUNCHER.md)**: a minimal dark home screen.
  * Launch C apps from tiles or a file browser; make folders, rename and delete.
  * Join Wi-Fi, then move apps and files from **any browser** (`http://tulip.local`) or
    a terminal (`tools/tulip.py`).
  * Settings, touch calibration, and a **debug REPL** (on screen and over USB serial).
* **metroseq**: a port of [`reference/metropolix_tulip.py`](reference/metropolix_tulip.py).
  It is a Metropolix-style two-track CV sequencer driving Makerfabs Mabee (GP8413) DACs,
  with rock-solid timing: the sequencer has a core to itself. It is now an app under the
  launcher. Design and verification are in **[docs/INVESTIGATION.md](docs/INVESTIGATION.md)**.
* **hello**: the smallest app, as a template for your own.

| Launcher | Files |
|---|---|
| ![home](docs/img/launcher_1_home.png) | ![files](docs/img/launcher_2_files.png) |
| **metroseq** | **Web transfer** |
| ![seq](docs/img/1_seq.png) | ![web](docs/img/web_app.png) |

*(Device screens are rendered from the real UI code by `tools/ui_preview`.)*

## Layout

```
launcher/            the launcher (factory app): UI, file browser, Wi-Fi, web server, REPL
  main/web/          the web app (one self-contained HTML file)
apps/metroseq/       the CV sequencer app
apps/hello/          minimal app template
components/
  tulip_hal/         shared by every app: panel + LVGL, touch + calibration, I2C,
                     /files, settings, app lifecycle; partition table + base sdkconfig
  lvgl/              LVGL v9.2.2 (git submodule)
  mdns/              vendored from espressif/esp-protocols (tulip.local)
tools/tulip.py       push/pull/launch over Wi-Fi (Python standard library only)
tools/make_full_image.sh   build everything into tulip-full.bin
tools/ui_preview/    render the on-device UIs to PNGs on a PC
tests/               host tests (engine, MIDI clock, touch cal, launcher core) and
                     end-to-end web/CLI tests against a mock device
reference/           the original MicroPython sketch
```

## Flash it

**First time:** flash the full image over USB. It holds the launcher, with metroseq
preloaded:

```sh
pip install esptool
esptool.py --chip esp32s3 write_flash 0x0 tulip-full.bin    # top (UART) USB port
```

This replaces the Tulip firmware **and the area where Tulip keeps `/user`**, so back
your files up first. To go back, flash a [Tulip release](https://github.com/shorepine/tulipcc/releases)
the same way.

**After that:** put apps on over Wi-Fi from the launcher's WI-FI page. See
[docs/LAUNCHER.md](docs/LAUNCHER.md).

## Build it

You need [ESP-IDF v5.3.x](https://docs.espressif.com/projects/esp-idf/en/v5.3.2/esp32s3/get-started/).

```sh
git clone --recursive <this repo>        # or: git submodule update --init
. $IDF_PATH/export.sh
tools/make_full_image.sh                 # -> tulip-full.bin (launcher + metroseq)

# develop an app:
cd apps/hello && idf.py set-target esp32s3 && idf.py build
../../tools/tulip.py --pin <PIN> push build/hello.bin --launch
```

**Apps are plain ESP-IDF `.bin` images** built on `components/tulip_hal/sdkconfig.tulip`.
The launcher reads each app's name and version from the image itself. The reasons,
and the rules an app must follow, are in [docs/LAUNCHER.md](docs/LAUNCHER.md#2-packaging-apps-recommendation).
Don't `idf.py flash` an app, because that overwrites the launcher; push it instead.

## metroseq options (`idf.py menuconfig` → Metroseq)

| Option | Default | |
|---|---|---|
| `METRO_DAC_I2C_HZ` | 400000 | DAC write speed. Lower to 100000 if the timing panel shows I2C errors |
| `METRO_TOUCH_I2C_HZ` | 100000 | GT911 speed (Tulip's value) |
| `METRO_CV_SET_RANGE_10V` | y | Force each GP8413 into its 0–10 V range at boot |
| `METRO_DEFER_FLASH_WHILE_PLAYING` | y | Preset/project flash commits wait for STOP |
| `METRO_TIMING_PROBE_GPIO` | -1 | Mirror TRK1 gate timing onto a pin for a scope (GPIO11 = MIDI OUT) |

**CV wiring:**
* **TRK 1:** Mabee at 0x59. VOUT0 = gate (0/5 V), VOUT1 = pitch, 1 V/oct over the full
  0–10 V (C0 = 0 V, C10 = 10 V).
* **TRK 2:** optional second Mabee with its A0 link cut (0x58).
* **MERGE T2>T1:** both tracks share 0x59, with TRK 1 taking priority.
* **MIDI IN:** clock and transport, enabled with SET → `CLOCK: MIDI`.
* **Calibration:** measure VOUT1 at two notes and edit `CAL[]` in
  `apps/metroseq/main/platform/cv_dac.c`.
* **Power:** if the Tulip browns out at power-on with a DAC attached, power the DAC from
  its own 5 V supply with a shared ground.

## Tests (no hardware needed)

```sh
make -C tests test            # engine, MIDI clock, touch cal, launcher core, fuzz, Python parity
python3 tests/test_web.py     # tulip.py + the web app in headless Chromium vs a mock device
make -C tools/ui_preview      # render every screen (needs Pillow)
python3 tests/web_mock.py     # run the web app locally against a mock device
```
