// preview_audio.h - the AUDIO PREVIEW from the Python sketch, without AMY.
//
// One saw voice per track through Tulip's I2S DAC so you can hear the
// sequence without a modular. Rendered by a low-priority task on core 0;
// note on/off from the sequencer are lock-free word writes, so audio can
// never hold up the sequencer (and an audio underrun is only a click).
#pragma once

#include <stdbool.h>

void preview_audio_start(void);
void preview_note_on(int track, int midi);
void preview_note_off(int track);
void preview_set_enabled(bool on);
bool preview_enabled(void);
