// main.c - Sloe on its own: the Studio runtime with one app, a synth played
// from MIDI IN. The same module runs alongside metroseq in apps/studio.
#include "mod_sloe.h"

void app_main(void) {
    static const tmod_t *const mods[] = {&mod_sloe};
    studio_run(mods, 1);
}
