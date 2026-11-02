// gx_ui.c - GinTonix UI: parameters, kits / bank / sounds, transport,
// MIDI CC and the header. (The Tulip UI part of gintonix.py.)
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "core/gx_json.h"
#include "gx_ui.h"
#include "gx_ui_int.h"

gx_ui_t A = {.focus_key = KEY_NONE, .learn_target = KEY_NONE};
gx_knob_t *g_knobs[MAX_KNOBS];
int g_nknobs;

const gx_sec_t SEC_OSC = {ACC, 0x242400, ACC_DIM};
const gx_sec_t SEC_NOISE = {0x24dbaa, 0x002400, 0x004955};
const gx_sec_t SEC_MIX = {0xdbdbff, 0x240000, 0x494955};
const gx_sec_t SEC_VEL = {0x92b6ff, 0x242455, 0x244955};

const gx_sec_t *gx_sec_of(int k) {
    switch (k) {
    case K_OSC_FREQ: case K_MOD_AMT: case K_MOD_RATE: case K_OSC_ATT: case K_OSC_DEC:
        return &SEC_OSC;
    case K_N_FREQ: case K_N_Q: case K_N_ATT: case K_N_DEC:
        return &SEC_NOISE;
    case K_VEL_OSC: case K_VEL_NOISE: case K_VEL_MOD:
        return &SEC_VEL;
    default:
        return &SEC_MIX;
    }
}

const gx_sec_t *gx_sec_of_sw(int s) {
    if (s == S_WAVE || s == S_MOD_MODE) return &SEC_OSC;
    if (s == S_N_MODE || s == S_N_ENV) return &SEC_NOISE;
    return &SEC_MIX;
}

static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// --- parameter keys ---------------------------------------------------------------------------

static const char *const G_KEYS[G_COUNT] = {"tempo", "swing", "fill", "master", "morph"};
static const char *const G_LABEL[G_COUNT] = {"TEMPO", "SWING", "FILL RATE", "MASTER", "MORPH"};
static const char *const G_TITLE[G_COUNT] = {"TEMPO", "SWING", "FILL", "MASTER", "MORPH"};

int key_parse(const char *s) {
    if (!strncmp(s, "k:", 2)) {
        int k = gx_knob_index(s + 2);
        return k < 0 ? KEY_NONE : KEY_SEL(k);
    }
    if (!strncmp(s, "g:", 2)) {
        for (int g = 0; g < G_COUNT; g++)
            if (!strcmp(s + 2, G_KEYS[g])) return KEY_G(g);
        return KEY_NONE;
    }
    if (s[0] == 'c' && s[1] >= '0' && s[1] <= '7' && s[2] == ':') {
        int k = gx_knob_index(s + 3);
        return k < 0 ? KEY_NONE : KEY_CH(s[1] - '0', k);
    }
    return KEY_NONE;
}

void key_name(int key, char *out, int len) {
    if (KEY_IS_SEL(key))
        snprintf(out, len, "k:%s", GX_KNOBS[key & 31].key);
    else if (KEY_IS_CH(key))
        snprintf(out, len, "c%d:%s", (key >> 5) & 7, GX_KNOBS[key & 31].key);
    else if (KEY_IS_G(key))
        snprintf(out, len, "g:%s", G_KEYS[key & 7]);
    else
        snprintf(out, len, "?");
}

static int key_ch(int key) { return KEY_IS_SEL(key) ? A.sel : (key >> 5) & 7; }

static float tempo_k(float bpm) { return clamp01(logf(bpm / 20.0f) / logf(15.0f)); }
static float k_tempo(float k) { return 20.0f * powf(15.0f, clamp01(k)); }

static bool ext_clock(void) { return studio_clock_src() == CLOCK_SRC_MIDI; }

float cur_tempo(void) {
    if (ext_clock()) {
        studio_clock_status_t st;
        studio_clock_status(&st);
        return st.ext_bpm_x10 ? st.ext_bpm_x10 / 10.0f : g_gx.tempo;
    }
    return studio_bpm_x100() / 100.0f;
}

