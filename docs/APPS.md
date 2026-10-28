# Writing a Tulip Studio app

Every app in Tulip Studio is written once and runs two ways, with no extra code:

* **On its own:** its tile on the launcher's start page. The Tulip runs just that app,
  full screen. The red square goes back to the launcher.
* **In the Studio:** with the other apps on one master clock. Every app starts **OFF**.
  You switch on the ones you want on the Studio home page, and only those use any CPU,
  RAM or audio time.

This page is the contract an app must keep to work both ways.
[`docs/templates/mod_example`](templates/mod_example) is a complete small app (a sine synth) to copy.

---

## 1. What an app is

An app is a folder `modules/mod_<name>/` holding an ESP-IDF component:

```
modules/mod_<name>/
  app.json            what the launcher and the Studio show (below)
  CMakeLists.txt      idf_component_register(... REQUIRES tulip_studio lvgl json)
  mod_<name>.h        extern tmod_t mod_<name>;
  linker.lf           puts the audio / real-time code in internal RAM (section 5)
  *.c                 the app
```

`<name>` is short, lower case, and the same in five places:

* the folder `mod_<name>`;
* the component `mod_<name>`;
* the header `mod_<name>.h`;
* the `tmod_t` variable `mod_<name>`;
* `.name` in the `tmod_t`, and `"name"` in `app.json`.

### `app.json`

```json
{
    "name": "example",
    "title": "EXAMPLE",
    "role": "synth",
    "blurb": "Template: a sine synth that plays MIDI IN and routed tracks"
}
```

| Key | Limit | |
|---|---|---|
| `name` | 15 characters | the app's id: its files (`/files/<name>/`), its settings, the launcher's boot mode |
| `title` | 15 characters | the tile's name |
| `role` | `sequencer`, `drums` or `synth` | what the runtime gives it (section 3) |
| `blurb` | 79 characters | one line on the tiles |

The build checks all of these and stops with a message if one is missing or too long.
Keep them the same as in the `tmod_t`. If `name` or `role` differ, Studio logs a warning
at start-up.

## 2. Adding an app

1. Copy `docs/templates/mod_example` to `modules/mod_<name>/` and rename everything
   that says `example`.
2. Build Tulip Studio:
   ```sh
   cd apps/studio && idf.py build
   ```
   Nothing else needs editing. The build finds every `modules/*/app.json` and
   generates:
   * the module table `studio_run()` gets;
   * the **manifest**: the list of apps inside `studio.bin`, which the launcher reads.

   The first five apps keep their places (saved routes and patches go by position).
   New apps come after them, sorted by name. The image holds 16 apps at most.
3. Upload it:
   ```sh
   ../../tools/tulip.py --pin <PIN> push build/studio.bin --launch
   ```
   You can also use the browser page at `http://tulip.local`.

   The launcher reads the new manifest. The app then has a tile on the start page, by
   its role, and a tile on the Studio home page. The `<` `>` arrows page through the
   apps four at a time.

`studio.bin` must fit a 1.5 MB app slot (`tools/make_full_image.sh` checks this). With
the five apps it is about 1.05 MB.

### Without ESP-IDF on your computer: the cloud build

`.github/workflows/build.yml` builds everything on GitHub on every push. To add an app:

1. Commit its folder in `modules/` and push.
2. Open the repository's **Actions** tab, then the latest **build** run.
3. Download the **tulip-studio** artifact. It holds:
   * `studio.bin`;
   * `tulip-full.bin` (for a first flash);
   * `example.elf` and `example.json` (section 7).
4. Push `studio.bin` to the Tulip as above.

A tag `v*` also makes a GitHub release with the same files. The same run builds the
host tests too.

### Built in, or loaded from a file

There are two ways to get an app onto the Tulip:

