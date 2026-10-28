// main.c - Tulip Studio: metroseq (sequencer, CV), GinTonix and Drumster9
// (drums), Sloe and Wavetron (synths) all running at once on one master clock. Switch between
// them with the blue square; route metroseq's tracks to a synth (or MIDI OUT)
// on the Studio home page (red square).
//
// To build your own combination, list the modules you want here (and in
// main/CMakeLists.txt). Each module also builds on its own (apps/metroseq,
// apps/gintonix).
#include "mod_drumster9.h"
#include "mod_gintonix.h"
#include "mod_metroseq.h"
#include "mod_sloe.h"
#include "mod_wavetron.h"

void app_main(void) {
    static const tmod_t *const mods[] = {&mod_metroseq, &mod_gintonix, &mod_sloe, &mod_wavetron,
                                           // last: saved routes and open apps go by position
                                           &mod_drumster9};
    studio_run(mods, sizeof mods / sizeof mods[0]);
}
