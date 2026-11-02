// preview_audio.h - the AUDIO PREVIEW from the Python sketch, without AMY.
//
// One saw voice per track, so you can hear the sequence without a
// modular. Rendered into the Studio mix by the audio task; note on/off
// from the sequencer are lock-free word writes, so audio can never hold up
// the sequencer. A track routed to a synth (Studio home page) plays there
// instead.
#pragma once

#include <stdbool.h>

#include "studio/studio.h"

void preview_init(void);
void preview_note_on(int track, int midi);
void preview_note_off(int track);
void preview_set_enabled(bool on);
bool preview_enabled(void);
// Studio render hook: adds the voices into l / r.
void preview_render(float *l, float *r, const studio_block_t *b);
