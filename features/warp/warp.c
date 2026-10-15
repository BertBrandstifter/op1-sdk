/* WARP synth engine (spec: README.md): a single-cycle wave through 16 phase / output warps.
 *
 * Runs on the engine host (features/enginehost, tag 6): a drwave sound is WARP when knob slots
 * 5-8 all hold 0x6000 | v (v = 0..4095, page 2); page 1 lives in slots 1-4 as 0..16383. Every
 * value is inside drwave's ranges (boot check passes, loads on stock firmware as drwave).
 *
 * wp_init         registers the voice renderer with the engine host (tag 6)
 * wp_engine_name  engine browser (shared point engine_name): the "warp" row becomes a drwave
 *                 sound with WARP's defaults
 * wp_key          on_key: synth engine screen, WARP sound: knob push cycles page 1 / 2 / 3
 * wp_knob         on_knob: synth engine screen, WARP sound: pages 1-2 edit WARP's 8 values (saved
 *                 through TE_PATCH_SET_KNOBS), page 3 the sound's FX knobs (engine host);
 *                 shift + blue turn browses WARP's built-in bank (16 sounds)
 * wp_frame        WARP screen over drwave's on the engine screen (above y 140)
 * TE's envelope, velocity, volume, FX and LFO apply as for any engine. */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "warp_core.h"
#include "../enginehost/eh_host.h"

#define KEY_KNOB_PUSH   59                      /* blue 59, green 60, white 61, orange 62 */
#define STEP_SCALE      10485                   /* ~100 clicks full range (as engine_plus) */
#define TAG             6
#define PAGES           3

static wp_voice voices[EH_VOICES];
volatile uint8_t wp_page;                       /* 0..2 = page 1..3 (bench reads) */
static volatile uint8_t consumed;               /* pushes whose key-up we eat */
volatile int8_t wp_bank = -1;                   /* last loaded bank entry (-1 none), bench reads */
static volatile uint8_t bank_edited;
static volatile int8_t bank_slot = -1;

static int marked(const volatile int16_t *k) { return eh_tag(k) == TAG; }

/* stored knob slots -> core parameters (0..32767) */
static void unpack(const volatile int16_t *k, int16_t *p)
{
    int i, v;

    for (i = 0; i < 4; i++) {
        v = k[i];                               /* TE's LFO adds to slots 1-4 (saturating) */
        v = v < 0 ? 0 : v > EH_PAGE1_MAX ? EH_PAGE1_MAX : v;
        p[i] = (int16_t)(v << 1);
        p[4 + i] = (int16_t)((k[4 + i] & EH_P2_MAX) << 3);
    }
}

static void wp_eh_render(int vi, int16_t *out, const int16_t *knobs, const uint32_t *inc, int flags)
{
    int16_t p[WP_PARAMS];

    unpack(knobs, p);
    if (flags & EH_FRESH)
        wp_note_on(&voices[vi]);
    wp_render(&voices[vi], out, TE_VOICE_SAMPLES, inc, p);
}

void wp_init(void)
{
    eh_register(TAG, wp_eh_render);
}

/* Engine browser row "warp" (content_sql): right after TE reads the picked row's type name,
 * "warp" becomes "drwave" (TE knows no type "warp": that would be fatal). */
void wp_engine_name(te_string *out, void *db, const char *sql)
{
    char *d = (char *)te_str_data(out);

    (void)db;
    (void)sql;
    if (out->size == 4 && d[0] == 'w' && d[1] == 'a' && d[2] == 'r' && d[3] == 'p') {
        d[0] = 'd'; d[1] = 'r'; d[2] = 'w'; d[3] = 'a'; d[4] = 'v'; d[5] = 'e'; d[6] = 0; /* cap >= 15 */
        out->size = 6;
        wp_page = 0;
        bank_slot = -1;
    }
}

/* ------------------------------------------------------------------ KeyHandler thread */

static volatile int16_t *current(int *slot)
{
    *slot = *TE_SYNTH_SLOT & 7;
    return TE_SYNTH_KNOBS + *slot * 8;
}

static int engine_screen_warp(void)
{
    int slot;

    return *TE_EDIT_MODE == 0 && *TE_SYNTH_SUBSCREEN == 0
           && TE_SYNTH_TYPE[*TE_SYNTH_SLOT & 7] == TE_INSTR_DRWAVE && marked(current(&slot));
}