| | Built into `studio.bin` (sections 1-6) | Loaded from `/files/studio/apps` (section 7, an experiment) |
|---|---|---|
| Adding an app | rebuild `studio.bin` (locally or in the cloud), push it | build the app's `.elf`, copy it over; no new `studio.bin` |
| An app that crashes | a build you tested; a crash returns to the launcher | the same, and the Tulip leaves that app out next time |
| Speed | `render` can run from internal RAM (`(noflash)`) | everything runs from PSRAM through the cache, like the rest of the firmware's code; fine for light DSP, slower for heavy DSP |
| What it may call | anything | only what Tulip Studio exports (`tools/tulip_abi.py list`) |
| Tested on a Tulip | yes, every built-in app | **not yet** |

The Studio runs several apps at the same moment, sharing one clock, one audio mix and
one MIDI port. That only works inside one running firmware: the ESP32-S3 boots one
image, and it has no memory protection to keep apps apart. So a loaded app isn't a
separate program. Tulip Studio links it into itself as it starts, and from then on it
is an app like the built-in ones. That is also why a loaded app can crash the whole
Tulip, just as a built-in one can.

(Flash writes don't pause code in PSRAM on this firmware: `CONFIG_SPIRAM_XIP_FROM_PSRAM`
keeps the cache on while files are saved.)

## 3. Roles: what the runtime gives each app

| Role | The runtime gives it | The app gives |
|---|---|---|
| **SEQUENCER** | the clock's pulses and transport; **MIDI clock in** (follow a master) and **MIDI clock out**; MMC | **CV gate + pitch** (flag `TMOD_F_CV`, on the Mabee DACs); note tracks the Studio routes to a synth or **MIDI OUT** (`n_note_outs`, `studio_note_out()`) |
| **DRUMS** | the clock's timeline and transport; **MIDI clock in / out**; **MIDI IN** notes on its channel; the **CV clock** on the Mabee DAC at 0x59 (VOUT0 clock 1/16, 1/8 or 1/4; VOUT1 a reset each bar, or high while running) | audio (`TMOD_F_AUDIO`) |
| **SYNTH** | **MIDI IN** notes on its channel; notes routed from sequencer tracks (`TMOD_F_NOTES_IN`) | audio |

**Shared services.** The clock, MIDI clock in / out and the CV clock belong to the
Tulip, not to any one app:

* **Settings:** the same in every app. One app's settings page changes them for all.
* **Saving:** each standalone app keeps its own copy. The Studio keeps another.
* **The CV clock:** it runs while a drum machine is on, unless a sequencer that drives
  the DACs (`TMOD_F_CV`) is on too. The sequencer's gate and pitch win.

**Put the controls on your settings page with one call:**

```c
// clock INT / MIDI IN, MIDI clock out, and by role: the CV clock (drums),
// what MIDI IN plays (synths) - a 330 x 276 panel that keeps itself up to date
studio_io_panel(page, x, y, &mod_<name>, ACCENT);

// a drum machine with no room for the panel: the CV clock on one 430-wide row
studio_cv_strip(page, x, y, 40, ACCENT);
```

## 4. The `tmod_t`

Fill in only what you need (`components/tulip_studio/include/studio/studio.h`):

```c
tmod_t mod_example = {
    .name = "example", .title = "EXAMPLE", .blurb = "...",
    .role = TMOD_SYNTH, .flags = TMOD_F_AUDIO | TMOD_F_NOTES_IN,
    .init = init,            // start-up: load settings, allocate
    .rt_enable = enable,     // switched ON / OFF in the Studio
    .render = render,        // audio: ADD a block into l / r
    .note = note_in,         // a routed note: queue it
    .midi = midi_in,         // MIDI IN channel messages: queue them
    .midi_desc = midi_desc,  // what it takes from MIDI IN, for the home page
    .ui_create = ui_create,  // build your screen
    .ui_poll = ui_poll,      // every UI pass
    .busy = busy,            // a save still to finish before leaving
    .patch_save = patch_save, .patch_load = patch_load,   // Studio patches
};
```

