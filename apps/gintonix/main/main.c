// main.c - GinTonix on its own: the Studio runtime with one app. The same
// module runs alongside metroseq in apps/studio.
#include "mod_gintonix.h"

void app_main(void) {
    static const tmod_t *const mods[] = {&mod_gintonix};
    studio_run(mods, 1);
}
