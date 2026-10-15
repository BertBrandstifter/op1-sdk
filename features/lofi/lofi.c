/* LOFI synth engine (spec: README.md): chiptune waves and pitch-tracking bytebeat, hosted on
 * TE's pulse engine.
 *
 * A pulse sound becomes LOFI when knob slots 5-8 are all negative: they then hold LOFI's page-2
 * values as ~v (v = 0..32766; -32768 is never stored). Stock pulse presets never have negative
 * slots, and the preset stays a "pulse" preset, so it loads on stock firmware too (as pulse).
 *
 * lf_te_render   pulse voice render slot: LOFI sound -> lofi_core renders the voice from TE's
 *                per-sample pitch buffer; otherwise TE's pulse render, untouched
 * lf_te_note_on  pulse note_on slot: TE's note_on, then mark the voice for a fresh LOFI note
 * lf_te_range_check  replaces TE's knob range check so stored LOFI sounds pass the patch
 *                validator (boot included)
 * lf_te_type_fetched  engine browser: the "lofi" row (content_sql) becomes a pulse sound with
 *                LOFI's defaults
 * lf_key         on_key: synth engine screen, LOFI sound: a knob push flips page 1/2 (consumed,
 *                down and up). LOFI is picked in TE's lists (engine list row, preset folder lofi)
 * lf_knob        on_knob: synth engine screen, LOFI sound: the knobs edit LOFI's 8 values and
 *                save them through TE (TE_PATCH_SET_KNOBS); TE's pulse knobs never see them.
 *                Shift + blue turn browses LOFI's built-in bank (14 sounds, the former factory
 *                presets: no TE preset rows, the factory list is capped at 176)
 * lf_frame       LOFI screen over pulse's on the engine screen: wave, page, values, scope
 * TE's envelope, velocity, volume, FX and LFO apply as for any engine. */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "lofi_core.h"
#include "lf_te246.h"

#define KEY_KNOB_PUSH   59                      /* blue 59, green 60, white 61, orange 62 */
#define STEP_SCALE      10485                   /* ~100 clicks full range (as engine_plus) */
#define VOICES          8
#define V_MAX           32766                   /* stored as ~v: never -32768 */

static lf_voice voices[VOICES];
static volatile uint8_t fresh[VOICES];          /* note_on seen, render starts the note */
volatile uint8_t lf_page;                       /* 0 = page 1, 1 = page 2 (bench reads) */
static volatile uint8_t consumed;               /* pushes whose key-up we eat */
volatile int8_t lf_bank = -1;                   /* last loaded bank entry (-1 none), bench reads */
static volatile int8_t bank_slot = -1;          /* sound it was loaded into */
static volatile uint8_t bank_edited;            /* knobs changed after loading it */
static int16_t scope[TE_VOICE_SAMPLES];         /* last rendered block of the newest note */
static volatile int scope_voice = -1;

static int marked(const volatile int16_t *k)
{
    return (k[4] & k[5] & k[6] & k[7]) < 0;
}

static int voice_index(void *self)
{
    uint32_t a = (uint32_t)self - LF_TE_VOICE_BASE, vi = a / LF_TE_PULSE_SIZE;

    return a % LF_TE_PULSE_SIZE == 0 && vi < VOICES ? (int)vi : -1;
}

void lf_te_render(void *self, int16_t *out, int16_t *knobs)
{
    int16_t k[LF_PARAMS];
    int vi = voice_index(self), i;

    if (!marked(knobs) || vi < 0) {
        LF_TE_PULSE_RENDER(self, out, knobs);   /* stock pulse */
        return;
    }
    for (i = 0; i < 4; i++) {
        k[i] = knobs[i] < 0 ? 0 : knobs[i];     /* TE's LFO adds to slots 1-4 (saturating) */
        k[4 + i] = (int16_t)~knobs[4 + i];
    }
    if (fresh[vi]) {
        fresh[vi] = 0;
        lf_note_on(&voices[vi]);
        scope_voice = vi;
    }
    lf_render(&voices[vi], out, TE_VOICE_SAMPLES, LF_TE_VOICE_INC, k);
    if (vi == scope_voice)
        for (i = 0; i < TE_VOICE_SAMPLES; i++)
            scope[i] = out[i];
}

void lf_te_note_on(void *self, int a1, int a2)
{
    int vi = voice_index(self);

    LF_TE_PULSE_NOTE_ON(self, a1, a2);
    if (vi >= 0)
        fresh[vi] = 1;
}

/* TE's knob range check, rebuilt (same table, same result) with one exception for pulse sounds:
 * LOFI's values pass, page 1 (0..32767) in slots 1-4 and page 2 (~v, -32767..-1) in slots 5-8.
 * Without it TE's patch validator rejects a stored LOFI sound at boot (fatal "Invalid synth
 * patch ... error 15"). */
