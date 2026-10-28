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
the five apps it is about 1.0 MB.

### Why apps are compiled in, not loaded as separate files

The Studio runs several apps at the same moment, sharing one clock, one audio mix and
one MIDI port. On the ESP32-S3 that only works inside **one firmware image**:

* The chip boots one image.
* It has no memory protection to keep separately built programs apart.

Loading app code from `/files` at run time (Espressif's `elf_loader`) is possible, but
it has costs:

* **Crashes:** an app that crashes takes everything down, with no way to contain it.
* **Interface:** every function an app calls has to be exported to the loader by hand.
* **Speed:** code loaded at run time runs from PSRAM. That is slower, and it stalls
  whenever flash is written, which hurts audio and real-time timing.

So "upload a new app" here means "upload a new `studio.bin`", which is one push. A
broken app then fails to build on your PC, not on stage.

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
   * Keep their data out of flash (`const` tables in flash stall during a write; copy
     them to RAM in `init`).
   * Don't call flash code (`sinf`, `powf`, `printf`) from them: make tables in `init`.

   A file save stalls flash for milliseconds, and anything running from flash stops
   with it.
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