void apply_tempo(float bpm) {
    if (ext_clock()) return;   // the master sets the tempo
    if (bpm < 20) bpm = 20;
    if (bpm > 300) bpm = 300;
    g_gx.tempo = bpm;
    studio_set_bpm_x100((int32_t)(bpm * 100 + 0.5f));
}

float get_val(int key) {
    if (KEY_IS_SEL(key) || KEY_IS_CH(key)) {
        gx_patch_t p;
        gx_eff(&g_gx, key_ch(key), &p);
        return p.k[key & 31];
    }
    switch (key & 7) {
    case G_TEMPO: return tempo_k(cur_tempo());
    case G_SWING: return g_gx.swing;
    case G_FILL: return g_gx.fill_rate / (float)(GX_NFILLS - 1);
    case G_MASTER: return GX_LEVEL_0DB * sqrtf(g_gx.master);
    case G_MORPH: return g_gx.morph;
    }
    return 0;
}

void set_val(int key, float v) {
    v = clamp01(v);
    if (KEY_IS_SEL(key) || KEY_IS_CH(key)) {
        int c = key_ch(key);
        gx_set_knob(&g_gx, c, key & 31, v);
        sound_edited(c);
        return;
    }
    switch (key & 7) {
    case G_TEMPO:
        apply_tempo(k_tempo(v));
        break;
    case G_SWING:
        g_gx.swing = v;
        break;
    case G_FILL:
        g_gx.fill_rate = (uint8_t)(v * (GX_NFILLS - 1) + 0.5f);
        break;
    case G_MASTER:
        g_gx.master = (v / GX_LEVEL_0DB) * (v / GX_LEVEL_0DB);
        sound_edited(-1);
        break;
    case G_MORPH:
        g_gx.morph = v;
        sound_edited(-1);
        A.knobs_dirty = true;
        break;
    }
    edited();
}

void val_text(int key, char *o, int n) {
    if (KEY_IS_SEL(key) || KEY_IS_CH(key)) {
        gx_patch_t p;
        gx_eff(&g_gx, key_ch(key), &p);
        gx_fmt(key & 31, &p, o, n);
        return;
    }
    switch (key & 7) {
    case G_TEMPO:
        snprintf(o, n, ext_clock() ? "EXT %d" : "%d BPM", (int)(cur_tempo() + 0.5f));
        return;
    case G_SWING: snprintf(o, n, "%d%%", (int)(g_gx.swing * 100 + 0.5f)); return;
    case G_FILL: snprintf(o, n, "%dx", GX_FILL_RATES[g_gx.fill_rate % GX_NFILLS]); return;
    case G_MASTER:
        if (g_gx.master < 0.0001f)
            snprintf(o, n, "-inf");
        else
            snprintf(o, n, "%+.1fdB", 20.0f * log10f(g_gx.master));
        return;
    case G_MORPH: snprintf(o, n, "A %d B", (int)(g_gx.morph * 100 + 0.5f)); return;
    }
    o[0] = 0;
}

const char *knob_title(int key) {
    if (KEY_IS_G(key)) return G_TITLE[key & 7];
    return GX_KNOBS[key & 31].title;
}

void key_label(int key, char *o, int n) {
    if (KEY_IS_G(key))
        snprintf(o, n, "%s", G_LABEL[key & 7]);
    else if (KEY_IS_CH(key))
        snprintf(o, n, "%d %s", ((key >> 5) & 7) + 1, GX_KNOBS[key & 31].label);
    else
        snprintf(o, n, "%s", GX_KNOBS[key & 31].label);
}

float key_default(int key) {
    if (!KEY_IS_G(key)) return GX_KNOBS[key & 31].def;
    if ((key & 7) == G_TEMPO) return tempo_k(120.0f);
    if ((key & 7) == G_MASTER) return GX_LEVEL_0DB;
    return 0;
}

