// gx_ui_int.h - GinTonix UI internals (the Tulip UI part of gintonix.py).
//
//   1024 x 600 touchscreen. Header: pages, kits, preset bank, status.
//   MAIN  : channel pads + mute / solo, the drum patch of the selected
//           channel, focus fader (last touched pot), PLAY, patterns, chain,
//           morph + ALTER A/B, master
//   SEQ   : all 8 channels x 16 steps, edit mode, PLAY, patterns, RATE,
//           chain, TEMPO / SWING / FILL rate with -/+
//   MIDI  : channel, note map, clock, CC map + learn, pattern MIDI out
//   TOOLS : randomize / alter / copy / paste / shift / reverse, morph tools
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/gx_json.h"
#include "gx_app.h"
#include "lvgl.h"
#include "studio/widgets.h"

// Studio apps are linked into one image: keep this UI's names to itself
// (another app has its own show_page(), refresh_all() ...).
#define A gxui_A
#define SEC_MIX gxui_SEC_MIX
#define SEC_NOISE gxui_SEC_NOISE
#define SEC_OSC gxui_SEC_OSC
#define SEC_VEL gxui_SEC_VEL
#define apply_tempo gxui_apply_tempo
#define audible gxui_audible
#define bar_new gxui_bar_new
#define bar_set_color gxui_bar_set_color
#define bar_show gxui_bar_show
#define ch_name gxui_ch_name
#define cur_tempo gxui_cur_tempo
#define del_user_drum gxui_del_user_drum
#define delete_slot gxui_delete_slot
#define dialog_active gxui_dialog_active
#define drum_count gxui_drum_count
#define drum_name gxui_drum_name
#define drum_patches gxui_drum_patches
#define edited gxui_edited
#define g_knobs gxui_g_knobs
#define g_nknobs gxui_g_nknobs
#define get_val gxui_get_val
#define header_create gxui_header_create
#define header_set_page gxui_header_set_page
#define header_slot_ui gxui_header_slot_ui
#define header_status gxui_header_status
#define header_tick gxui_header_tick
#define is_solo gxui_is_solo
#define key_default gxui_key_default
#define key_label gxui_key_label
#define key_name gxui_key_name
#define key_parse gxui_key_parse
#define kit_count gxui_kit_count
#define kit_name gxui_kit_name
#define knob_by_key gxui_knob_by_key
#define knob_new gxui_knob_new
#define knob_refresh gxui_knob_refresh
#define knob_retitle gxui_knob_retitle
#define knob_set_focus gxui_knob_set_focus
#define knob_set_learn gxui_knob_set_learn
#define knob_title gxui_knob_title
#define load_kit gxui_load_kit
#define load_slot gxui_load_slot
#define main_page_create gxui_main_page_create
#define main_page_drums_changed gxui_main_page_drums_changed
#define main_page_refresh gxui_main_page_refresh
#define main_page_tick gxui_main_page_tick
#define midi_page_create gxui_midi_page_create
#define midi_page_refresh gxui_midi_page_refresh
#define midi_page_tick gxui_midi_page_tick
#define nudge gxui_nudge
#define pattern_edited gxui_pattern_edited
#define pbar_new gxui_pbar_new
#define pbar_tick gxui_pbar_tick
#define pick_new gxui_pick_new
#define pick_set_options gxui_pick_set_options
#define pick_set_selected gxui_pick_set_selected
#define pick_set_text gxui_pick_set_text
#define picker_active gxui_picker_active
#define picker_open gxui_picker_open
#define refresh_all gxui_refresh_all
#define refresh_channel gxui_refresh_channel
#define save_kit_dialog gxui_save_kit_dialog
#define save_slot gxui_save_slot
#define save_user_drum gxui_save_user_drum
#define select_channel gxui_select_channel
#define select_pattern gxui_select_pattern
#define seq_page_create gxui_seq_page_create
#define seq_page_refresh gxui_seq_page_refresh
#define seq_page_tick gxui_seq_page_tick
#define set_focus gxui_set_focus
#define set_learn_target gxui_set_learn_target
#define set_val gxui_set_val
#define show_page gxui_show_page
#define slot_name gxui_slot_name
#define sound_edited gxui_sound_edited
#define say gxui_say
#define suggest_save_slot gxui_suggest_save_slot
#define text_dialog gxui_text_dialog
#define toggle_mute gxui_toggle_mute
#define tools_alter_ab gxui_tools_alter_ab
#define tools_page_create gxui_tools_page_create
#define transport_toggle gxui_transport_toggle
#define user_drum_index gxui_user_drum_index
#define val_text gxui_val_text