| Hook | Runs in | When |
|---|---|---|
| `init` | on its own: boot, before any task; in the Studio: the UI task, the first time the app is switched ON | once |
| `rt_init`, `sink.*`, `rt_next_due`, `rt_advance`, `rt_cmd`, `rt_poll` | rt task, core 1, priority 23 | after `init`, while the app has started |
| `rt_enable(on)` | rt task | switched ON / OFF in the Studio (a sequencer drops its gates, a synth lets go of its notes) |
| `render` | audio task, core 1 | every 48 frames (1.09 ms) while ON |
| `note` | rt or MIDI task | a routed note, while ON |
| `midi` | MIDI task | MIDI IN channel messages, while ON |
| `ui_create`, `ui_poll`, `ui_show`, `busy`, `patch_*`, `midi_desc` | UI task, core 0 (LVGL) | after `init` |

**Apps that are off cost nothing:**

* until the first time it is switched ON, the app has no RAM, no screen and no task
  time;
* once switched OFF, it gets no `render`, `note` or `midi` calls.

## 5. Rules

These keep the audio and the timing solid with every app running:

1. **Never block in `rt_*`, `render`, `note` or `midi`.**
   * No waiting on a queue, mutex or file.
   * No `printf`, and no `malloc` in `render`.
   * Hand work over through a queue (`xQueueSend(.., 0)`), a single-word write, or a
     lock held for microseconds.
2. **Real-time code lives in internal RAM.**
   * Map `render` and what it calls `(noflash)` in your `linker.lf`.
   * Copy the tables it reads to RAM in `init`.
   * Don't call library code (`sinf`, `powf`, `printf`) from it: make tables in `init`.

   The rest of the firmware's code runs from PSRAM through a cache. A cache miss costs
   time at random moments. Code in internal RAM always runs at full speed, so a
   1.09 ms audio block never waits on the cache. (A loaded app can't do this: section
   7.)
3. **`init` is short and allocates once.**
   * In the Studio it runs while the other apps play.
   * Big buffers go in PSRAM (`heap_caps_malloc(.., MALLOC_CAP_SPIRAM)`).
   * Internal RAM is for DMA, stacks and the real-time code; Tulip Studio leaves about
     75 KB of it free.
4. **Keep your names to yourself.** Every app links into one image.
   * Make functions and globals `static`, or prefix them (`ex_...`).
   * Two `show_page()`s won't link.
5. **Files go in `/files/<name>/`** (`studio_files_root()`).
   * Write to a temporary name, then `rename()`.
   * Save from the UI task, a moment after the last change (the GinTonix, Sloe and
     Drumster9 pattern), and finish it in `busy()`.
6. **The DACs belong to the runtime and to sequencers with `TMOD_F_CV`.** A drum
   machine asks for the CV clock and does not drive the DAC itself.
7. **Look like the others.** Use the shared widgets (`studio/widgets.h`: `sw_btn`,
   `sw_label`, `sw_slider` ...). Put your title at the top left, `studio_chrome(scr)`
   for the two squares, and a rule at `STUDIO_HDR_LINE_Y`.
8. **Studio patches:** `patch_save` returns everything the app needs to come back as it
   was. `patch_load` takes it back, and keys that are missing keep their defaults. It
   is only called once the app has started.

## 6. Checklist

* [ ] `modules/mod_<name>/` with `app.json`, `CMakeLists.txt`, `mod_<name>.h`,
  `linker.lf`.
* [ ] The `tmod_t` and `app.json` agree (name, title, role, blurb).
* [ ] Nothing blocks in `render` / `note` / `midi` / `rt_*`; `render` is `(noflash)`.
* [ ] Settings page: `studio_io_panel()` (or `studio_cv_strip()` for a drum machine).
* [ ] MIDI IN by channel, with `midi_desc` saying what it listens to.
* [ ] `patch_save` / `patch_load`; files in `/files/<name>/`; `busy()` finishes a save.
* [ ] `cd apps/studio && idf.py build` with no warnings; the app on the launcher's start
  page and on the Studio home page.
* [ ] Optional: host tests for the DSP or the sequencer in `tests/`, and screens in
  `tools/ui_preview`.

## 7. Loading apps from files (an experiment)

