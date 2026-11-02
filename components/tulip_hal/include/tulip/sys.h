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

// The Tulip Studio image runs either one app on its own or the Studio:
// the launcher leaves the choice here before it boots the image. "" (or
// "studio") = the Studio; else an app's name ("drumster9"). NVS must be up.
void tulip_boot_mode_set(const char *mode);
void tulip_boot_mode_get(char *out, int len);

// Apps: leave and go back to the launcher. Never returns.
void tulip_exit_to_launcher(void);

// True when this image is running from an app slot (i.e. under the launcher).
bool tulip_under_launcher(void);

// True on a real start (power on, reset button, after flashing); false
// when the board restarted itself: back from an app, a software restart,
// a crash. The launcher's power-on extras (calibration prompt, autostart)
// only happen on a real start.
bool tulip_cold_start(void);