// focus fader -/+ : one fine step (tempo: 1 BPM, fill rate: one rate)
void nudge(int key, int d) {
    if (KEY_IS_G(key) && (key & 7) == G_TEMPO) {
        if (!ext_clock()) {
            apply_tempo((float)((int)(cur_tempo() + 0.5f) + d));
            edited();
        }
    } else {
        float step = 0.002f;
        if (KEY_IS_G(key) && (key & 7) == G_FILL) step = 1.0f / (GX_NFILLS - 1);
        if (KEY_IS_G(key) && (key & 7) == G_SWING) step = 0.01f;
        float v = get_val(key) + d * step;
        if (KEY_IS_G(key) && (key & 7) == G_SWING) v = (int)(v * 100 + 0.5f) / 100.0f;
        set_val(key, v);
    }
    gx_knob_t *k = knob_by_key(key);
    if (k) knob_refresh(k);
}

gx_knob_t *knob_by_key(int key) {
    for (int i = 0; i < g_nknobs; i++)
        if (g_knobs[i]->key == key) return g_knobs[i];
    return NULL;
}

void set_focus(int key) {
    int old = A.focus_key;
    if (old == key) return;
    A.focus_key = key;
    for (int i = 0; i < g_nknobs; i++) knob_set_focus(g_knobs[i], g_knobs[i]->key == key);
}

// what a learned knob maps to: the selected channel ("k:"), or this one
static int cc_key(int key) {
    if (KEY_IS_SEL(key) && !g_gxset.cc_sel) return KEY_CH(A.sel, key & 31);
    return key;
}

void set_learn_target(int key) {
    A.learn_target = key;
    for (int i = 0; i < g_nknobs; i++) knob_set_learn(g_knobs[i], g_knobs[i]->key == key);
    A.midi_dirty = true;
}

// --- edits ----------------------------------------------------------------------------------------

void edited(void) {
    if (!A.dirty) {
        A.dirty = true;
        A.status_dirty = true;
    }
}

void sound_edited(int c) {
    A.sound_dirty = true;
    A.sound_dirty_ch = c < 0 ? 0xFF : (uint8_t)(A.sound_dirty_ch | (1u << c));
    edited();
}

void pattern_edited(void) {
    A.grid_dirty = true;
    edited();
}

const char *ch_name(int c, char *buf, int len) {
    snprintf(buf, len, "%d %.9s", c + 1, g_gx.names[c]);
    return buf;
}

// Microtonic: tap a channel to select it, tap the selected one to hear it
void select_channel(int c, bool play) {
    if (c == A.sel) {
        if (play) {
            gx_trigger(c, 0.5f);
            A.flash_ms[c] = (uint32_t)(studio_now_us() / 1000);
        }
        return;
    }
    A.sel = c;
    refresh_channel();
}

bool is_solo(int c) {
    uint8_t m = g_live.mute;
    return !(m & (1u << c)) && (m | (1u << c)) == 0xFF;
}

void toggle_mute(int c, bool solo) {
    if (solo)
        g_live.mute = is_solo(c) ? 0 : (uint8_t)(0xFF & ~(1u << c));
    else
        g_live.mute ^= (uint8_t)(1u << c);
    A.strip_dirty = true;
}

bool audible(void) { return g_gxs.running; }

void transport_toggle(void) {
    studio_toggle_play();
    studio_clock_status_t st;
    studio_clock_status(&st);
    if (st.ext && !st.playing && !st.armed) say("EXT: armed, waits for MIDI Start");
}

// pattern buttons: edit pattern i; while playing it starts at the end of the pass
void select_pattern(int i) {
    g_gx.cur = (uint8_t)i;
    if (g_gxs.running)
        g_live.next = (int8_t)(i != g_gxs.pat ? i : -1);
    A.grid_dirty = true;
    edited();
}

void say(const char *msg) { header_status(msg); }

// --- kits ---------------------------------------------------------------------------------------

#define MAX_KIT_FILES 96
static char *s_kit_files[MAX_KIT_FILES];
static int s_nkit_files = -1;
static char s_kit_dir[64];

