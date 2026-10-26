// main.c - Wavetron on its own: the Studio runtime with one app, a dual
// wavetable synth played from MIDI IN. The same module runs alongside
// metroseq in apps/studio.
#include "mod_wavetron.h"

void app_main(void) {
    static const tmod_t *const mods[] = {&mod_wavetron};
    studio_run(mods, 1);
}