An app can also be built on its own as an `.elf` file, and Tulip Studio loads it when
it starts. There is **no new `studio.bin`**: copy two files over and restart.

**This is a prototype, and it hasn't run on a Tulip yet.** Everything below builds, and
the example's `.elf` is checked against what the firmware exports, but no loaded app
has played a note on the hardware. Test it with nothing important at stake first.

### Building and installing one

The app is the same folder as in section 1. Its last line must be:

```c
TULIP_APP(mod_<name>)   // the entry point when built as an .elf; nothing otherwise
```

Then:

```sh
tools/build_elf_app.sh docs/templates/mod_example      # -> build/elf_apps/example.elf + .json
tools/tulip.py --pin <PIN> push build/elf_apps/example.elf build/elf_apps/example.json
```

* `tulip.py` puts an `.elf`, and the `.json` pushed with it, in `/files/studio/apps/`.
  From a browser, upload both into that folder.
* Then start Tulip Studio again (or tap any of its tiles). The app appears on the
  launcher's start page, marked LOADED, and on the Studio home page.
* The same folder works both ways: put it in `modules/` instead and it's built in.
* The cloud build makes `example.elf` too.

The `.elf` build takes seconds. It fails with a list if the app calls anything Tulip
Studio doesn't export. That list is every function in `studio/studio.h` and
`studio/widgets.h`, the clock's timeline functions, and
`components/tulip_studio/abi/extra.txt`: common C library, maths, FreeRTOS queues,
cJSON and a set of LVGL. `tools/tulip_abi.py list` prints it.

### How it works

* **Loading.** At start-up, `components/tulip_studio/src/loader.c` reads each
  `/files/studio/apps/<name>.elf`. Espressif's `elf_loader` (vendored in
  `components/elf_loader`) then:
  1. copies the file into PSRAM;
  2. links its calls to the firmware's exported functions (generated from that list
     at build time);
  3. calls the app's `app_main()`, which hands over its `tmod_t`.
* **Which apps load.** In the Studio, every one. On its own, only the app you tapped.
  For a built-in app, none.
* **The checks.** An app is refused, with a line in the log, if:
  * it calls something not exported;
  * it was built for another `TULIP_ABI_VERSION`;
  * its name isn't its file name;
  * a built-in app already has that name.
* **The crash guard.** Before a loaded app's code first runs (`app_main`, `init`, its
  screen), its name is written to NVS, and it is cleared afterwards. If the Tulip
  restarts with a name still there, that app crashed. Its file is renamed
  `<name>.elf.crashed` and Tulip Studio starts without it.
  * A crash later on (in `render`, say) isn't caught this way. It still returns to
    the launcher, as any crash does.
* **Memory.** The example is 6 KB on file and uses about 6 KB of PSRAM once loaded.
  Internal RAM isn't touched, so memory protection stays on.
* **Size.** Exporting the functions added about 48 KB to `studio.bin`.

### Limits of the prototype

* **Speed:** a loaded app's code all runs from PSRAM through the cache (`(noflash)`
  doesn't apply). Light DSP like the example's is fine. Something like Wavetron should
  stay built in.
* **One version:** an app runs only on the `TULIP_ABI_VERSION` it was built for. That
  number goes up only when an exported function changes. Adding exports keeps old
  apps working.
* **What isn't exported:** anything not on the list (the network, the I2C bus
  directly, most of LVGL) can't be used. Add to `abi/extra.txt` and rebuild
  `studio.bin` once.
* **Loading happens at start-up only.** Dropping in a new `.elf` needs a restart of
  Tulip Studio.

### To test on a Tulip

1. Flash or push this firmware, then push the example as above.
2. Tap **STUDIO**. EXAMPLE should be on the home page.
   * Switch it on, play notes on MIDI channel 1 (or route a metroseq track to it), and
     listen.
   * Watch the audio load and XRUN on the home page.
3. Tap the EXAMPLE tile on the start page: it runs on its own.
4. The serial log (`idf.py monitor` on the USB port) says what was loaded or why not:
   lines from `loader`.