static int cmp_str(const void *a, const void *b) {
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static void scan_kits(void) {
    if (s_nkit_files >= 0) return;
    s_nkit_files = 0;
    const char *root = studio_files_root();
    if (!root) return;
    // gintonix/kits, or the GinTonic app's kits folder next to it
    const char *dirs[2] = {"gintonix/kits", "gintonic/kits"};
    for (int d = 0; d < 2 && !s_nkit_files; d++) {
        snprintf(s_kit_dir, sizeof s_kit_dir, "%s/%s", root, dirs[d]);
        DIR *dp = opendir(s_kit_dir);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp)) && s_nkit_files < MAX_KIT_FILES) {
            size_t n = strlen(e->d_name);
            if (n > 5 && !strcasecmp(e->d_name + n - 5, ".json")) {
                s_kit_files[s_nkit_files] = strndup(e->d_name, n - 5);
                s_nkit_files++;
            }
        }
        closedir(dp);
        qsort(s_kit_files, s_nkit_files, sizeof(char *), cmp_str);
    }
}

int kit_count(void) {
    scan_kits();
    return GX_NKITS + s_nkit_files;
}

const char *kit_name(int i) {
    scan_kits();
    return i < GX_NKITS ? GX_KITS[i].name : s_kit_files[i - GX_NKITS];
}

static gx_state_t s_tmp;

// Put a whole new kit in place: sounds, patterns (and tempo, on the
// internal clock).
static void apply_state(const gx_state_t *s, const char *name, int slot) {
    gx_silence();   // drop the old kit's tails
    gx_lock();
    float t = g_gx.tempo;
    g_gx = *s;
    if (ext_clock()) g_gx.tempo = t;
    gx_unlock();
    if (!ext_clock()) apply_tempo(s->tempo);
    // while playing, the new kit's pattern starts at the end of the pass
    if (g_gxs.running) g_live.next = (int8_t)(g_gx.cur != g_gxs.pat ? g_gx.cur : -1);
    gx_sound_changed(-1);
    snprintf(A.patch_name, sizeof A.patch_name, "%s", name);
    A.source_slot = slot;
    A.dirty = false;
    if (A.scr) refresh_all();
    A.status_dirty = true;
}

// --- Studio patches: the kit with its patterns, as the bank saves it --------------------------

cJSON *gx_patch_save(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", A.patch_name);
    cJSON_AddNumberToObject(o, "slot", A.source_slot);
    cJSON_AddNumberToObject(o, "midi_ch", g_gxset.midi_ch);
    g_gx.tempo = studio_bpm_x100() / 100.0f;
    cJSON_AddItemToObject(o, "state", gx_json_state(&g_gx));
    return o;
}

void gx_patch_load(const cJSON *o) {
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(o, "state");
    if (!cJSON_IsObject(st)) return;
    s_tmp.tempo = studio_bpm_x100() / 100.0f;
    gx_json_to_state(st, &s_tmp, true);   // the tempo is the Studio patch's
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "midi_ch");
    if (cJSON_IsNumber(v) && v->valueint >= -1 && v->valueint <= 16) {
        g_gxset.midi_ch = (int8_t)v->valueint;
        gx_settings_save();
    }
    v = cJSON_GetObjectItemCaseSensitive(o, "name");
    const cJSON *sl = cJSON_GetObjectItemCaseSensitive(o, "slot");
    int slot = cJSON_IsNumber(sl) && sl->valueint >= 0 && sl->valueint <= GX_BANK_SLOTS ? sl->valueint : 0;
    apply_state(&s_tmp, cJSON_IsString(v) ? v->valuestring : "", slot);
    if (A.scr) header_slot_ui();
}

void load_kit(int i) {
    if (i < 0 || i >= kit_count()) return;
    if (i < GX_NKITS) {
        gx_kit_state(&GX_KITS[i], &s_tmp);
    } else {
        char path[160];
        snprintf(path, sizeof path, "%s/%s.json", s_kit_dir, s_kit_files[i - GX_NKITS]);
        s_tmp.tempo = g_gx.tempo;
        if (!gx_load_kit_file(path, &s_tmp, ext_clock())) {
            say("kit load error");
            return;
        }
    }
    apply_state(&s_tmp, kit_name(i), 0);
}

