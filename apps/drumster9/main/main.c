// main.c - Drumster9 on its own: the Studio runtime with one app, the
// 32-step sample drum machine, with a CV clock on the Mabee DAC. The same
// module runs alongside the others in apps/studio.
#include "mod_drumster9.h"

void app_main(void) {
    static const tmod_t *const mods[] = {&mod_drumster9};
    studio_run(mods, 1);
}
