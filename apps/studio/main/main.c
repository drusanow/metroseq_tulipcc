// main.c - Tulip Studio: metroseq (sequencer, CV), GinTonix (drums) and
// Sloe (synth) all running at once on one master clock. Switch between
// them with the blue square; route metroseq's tracks to Sloe (or MIDI OUT)
// on the Studio home page (red square).
//
// To build your own combination, list the modules you want here (and in
// main/CMakeLists.txt). Each module also builds on its own (apps/metroseq,
// apps/gintonix).
#include "mod_gintonix.h"
#include "mod_metroseq.h"
#include "mod_sloe.h"

void app_main(void) {
    static const tmod_t *const mods[] = {&mod_metroseq, &mod_gintonix, &mod_sloe};
    studio_run(mods, sizeof mods / sizeof mods[0]);
}