// --- the preset bank: 128 slots, one JSON file each ------------------------------------------

static void slot_path(int slot, char *out, int len) {
    char nm[32];
    snprintf(nm, sizeof nm, "gintonic_bank/%03d.json", slot);
    gx_path(out, len, nm);
}

// the name only: our files (and the Python's) start with {"name": ...}
static bool quick_name(const char *path, char *out, int len) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char buf[200];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = strstr(buf, "\"name\"");
    if (p && (p = strchr(p + 6, '"'))) {
        char *e = strchr(p + 1, '"');
        if (e && !memchr(p + 1, '\\', e - p - 1)) {
            int l = (int)(e - p - 1);
            snprintf(out, len, "%.*s", l, p + 1);
            return true;
        }
    }
    return gx_bank_read_name(path, out, len);
}

static void read_slot_names(void) {
    if (A.slots_read) return;
    A.slots_read = true;
    for (int s = 1; s <= GX_BANK_SLOTS; s++) {
        char path[96], nm[48];
        slot_path(s, path, sizeof path);
        if (access(path, F_OK) == 0 && quick_name(path, nm, sizeof nm)) {
            if (!nm[0]) snprintf(nm, sizeof nm, "KIT %03d", s);
            A.slot_names[s - 1] = strdup(nm);
        }
    }
}

const char *slot_name(int slot) {
    if (slot < 1 || slot > GX_BANK_SLOTS) return NULL;
    read_slot_names();
    return A.slot_names[slot - 1];
}

bool load_slot(int slot) {
    if (slot < 1) slot = 1;
    if (slot > GX_BANK_SLOTS) slot = GX_BANK_SLOTS;
    g_gxset.slot = (uint8_t)slot;
    gx_settings_save();
    char path[96], nm[48];
    slot_path(slot, path, sizeof path);
    s_tmp.tempo = g_gx.tempo;
    if (!gx_bank_load(path, &s_tmp, nm, sizeof nm, ext_clock())) {
        header_slot_ui();
        char t[24];
        snprintf(t, sizeof t, "%03d empty", slot);
        say(t);
        return false;
    }
    if (!nm[0]) snprintf(nm, sizeof nm, "KIT %03d", slot);
    apply_state(&s_tmp, nm, slot);
    header_slot_ui();
    return true;
}

void save_slot(int slot, const char *name) {
    char nm[48];
    if (!name || !name[0])
        snprintf(nm, sizeof nm, "KIT %03d", slot);
    else
        snprintf(nm, sizeof nm, "%s", name);
    char path[96];
    slot_path(slot, path, sizeof path);
    g_gx.tempo = ext_clock() ? g_gx.tempo : studio_bpm_x100() / 100.0f;
    if (!gx_dir() || !gx_bank_save(path, nm, &g_gx)) {
        say("save error");
        return;
    }
    read_slot_names();
    free(A.slot_names[slot - 1]);
    A.slot_names[slot - 1] = strdup(nm);
    g_gxset.slot = (uint8_t)slot;
    gx_settings_save();
    A.source_slot = slot;
    snprintf(A.patch_name, sizeof A.patch_name, "%s", nm);
    A.dirty = false;
    header_slot_ui();
}

void delete_slot(int slot) {
    char path[96];
    slot_path(slot, path, sizeof path);
    remove(path);
    read_slot_names();
    free(A.slot_names[slot - 1]);
    A.slot_names[slot - 1] = NULL;
    if (A.source_slot == slot) A.source_slot = 0;
    header_slot_ui();
}

int suggest_save_slot(void) {
    if (A.source_slot) return A.source_slot;
    for (int i = 0; i < GX_BANK_SLOTS; i++) {
        int s = (g_gxset.slot - 1 + i) % GX_BANK_SLOTS + 1;
        if (!slot_name(s)) return s;
    }
    return g_gxset.slot;
}

// --- saved drum sounds (the DRUM list) ------------------------------------------------------------

