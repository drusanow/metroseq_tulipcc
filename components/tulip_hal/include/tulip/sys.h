// sys.h - app lifecycle under the Tulip launcher.
//
// The launcher lives in the factory partition. To run an app it copies the
// app image into one of four OTA slots (or reuses the slot that already
// holds it) and reboots into it. Every app calls tulip_app_init() first
// thing: that points the next boot back at the launcher, so a reset, a
// crash or a power cycle always lands in the launcher. (If an app dies
// before even getting that far, the bootloader's rollback check sends it
// back to the launcher too.)
#pragma once

#include <stdbool.h>

// Apps: call at the top of app_main(). Harmless when run standalone.
void tulip_app_init(void);

// Apps: leave and go back to the launcher. Never returns.
void tulip_exit_to_launcher(void);

// True when this image is running from an app slot (i.e. under the launcher).
bool tulip_under_launcher(void);
