// main.c - metroseq on its own: the Studio runtime with one app.
//
// The same module runs alongside other apps in apps/studio. See
// components/tulip_studio/include/studio/studio.h for the task layout and
// docs/INVESTIGATION.md for why it is arranged this way.
#include "mod_metroseq.h"

void app_main(void) {
    static const tmod_t *const mods[] = {&mod_metroseq};
    studio_run(mods, 1);
}