#define MAX_USER_DRUMS 128
#define DRUMS_FILE "gintonix_drums.json"

static void load_user_drums(void) {
    if (!A.user_drums) A.user_drums = calloc(MAX_USER_DRUMS, sizeof(gx_user_drum_t));
    char path[96];
    gx_path(path, sizeof path, DRUMS_FILE);
    A.n_user = gx_dir() ? gx_drums_load(path, A.user_drums, MAX_USER_DRUMS) : 0;
}

static void write_user_drums(void) {
    char path[96];
    gx_path(path, sizeof path, DRUMS_FILE);
    if (gx_dir()) gx_drums_save(path, A.user_drums, A.n_user);
}

int drum_count(void) { return A.n_user + GX_NDRUMS; }

const char *drum_name(int i) {
    return i < A.n_user ? A.user_drums[i].name : GX_DRUMS[i - A.n_user].name;
}

void drum_patches(int i, gx_patch_t *a, gx_patch_t *b) {
    if (i < A.n_user) {
        *a = A.user_drums[i].a;
        *b = A.user_drums[i].b;
    } else {
        *a = *b = GX_DRUMS[i - A.n_user].p;
    }
}

int user_drum_index(const char *name) {
    for (int i = 0; i < A.n_user; i++)
        if (!strcmp(A.user_drums[i].name, name)) return i;
    return -1;
}

void save_user_drum(const char *name, const gx_patch_t *a, const gx_patch_t *b) {
    int i = user_drum_index(name);
    if (i < 0) {
        if (A.n_user >= MAX_USER_DRUMS) return;
        // sorted by name, case-insensitive
        i = 0;
        while (i < A.n_user && strcasecmp(A.user_drums[i].name, name) < 0) i++;
        memmove(&A.user_drums[i + 1], &A.user_drums[i], (A.n_user - i) * sizeof(gx_user_drum_t));
        A.n_user++;
    }
    snprintf(A.user_drums[i].name, sizeof A.user_drums[i].name, "%s", name);
    A.user_drums[i].a = *a;
    A.user_drums[i].b = *b;
    write_user_drums();
}

void del_user_drum(int i) {
    if (i < 0 || i >= A.n_user) return;
    memmove(&A.user_drums[i], &A.user_drums[i + 1], (A.n_user - i - 1) * sizeof(gx_user_drum_t));
    A.n_user--;
    write_user_drums();
}

// --- boot: the last kit slot ----------------------------------------------------------------------

void gx_boot_load(void) {
    char path[96], nm[48];
    slot_path(g_gxset.slot, path, sizeof path);
    s_tmp.tempo = g_gx.tempo;
    snprintf(A.patch_name, sizeof A.patch_name, "%s", GX_KITS[0].name);
    if (gx_dir() && gx_bank_load(path, &s_tmp, nm, sizeof nm, true)) {
        g_gx = s_tmp;
        snprintf(A.patch_name, sizeof A.patch_name, "%s", nm[0] ? nm : "KIT");
        A.source_slot = g_gxset.slot;
    }
}

// --- MIDI CC / program change / pattern keys --------------------------------------------------

static void cc_assign(int cc, int key) {
    char kn[20];
    key_name(key, kn, sizeof kn);
    // one CC per target: drop the target's old CC and the CC's old target
    int w = 0;
    for (int i = 0; i < g_gxset.ncc; i++)
        if (g_gxset.cc[i].cc != cc && strcmp(g_gxset.cc[i].key, kn)) g_gxset.cc[w++] = g_gxset.cc[i];
    g_gxset.ncc = w;
    if (w < GX_MAX_CC) {
        g_gxset.cc[w].cc = (uint8_t)cc;
        snprintf(g_gxset.cc[w].key, sizeof g_gxset.cc[w].key, "%s", kn);
        g_gxset.ncc++;
    }
    gx_settings_save();
}