// --- palette (the Python's: colours that survive the panel's RGB332) -------------------
#define C_BG       0x000000
#define C_LINE     0x494955
#define C_BTN_EDGE 0x6d6d55
#define C_TRACK    0x494955
#define C_TEXT     0xffffff
#define C_DIM      0x9292aa
#define ACC        0xff9200   // drum patch accent (amber)
#define ACC_DIM    0x492400
#define SEQ_ACC    0x24b6ff   // pattern / transport (cyan)
#define SEQ_DIM    0x004955
#define TRIG_C     0x24b6ff
#define ACC_C      0xffdb00
#define FILL_C     0xff6dff
#define STEP_OFF   0x242424
#define STEP_OUT   0x000024
#define STEP_DIM   0x244955
#define LED_ON     0xff2400
#define LED_OFF    0x490000
#define PLAY_C     0x6dff00
#define MUTE_C     0xff2400
#define LEARN_C    0x6dff00
#define CHAIN_C    0x6db6ff
#define SOLO_C     0x6dff00
#define CAP_C      0x242400
#define CAP_EDGE   0x6d6d55

#define F_TINY  (&lv_font_montserrat_12)
#define F_SMALL (&lv_font_montserrat_12)
#define F_LABEL (&lv_font_montserrat_12)
#define F_MID   (&lv_font_montserrat_16)
#define F_BIG   (&lv_font_montserrat_20)

#define PAGE_Y   44
#define PAGE_H   (600 - PAGE_Y)
#define CELL_H   118
#define KNOB     64
#define KN_W     76
#define SW_W     70
#define KNOB_Y   18
#define DRAG_PX  260.0f
#define CAP      42
#define KIT_NAME GX_KIT_NAME_MAX   // kit / sound names typed in

typedef struct {
    uint32_t col, tint, dim;
} gx_sec_t;

extern const gx_sec_t SEC_OSC, SEC_NOISE, SEC_MIX, SEC_VEL;
const gx_sec_t *gx_sec_of(int knob);       // by knob index
const gx_sec_t *gx_sec_of_sw(int sw);

// --- parameter keys ----------------------------------------------------------------------
// A key names a value a pot, the focus fader or a MIDI CC can move:
//   KEY_SEL(k)    drum knob k of the selected channel   ("k:osc_freq")
//   KEY_CH(c, k)  drum knob k of channel c              ("c3:osc_freq")
//   KEY_G(g)      a global                              ("g:tempo")
enum { G_TEMPO = 0, G_SWING, G_FILL, G_MASTER, G_MORPH, G_COUNT };
#define KEY_NONE   (-1)
#define KEY_SEL(k) (0x100 | (k))
#define KEY_CH(c, k) (0x200 | ((c) << 5) | (k))
#define KEY_G(g)   (0x400 | (g))
#define KEY_IS_SEL(x) (((x) & 0x700) == 0x100)
#define KEY_IS_CH(x)  (((x) & 0x700) == 0x200)
#define KEY_IS_G(x)   (((x) & 0x700) == 0x400)

int key_parse(const char *s);                // "k:osc_freq" -> key (KEY_NONE if bad)
void key_name(int key, char *out, int len);  // key -> "k:osc_freq"
float get_val(int key);
void set_val(int key, float v);
void val_text(int key, char *out, int len);
const char *knob_title(int key);
void key_label(int key, char *out, int len);
float key_default(int key);
void nudge(int key, int d);

// --- widgets ---------------------------------------------------------------------------------

typedef struct gx_knob {
    int key;
    uint32_t accent;
    lv_obj_t *cell, *title, *arc, *ptr, *val;
    int shown;
    char shown_txt[20];
    int32_t y0;
    float v0;
    bool dragging, learn_mark, focus_mark, big;
} gx_knob_t;

gx_knob_t *knob_new(lv_obj_t *parent, int key, int x, int y, uint32_t accent, int w, int bipolar,
                    int top, bool big);
void knob_refresh(gx_knob_t *k);
void knob_set_learn(gx_knob_t *k, bool on);
void knob_set_focus(gx_knob_t *k, bool on);
void knob_retitle(gx_knob_t *k);

typedef void (*bar_cb_t)(float v);

typedef struct {
    lv_obj_t *box, *fill, *knob;
    int w, thumb;
    float v, v0;
    int32_t x0;
    bool dragging;
    bar_cb_t cb;
    float (*get)(void);
    int key;   // focus / learn target when touched (KEY_NONE: none)
} gx_bar_t;

gx_bar_t *bar_new(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, uint32_t dim,
                  bar_cb_t cb, int thumb);
void bar_show(gx_bar_t *b, float v);
void bar_set_color(gx_bar_t *b, uint32_t color, uint32_t dim);