int lf_te_range_check(int type, int slot, const int16_t *v)
{
    const volatile int16_t *r;

    if (slot < 0 || slot >= 8)
        return 0;
    if (type == TE_INSTR_PULSE && (slot < 4 ? *v >= 0 : *v < 0 && *v != -32768))
        return 1;
    r = LF_TE_KNOB_RANGES + type * 32 + slot * 4;
    return *v >= r[0] && *v <= r[1];
}

/* Engine browser row "lofi" (content_sql adds it to TE's synth_types with LOFI's defaults as its
 * default_params): right after TE reads the picked row's type name, "lofi" becomes "pulse", so the
 * sound is a pulse sound with LOFI's marker (TE knows no type "lofi": that would be fatal). */
void lf_te_type_fetched(te_string *out, void *db, const char *sql)
{
    char *d = (char *)te_str_data(out);

    (void)db;
    (void)sql;
    if (out->size == 4 && d[0] == 'l' && d[1] == 'o' && d[2] == 'f' && d[3] == 'i') {
        d[0] = 'p'; d[1] = 'u'; d[2] = 'l'; d[3] = 's'; d[4] = 'e'; d[5] = 0;   /* capacity >= 15 */
        out->size = 5;
        lf_page = 0;
        bank_slot = -1;
    }
}

/* ------------------------------------------------------------------ KeyHandler thread */

static volatile int16_t *current(int *slot)
{
    *slot = *TE_SYNTH_SLOT & 7;
    return TE_SYNTH_KNOBS + *slot * 8;
}

static int engine_screen_pulse(void)
{
    return *TE_EDIT_MODE == 0 && *TE_SYNTH_SUBSCREEN == 0
           && TE_SYNTH_TYPE[*TE_SYNTH_SLOT & 7] == TE_INSTR_PULSE;
}

static void save(int slot, volatile int16_t *k)
{
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
}

int lf_key(int key, int down)
{
    int b = key - KEY_KNOB_PUSH, slot;
    volatile int16_t *k;

    if (b < 0 || b > 3)
        return 0;
    if (!down) {
        if (consumed & (1 << b)) {
            consumed &= (uint8_t)~(1 << b);
            return 1;
        }
        return 0;
    }
    if (!engine_screen_pulse())
        return 0;
    k = current(&slot);
    if (!marked(k) || *TE_SHIFT_HELD)
        return 0;
    lf_page ^= 1;
    consumed |= (uint8_t)(1 << b);
    return 1;
}

/* built-in bank: stored knob slots as they are saved (page 1 raw, page 2 as ~v) */
static const struct {
    const char *name;
    int16_t k[8];
} bank[] = {
    { "CHIP LEAD", { 655, 16000, 0, 0, -1, -1, -1, -6001 } },
    { "NES ARP", { 655, 24000, 0, 0, -1, -14337, -1, -4001 } },
    { "NES MINOR", { 655, 8000, 0, 0, -1, -18433, -1, -4001 } },
    { "TRI BASS", { 1965, 0, 0, 0, -1, -1, -1, -1 } },
    { "SAW GRIT", { 3276, 16000, 9000, 14000, -6001, -1, -1, -8001 } },
    { "CHIP NOISE", { 4587, 12000, 0, 0, -1, -1, -1, -1 } },
    { "METAL BELL", { 5897, 0, 0, 0, -1, -1, -1, -12001 } },
    { "GB WAVE", { 7208, 0, 0, 20000, -1, -1, -1, -1 } },
    { "GB MORPH", { 8519, 20000, 0, 0, -8001, -1, -1, -6001 } },
    { "BYTE ONE", { 12451, 0, 0, 0, -1, -1, -1, -4001 } },
    { "BYTE LOOP", { 16383, 9000, 0, 0, -1, -22001, -1, -4001 } },
    { "BYTE XOR", { 22937, 16000, 0, 0, -12001, -1, -1, -6001 } },
    { "BYTE TONE", { 29490, 0, 0, 0, -1, -30001, -1, -2001 } },
    { "SWEEP DRUM", { 655, 0, 0, 0, -1, -1, -22001, -1 } },
};
#define BANK            ((int)(sizeof(bank) / sizeof(bank[0])))

/* shift + blue turn: one bank entry per knob event (one detent), wrapping */
static void bank_browse(volatile int16_t *k, int slot, int16_t diff)
{
    int e = bank_slot == slot ? lf_bank : -1, d = diff > 0 ? 1 : -1, i;

    if (!diff)
        return;
    e = e < 0 ? (d > 0 ? 0 : BANK - 1) : (e + d + BANK) % BANK;
    for (i = 0; i < 8; i++)
        k[i] = bank[e].k[i];
    save(slot, k);
    lf_bank = (int8_t)e;
    bank_slot = (int8_t)slot;
    bank_edited = 0;
}