static void handle_midi(const gx_midi_msg_t *m) {
    uint8_t st = m->status & 0xF0;
    if (st == 0x90 && m->d2 > 0) {
        if (m->d1 >= 60 && m->d1 < 60 + GX_NPAT) select_pattern(m->d1 - 60);
        return;
    }
    if (st == 0xC0) {
        if (!picker_active() && !dialog_active()) load_slot(m->d1 + 1);
        return;
    }
    if (st != 0xB0) return;
    int cc = m->d1;
    if (A.learn_on && A.learn_target != KEY_NONE) {
        cc_assign(cc, cc_key(A.learn_target));
        A.learn_on = false;
        set_learn_target(KEY_NONE);
        return;
    }
    for (int i = 0; i < g_gxset.ncc; i++) {
        if (g_gxset.cc[i].cc != cc) continue;
        int key = key_parse(g_gxset.cc[i].key);
        if (key == KEY_NONE) continue;
        set_val(key, m->d2 / 127.0f);
        gx_knob_t *k = KEY_IS_CH(key) ? (((key >> 5) & 7) == A.sel ? knob_by_key(KEY_SEL(key & 31))
                                                                  : NULL)
                                      : knob_by_key(key);
        if (k) knob_refresh(k);
    }
}

// --- refresh ------------------------------------------------------------------------------------

void refresh_channel(void) {
    main_page_refresh(false);
    A.knobs_dirty = true;
    if (A.page == 1) seq_page_refresh();
    if (A.learn_target != KEY_NONE) set_learn_target(A.learn_target);
}

void refresh_all(void) {
    if (A.page == 0) main_page_refresh(false);
    if (A.page == 1) seq_page_refresh();
    A.knobs_dirty = true;
    A.midi_dirty = true;
    A.strip_dirty = true;
}

void show_page(int i) {
    A.page = i;
    for (int j = 0; j < 4; j++) {
        if (j == i)
            lv_obj_remove_flag(A.pages[j], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(A.pages[j], LV_OBJ_FLAG_HIDDEN);
    }
    header_set_page(i);
    if (i == 0) main_page_refresh(false);
    if (i == 1) seq_page_refresh();
    if (i == 2) midi_page_refresh();
    A.knobs_dirty = true;
}

// --- the module's UI hooks -------------------------------------------------------------------------

void gx_ui_create(lv_obj_t *scr) {
    A.scr = scr;
    load_user_drums();
    lv_obj_set_style_bg_color(scr, sw_c(C_BG), 0);
    header_create(scr);
    void (*make[4])(lv_obj_t *) = {main_page_create, seq_page_create, midi_page_create,
                                   tools_page_create};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *g = A.pages[i] = sw_box(scr, 0, PAGE_Y, 1024, PAGE_H, C_BG, 0);
        lv_obj_add_flag(g, LV_OBJ_FLAG_CLICKABLE);
        make[i](g);
    }
    set_focus(KEY_SEL(K_OSC_FREQ));
    show_page(0);
    header_slot_ui();
}

void gx_ui_show(bool shown) {
    if (shown) refresh_all();
}

void gx_ui_poll(bool visible) {
    static uint32_t last, last_sound;
    uint32_t now = (uint32_t)(studio_now_us() / 1000);
    gx_midi_msg_t m;
    while (gx_midi_next(&m)) handle_midi(&m);
    if (A.sound_dirty && now - last_sound >= 30) {
        last_sound = now;
        A.sound_dirty = false;
        uint8_t mask = A.sound_dirty_ch;
        A.sound_dirty_ch = 0;
        gx_sound_changed(mask == 0xFF ? -1 : __builtin_ctz(mask));
        if (mask != 0xFF && (mask & (mask - 1))) gx_sound_changed(-1);   // several
    }
    gx_settings_poll();
    if (!visible || now - last < 25) return;
    last = now;
    header_tick(now);
    if (A.knobs_dirty) {
        A.knobs_dirty = false;
        for (int i = 0; i < g_nknobs; i++) {
            knob_retitle(g_knobs[i]);
            knob_refresh(g_knobs[i]);
        }
    }
    if (A.page == 0) main_page_tick(now);
    if (A.page == 1) seq_page_tick(now);
    if (A.page == 2) midi_page_tick(now);
}