// A button that opens a full-screen list.
typedef void (*pick_cb_t)(int i);
typedef struct {
    lv_obj_t *btn;
    char **opts;
    int n, sel;
    const char *fixed;
    const char *(*title)(void);
    char title_txt[48];
    uint32_t accent;
    pick_cb_t cb;
} gx_pick_t;

gx_pick_t *pick_new(lv_obj_t *parent, int x, int y, int w, int h, uint32_t accent,
                    const char *title, pick_cb_t cb);
void pick_set_options(gx_pick_t *p, const char *const *opts, int n);
void pick_set_selected(gx_pick_t *p, int i);
void pick_set_text(gx_pick_t *p, const char *fixed);   // NULL: show the selection
void picker_open(const char *title, const char *const *opts, int n, int sel, uint32_t accent,
                 pick_cb_t cb);
bool picker_active(void);

// Full-screen name entry with a keyboard. label(name) gives the OK button's
// text (e.g. OVERWRITE); NULL = "SAVE".
typedef void (*text_cb_t)(const char *name);
void text_dialog(const char *title, const char *text, text_cb_t ok,
                 const char *(*label)(const char *name));
// The SAVE KIT dialog: slot chooser + name.
void save_kit_dialog(void);
bool dialog_active(void);

// --- the app -----------------------------------------------------------------------------------

typedef struct {
    int page;
    int sel;               // selected channel
    int focus_key;
    bool learn_on;
    int learn_target;
    bool dirty;            // kit edited since load / save
    char patch_name[48];
    int source_slot;       // slot the kit came from, 0 = none
    uint32_t flash_ms[GX_NCH];
    // refresh flags (set anywhere in the UI task, handled by the tick)
    bool knobs_dirty, strip_dirty, grid_dirty, midi_dirty, status_dirty, sound_dirty;
    uint8_t sound_dirty_ch;   // bit mask
    lv_obj_t *scr, *pages[4];
    // user drum sounds
    gx_user_drum_t *user_drums;
    int n_user;
    // bank slot names (NULL = empty), read lazily
    char *slot_names[GX_BANK_SLOTS];
    bool slots_read;
} gx_ui_t;

extern gx_ui_t A;

// all pots, for refresh / learn / focus marks
#define MAX_KNOBS 32
extern gx_knob_t *g_knobs[MAX_KNOBS];
extern int g_nknobs;
gx_knob_t *knob_by_key(int key);

void edited(void);                      // the kit changed
void sound_edited(int c);               // a sound changed: c, or -1 all
void pattern_edited(void);
void set_focus(int key);
void set_learn_target(int key);
void select_channel(int c, bool play);
void toggle_mute(int c, bool solo);
bool is_solo(int c);
void transport_toggle(void);
bool audible(void);                     // the patterns really play
void select_pattern(int i);
void apply_tempo(float bpm);
float cur_tempo(void);
void say(const char *msg);           // header status line (NULL: kit name)
void refresh_all(void);
void refresh_channel(void);
void show_page(int i);
const char *ch_name(int c, char *buf, int len);

// kits / bank / sounds
int kit_count(void);                    // factory kits + kits/*.json
const char *kit_name(int i);
void load_kit(int i);
bool load_slot(int slot);
void save_slot(int slot, const char *name);
int suggest_save_slot(void);
const char *slot_name(int slot);        // NULL = empty
void delete_slot(int slot);
int drum_count(void);                   // saved sounds first, then factory
const char *drum_name(int i);
void drum_patches(int i, gx_patch_t *a, gx_patch_t *b);
int user_drum_index(const char *name);
void save_user_drum(const char *name, const gx_patch_t *a, const gx_patch_t *b);
void del_user_drum(int i);

// pages
void main_page_create(lv_obj_t *g);
void main_page_refresh(bool knobs);
void main_page_tick(uint32_t now);
void main_page_drums_changed(void);
void seq_page_create(lv_obj_t *g);
void seq_page_refresh(void);
void seq_page_tick(uint32_t now);
void midi_page_create(lv_obj_t *g);
void midi_page_refresh(void);
void midi_page_tick(uint32_t now);
void tools_page_create(lv_obj_t *g);
void tools_alter_ab(bool b);
void header_create(lv_obj_t *g);
void header_tick(uint32_t now);
void header_slot_ui(void);
void header_set_page(int i);
void header_status(const char *msg);

// the pattern bar (MAIN + SEQ)
typedef struct gx_pbar gx_pbar_t;
gx_pbar_t *pbar_new(lv_obj_t *g, int x, int y, int cols, int bw, int bh, int gap, int cx, int cy,
                    int cw, int ch, int px, int py, int pw, int ph);
void pbar_tick(gx_pbar_t *p);
