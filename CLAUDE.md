# Notes for coding assistants (Claude Code, and others via AGENTS.md)

TULIP STUDIO is native C firmware for the Tulip Creative Computer (ESP32-S3,
ESP-IDF 5.3, LVGL 9). It is one image, `apps/studio` → `studio.bin`, holding
every app in `modules/`. Each app runs either on its own or in the Studio,
together with the others on one master clock. `launcher/` is the start page
and file manager.

Read `README.md` for the layout and `docs/APPS.md` before writing an app.

## Making a new app (synth, drum machine, sequencer ...)

1. Copy `docs/templates/mod_example/` to `modules/mod_<name>/`. Rename
   `example` everywhere: the folder, the header, `mod_example`, `.name`,
   `app.json`.
2. Pick the role in `app.json` and the `tmod_t`: `sequencer`, `drums` or
   `synth`. `docs/APPS.md` §3 says what each role gets: CV, MIDI in / out,
   MIDI clock, the CV clock.
3. Learn from the nearest real app:
   * `mod_sloe`: a small synth;
   * `mod_wavetron`: a large synth, with a host-tested `core/`;
   * `mod_gintonix` and `mod_drumster9`: drum machines on the clock's timeline;
   * `mod_metroseq`: a sequencer with CV and routed note tracks.
4. Keep the DSP or sequencer logic in plain C with no ESP-IDF or LVGL calls
   (a `core/` folder). Then it can be tested on the PC: add a test in `tests/`
   modelled on `tests/test_sloe.c` and a target in `tests/Makefile`.
5. Nothing else is edited to add an app. The build finds every
   `modules/*/app.json`.

## Rules that keep the audio and timing solid (docs/APPS.md §5)

* **Never block** in `render`, `note`, `midi` or any `rt_*` hook:
  * no mutex waits, file I/O, `printf` or `malloc`;
  * hand data over with `xQueueSend(.., 0)` or single-word writes.
* `render` **adds** into `l` / `r`, `STUDIO_BLOCK` (48) frames per call.
  * Map it `(noflash)` in the app's `linker.lf`.
  * Use tables made in `init()`, not `sinf` / `powf` per sample.
* **`init()` may run while other apps play** (when the app is switched on in the
  Studio): keep it short. Big buffers go in PSRAM (`MALLOC_CAP_SPIRAM`).
* **Every app links into one image:** make functions and globals `static`, or
  prefix them.
* **Files** go in `/files/<name>/` (`studio_files_root()`). Save from the UI task,
  a moment after the last change, and finish pending saves in `busy()`.
* **The DACs:** only a sequencer with `TMOD_F_CV` drives them. Drum machines get
  the runtime's CV clock.
* **The UI:**
  * use the shared widgets in `studio/widgets.h` (`sw_btn`, `sw_label`,
    `sw_slider` ...), a title at the top left and `studio_chrome(scr)`;
  * on the settings page, `studio_io_panel()`.
* **Studio patches:** implement `patch_save` / `patch_load` (cJSON). Missing keys
  keep their defaults.

## Build and check

```sh
. $IDF_PATH/export.sh            # ESP-IDF v5.3.x
export IDF_COMPONENT_MANAGER=0   # every component is in the repo
cd apps/studio && idf.py build   # must finish with no warnings (-Werror for most)
make -C tests test               # host tests: must all pass
make -C tools/ui_preview         # renders screens to tools/ui_preview/build/*.png (Pillow)
tools/build_elf_app.sh modules/mod_<name>   # optional: the app as a loadable .elf
```

* No ESP-IDF here? Push the branch. `.github/workflows/build.yml` builds
  everything and runs the tests, and the `tulip-studio` artifact holds
  `studio.bin`.
* `tools/ui_preview` lists its modules in its `Makefile`. Add a new app there,
  with stubs if needed, to see its screens.

## Don't

* `idf.py flash` an app: it overwrites the launcher. Push `studio.bin` with
  `tools/tulip.py --pin <PIN> push studio.bin --launch`, or from the browser
  page.
* Edit generated files (`apps.gen.c`, `abi_exports.c`), or the vendored
  components: `components/lvgl`, `components/mdns`, `components/elf_loader`.
* Reorder the first five apps (`metroseq gintonix sloe wavetron drumster9`):
  saved routes and patches go by position.
* Commit build output (`build/`, `*.bin`, `sdkconfig` under `apps/`).
* Say something works on the Tulip when it has only built. Assistants have no
  hardware: say what was built and tested on the PC, and list what the person
  should try on the device (sound, touch, MIDI, CV).

## Style

Match the code around you: C11, 4-space indent, `static` helpers, short
comments that say why rather than what, and plain-English user-facing text.
Docs live in `docs/`. When an app's behaviour changes, update its page there
and `README.md`.