int lf_knob(int knob, int16_t *diff)
{
    int slot, i, v;
    volatile int16_t *k;

    if ((unsigned int)knob > 3 || !engine_screen_pulse())
        return 0;
    k = current(&slot);
    if (!marked(k))
        return 0;
    if (*TE_SHIFT_HELD) {                       /* TE's shift + knob on pulse: not on a LOFI sound */
        if (knob == 0)
            bank_browse(k, slot, *diff);
        return 1;
    }
    bank_edited = 1;
    i = lf_page ? 4 + knob : knob;
    v = (i < 4 ? k[i] : ~k[i]) + ((*diff * STEP_SCALE) >> 15);
    v = v < 0 ? 0 : v > V_MAX ? V_MAX : v;
    k[i] = (int16_t)(i < 4 ? v : ~v);
    save(slot, k);
    return 1;
}

/* ------------------------------------------------------------------ GUI thread */

static const char *const wave_names[LF_W_BYTE] = {
    "PULSE", "TRIANGLE", "SAW", "NOISE", "METAL", "WAVE A", "WAVE B", "WAVE C", "WAVE D",
};

static const char *label(int i, int w)
{
    static const char *const fixed[LF_PARAMS] = {
        "WAVE", 0, "CRUSH", "BITS", "BEND", 0, "SWEEP", "TONE",
    };

    if (i == LF_SHAPE)
        return w == LF_W_PULSE ? "DUTY" : w <= LF_W_SAW ? "STEPS" : w <= LF_W_METAL ? "COLOR"
               : w < LF_W_BYTE ? "MORPH" : "VAR";
    if (i == LF_LOOP)
        return w < LF_W_BYTE ? "ARP" : "LOOP";
    return fixed[i];
}

void lf_frame(uint16_t *buf)
{
    static const uint16_t col[4] = {
        GFX_RGB(0x69, 0x8e, 0xff), GFX_RGB(0x00, 0xed, 0x95),   /* blue, green */
        GFX_RGB(0xe6, 0xe6, 0xe6), GFX_RGB(0xff, 0x6a, 0x3d),   /* white, orange */
    };
    static const uint16_t dim = GFX_RGB(0x5a, 0x58, 0x66), track = GFX_RGB(0x35, 0x32, 0x38);
    int16_t v[LF_PARAMS];
    volatile int16_t *k;
    int slot, i, w, x, y, pg = lf_page, sv;

    if (!engine_screen_pulse())
        return;
    k = current(&slot);
    if (!marked(k))
        return;
    for (i = 0; i < LF_PARAMS; i++)
        v[i] = (int16_t)(i < 4 ? (k[i] < 0 ? 0 : k[i]) : ~k[i]);
    w = lf_wave(v[LF_WAVE]);

    gfx_begin(buf);
    gfx_rect(0, 0, gfx_w, gfx_h, GFX_BLACK);
    x = gfx_text(4, 4, "LOFI", col[1], 2);
    if (lf_bank >= 0 && bank_slot == slot) {   /* the bank entry this sound came from */
        x = gfx_text(x + 8, 4, bank[(int)lf_bank].name, GFX_WHITE, 2);
        if (bank_edited)
            gfx_text(x, 4, "*", dim, 2);
    } else {
        gfx_text(x + 8, 4, "SHIFT+BLUE: PRESETS", dim, 1);
    }
    gfx_text(272, 4, pg ? "P2" : "P1", GFX_WHITE, 2);
    if (w < LF_W_BYTE) {
        x = gfx_text(4, 22, wave_names[w], GFX_WHITE, 3);
    } else {
        x = gfx_text(4, 22, "BYTE ", GFX_WHITE, 3);
        gfx_dec(x, 22, w - LF_W_BYTE + 1, GFX_WHITE, 3);
    }

    /* scope: the newest note's last block, 128 samples over 256 px */
    sv = scope_voice;
    gfx_rect(32, 82, 256, 1, track);
    if (sv >= 0)
        for (i = 0; i < TE_VOICE_SAMPLES; i++) {
            y = 82 - (scope[i] * 28 >> 15);
            gfx_rect(32 + i * 2, y, 2, 2, col[1]);
        }

    /* the four knobs of this page, the other page's values dimmed below */
    for (i = 0; i < 4; i++) {
        int p = pg * 4 + i, o = (1 - pg) * 4 + i;

        x = 4 + i * 80;
        gfx_text(x, 118, label(p, w), col[i], 2);
        gfx_rect(x, 132, 72, 6, track);
        gfx_rect(x, 132, v[p] * 72 / V_MAX, 6, col[i]);
        gfx_text(x, 144, label(o, w), dim, 1);
        gfx_rect(x + 40, 146, 32, 3, track);
        gfx_rect(x + 40, 146, v[o] * 32 / V_MAX, 3, dim);
    }
}
