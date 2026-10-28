// main.c - Tulip Studio: every app in modules/ in one firmware (the list is
// generated from their app.json files, see main/CMakeLists.txt). The
// launcher starts it as the Studio - all the apps on one master clock, each
// switched on from the home page - or as one app on its own (the boot mode,
// tulip_boot_mode_set()).
#include <string.h>

#include "esp_log.h"
#include "studio/studio.h"
#include "tulip/app_manifest.h"

extern const tmod_t *const g_studio_apps[];
extern const int g_studio_napps;
extern const app_manifest_t g_studio_manifest;

void app_main(void) {
    // the manifest must say what the modules are (and referring to it here
    // keeps the linker from dropping it)
    for (int i = 0; i < g_studio_napps; i++) {
        const app_manifest_entry_t *e = &g_studio_manifest.app[i];
        if (strcmp(e->name, g_studio_apps[i]->name) || e->role != g_studio_apps[i]->role)
            ESP_LOGW("studio", "app.json of %s doesn't match its tmod_t (name / role)", g_studio_apps[i]->name);
    }
    studio_run(g_studio_apps, g_studio_napps);
}