int wp_key(int key, int down)
{
    int b = key - KEY_KNOB_PUSH;

    if (b < 0 || b > 3)
        return 0;
    if (!down) {
        if (consumed & (1 << b)) {
            consumed &= (uint8_t)~(1 << b);
            return 1;
        }
        return 0;
    }
    if (*TE_SHIFT_HELD || !engine_screen_warp())
        return 0;
    wp_page = (uint8_t)(wp_page + 1 < PAGES ? wp_page + 1 : 0);
    consumed |= (uint8_t)(1 << b);
    return 1;
}

/* built-in bank: wave, mode, then amount, color, sweep, decay, motion, unison in % */
typedef struct {
    const char *name;
    int8_t wave, mode, amount, color, sweep, decay, motion, unison;
} bank_entry;

enum { SINE, TRI, SAW, SQUARE, PULSE, HALF, ORGAN, VOX };

static const bank_entry bank[] = {
    { "CZ BASS",    SAW,    WP_BEND,    40, 30, 60, 20, 0,  0 },
    { "SYNC LEAD",  SAW,    WP_SYNC,    30, 0,  50, 35, 0,  40 },
    { "FOLD PAD",   SINE,   WP_FOLD,    35, 20, 0,  50, 35, 60 },
    { "GLASS",      SINE,   WP_FM,      25, 0,  40, 25, 0,  0 },
    { "MIRROR KEY", TRI,    WP_MIRROR,  50, 10, 30, 30, 0,  20 },
    { "BITCRUSH",   SQUARE, WP_QUANT,   60, 0,  0,  0,  0,  0 },
    { "PLUCK",      SAW,    WP_SQUEEZE, 10, 20, 80, 15, 0,  0 },
    { "PINCH VOX",  VOX,    WP_PINCH,   45, 20, 0,  40, 30, 30 },
    { "WRAP BELL",  SINE,   WP_WRAP,    30, 0,  60, 45, 0,  0 },
    { "GAP CHIP",   PULSE,  WP_GAP,     50, 0,  0,  0,  0,  0 },
    { "BITREV FX",  SAW,    WP_BITREV,  50, 30, 0,  0,  50, 0 },
    { "CHAOS",      TRI,    WP_CHAOS,   60, 25, 30, 40, 40, 0 },
    { "STAIR BASS", SINE,   WP_STAIR,   60, 40, 0,  0,  0,  0 },
    { "HARMONICS",  ORGAN,  WP_HARM,    30, 0,  40, 60, 0,  30 },
    { "RING METAL", SINE,   WP_RING,    60, 0,  30, 50, 0,  0 },
    { "XOR GLITCH", SQUARE, WP_XOR,     50, 20, 0,  0,  40, 0 },
};
#define BANK            ((int)(sizeof(bank) / sizeof(bank[0])))

/* centre of zone z of n, stored as a page-1 value (0..16383) */
static int16_t zone1(int z, int n) { return (int16_t)(((2 * z + 1) * 32768 / (2 * n)) >> 1); }

static void bank_load(volatile int16_t *k, int slot, int e)
{
    const bank_entry *b = &bank[e];

    k[0] = zone1(b->wave, 8);
    k[1] = zone1(b->mode, WP_MODES);
    k[2] = (int16_t)(b->amount * EH_PAGE1_MAX / 100);
    k[3] = (int16_t)(b->color * EH_PAGE1_MAX / 100);
    k[4] = eh_p2(TAG, b->sweep * EH_P2_MAX / 100);
    k[5] = eh_p2(TAG, b->decay * EH_P2_MAX / 100);
    k[6] = eh_p2(TAG, b->motion * EH_P2_MAX / 100);
    k[7] = eh_p2(TAG, b->unison * EH_P2_MAX / 100);
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
    wp_bank = (int8_t)e;
    bank_slot = (int8_t)slot;
    bank_edited = 0;
}

int wp_knob(int knob, int16_t *diff)
{
    int slot, i, v;
    volatile int16_t *k;

    if ((unsigned int)knob > 3 || !engine_screen_warp())
        return 0;
    k = current(&slot);
    if (*TE_SHIFT_HELD) {                       /* drwave's shift + knob: not on a WARP sound */
        if (knob == 0 && *diff) {
            int e = bank_slot == slot ? wp_bank : -1, d = *diff > 0 ? 1 : -1;

            bank_load(k, slot, e < 0 ? (d > 0 ? 0 : BANK - 1) : (e + d + BANK) % BANK);
        }
        return 1;
    }
    if (wp_page == 2)
        return eh_fx_knob(slot, knob, *diff);
    bank_edited = 1;
    i = wp_page ? 4 + knob : knob;
    if (i < 4) {
        v = k[i] + ((*diff * (STEP_SCALE >> 1)) >> 15);
        k[i] = (int16_t)(v < 0 ? 0 : v > EH_PAGE1_MAX ? EH_PAGE1_MAX : v);
    } else {
        v = (k[i] & EH_P2_MAX) + ((*diff * (STEP_SCALE >> 3)) >> 15);
        k[i] = eh_p2(TAG, v < 0 ? 0 : v > EH_P2_MAX ? EH_P2_MAX : v);
    }
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
    return 1;
}

/* ------------------------------------------------------------------ GUI thread */

static const char *const labels[WP_PARAMS] = {
    "WAVE", "MODE", "AMOUNT", "COLOR", "SWEEP", "DECAY", "MOTION", "UNISON",
};

void wp_frame(uint16_t *buf)
{
    static const uint16_t col[4] = {
        GFX_RGB(0x69, 0x8e, 0xff), GFX_RGB(0x00, 0xed, 0x95),   /* blue, green */
        GFX_RGB(0xe6, 0xe6, 0xe6), GFX_RGB(0xff, 0x6a, 0x3d),   /* white, orange */
    };
    static const uint16_t dim = GFX_RGB(0x5a, 0x58, 0x66), track = GFX_RGB(0x35, 0x32, 0x38);
    int16_t p[WP_PARAMS], cyc[128];
    volatile int16_t *k;
    int slot, i, x, y, pg = wp_page, wave, mode;

    if (!engine_screen_warp())
        return;
    k = current(&slot);
    unpack(k, p);
    wave = wp_wave_of(p[WP_WAVE]);
    mode = wp_mode_of(p[WP_MODE]);

    gfx_begin(buf);
    gfx_rect(0, 0, gfx_w, 140, GFX_BLACK);
    x = gfx_text(4, 4, "WARP", col[1], 2);
    if (wp_bank >= 0 && bank_slot == slot) {
        x = gfx_text(x + 8, 4, bank[(int)wp_bank].name, GFX_WHITE, 2);
        if (bank_edited)
            gfx_text(x, 4, "*", dim, 2);
    } else {
        gfx_text(x + 8, 4, "SHIFT+BLUE: PRESETS", dim, 2);
    }
    gfx_text(272, 4, pg == 2 ? "P3" : pg ? "P2" : "P1", GFX_WHITE, 2);

    /* one cycle of the warped wave (amount as set, without sweep / motion), 128 points */
    wp_cycle(wave, mode, p[WP_AMOUNT], cyc, 128);
    gfx_rect(4, 52, 128, 1, track);
    for (i = 0; i < 128; i++) {
        y = 52 - (cyc[i] * 30 >> 15);
        gfx_rect(4 + i, y, 1, 2, col[1]);
    }
    gfx_text(144, 22, wp_wave_names[wave], GFX_WHITE, 2);
    gfx_text(144, 42, wp_mode_names[mode], col[1], 3);
    x = gfx_text(144, 68, "AMOUNT ", dim, 1);
    gfx_dec(x, 68, p[WP_AMOUNT] * 100 / 32767, GFX_WHITE, 1);

    if (pg == 2) {                              /* page 3: the sound's FX knobs (engine host) */
        eh_fx_draw(slot, 90);
        return;
    }
    for (i = 0; i < 4; i++) {
        int q = pg * 4 + i, o = (1 - pg) * 4 + i;

        x = 4 + i * 80;
        gfx_text(x, 96, labels[q], col[i], 2);
        gfx_rect(x, 110, 72, 6, track);
        gfx_rect(x, 110, p[q] * 72 / 32767, 6, col[i]);
        gfx_text(x, 122, labels[o], dim, 1);
        gfx_rect(x + 40, 124, 32, 3, track);
        gfx_rect(x + 40, 124, p[o] * 32 / 32767, 3, dim);
    }
}
