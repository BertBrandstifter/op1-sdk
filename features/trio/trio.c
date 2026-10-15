/* TRIO synth engine (spec: README.md): three chip oscillators with ring and sync into a gritty
 * multimode filter.
 *
 * Runs on the engine host (features/enginehost, tag 4). Page 1 (WAVE CUTOFF RES PW) lives in slots
 * 1-4 as 0..16383, so TE's LFO can move cutoff and pulse width. Slots 5-8 hold 0x4000 | two 6-bit
 * fields each: the high field = page 2 (INT2 INT3 DETUNE MODE), the low one = page 3 (FENV FDECAY
 * DRIVE NOISE). Every value is inside drwave's ranges (boot check passes, loads on stock as drwave).
 *
 * tr_init / tr_engine_name / tr_key / tr_knob / tr_frame: as WARP (warp.c), with 4 pages (page 4 =
 * the sound's FX knobs) and a 16-sound bank on shift + blue turn. */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "trio_core.h"
#include "../enginehost/eh_host.h"

#define KEY_KNOB_PUSH   59                      /* blue 59, green 60, white 61, orange 62 */
#define STEP_SCALE      10485                   /* ~100 clicks full range (as engine_plus) */
#define TAG             4
#define PAGES           4

static tr_voice voices[EH_VOICES];
volatile uint8_t tr_page;                       /* 0..3 = page 1..4 (bench reads) */
static volatile uint8_t consumed;
volatile int8_t tr_bank = -1;                   /* bench reads */
static volatile uint8_t bank_edited;
static volatile int8_t bank_slot = -1;

/* 6-bit fields: range per field (high = page 2, low = page 3) */
static const uint8_t hi_max[4] = { 48, 48, 63, 3 };    /* INT2 / INT3 = semitones + 24, DETUNE, MODE */

static int marked(const volatile int16_t *k) { return eh_tag(k) == TAG; }

static int field(const volatile int16_t *k, int slot, int hi)
{
    int v = k[4 + slot] & EH_P2_MAX;

    return hi ? v >> 6 : v & 63;
}

static int16_t pack(int hi, int lo) { return eh_p2(TAG, hi << 6 | lo); }

/* stored knob slots -> core parameters (0..32767) */
static void unpack(const volatile int16_t *k, int16_t *p)
{
    int i, v;

    for (i = 0; i < 4; i++) {
        v = k[i];                               /* TE's LFO adds to slots 1-4 (saturating) */
        v = v < 0 ? 0 : v > EH_PAGE1_MAX ? EH_PAGE1_MAX : v;
        p[i] = (int16_t)(v << 1);
        p[TR_FENV + i] = (int16_t)(field(k, i, 0) * 520);
    }
    v = field(k, 0, 1);
    p[TR_INT2] = (int16_t)(((2 * (v > 48 ? 48 : v) + 1) * 32768) / 98);   /* zone centre of 49 */
    v = field(k, 1, 1);
    p[TR_INT3] = (int16_t)(((2 * (v > 48 ? 48 : v) + 1) * 32768) / 98);
    p[TR_DETUNE] = (int16_t)(field(k, 2, 1) * 520);
    p[TR_MODE] = (int16_t)(((2 * (field(k, 3, 1) & 3) + 1) * 32768) / 8);
}

static void tr_eh_render(int vi, int16_t *out, const int16_t *knobs, const uint32_t *inc, int flags)
{
    int16_t p[TR_PARAMS];

    unpack(knobs, p);
    if (flags & EH_FRESH)
        tr_note_on(&voices[vi]);
    tr_render(&voices[vi], out, TE_VOICE_SAMPLES, inc, p);
}

void tr_init(void)
{
    eh_register(TAG, tr_eh_render);
}

/* Engine browser row "trio" (content_sql) -> "drwave" (TE knows no type "trio": fatal) */
void tr_engine_name(te_string *out, void *db, const char *sql)
{
    char *d = (char *)te_str_data(out);

    (void)db;
    (void)sql;
    if (out->size == 4 && d[0] == 't' && d[1] == 'r' && d[2] == 'i' && d[3] == 'o') {
        d[0] = 'd'; d[1] = 'r'; d[2] = 'w'; d[3] = 'a'; d[4] = 'v'; d[5] = 'e'; d[6] = 0; /* cap >= 15 */
        out->size = 6;
        tr_page = 0;
        bank_slot = -1;
    }
}

/* ------------------------------------------------------------------ KeyHandler thread */

static volatile int16_t *current(int *slot)
{
    *slot = *TE_SYNTH_SLOT & 7;
    return TE_SYNTH_KNOBS + *slot * 8;
}

static int engine_screen_trio(void)
{
    int slot;

    return *TE_EDIT_MODE == 0 && *TE_SYNTH_SUBSCREEN == 0
           && TE_SYNTH_TYPE[*TE_SYNTH_SLOT & 7] == TE_INSTR_DRWAVE && marked(current(&slot));
}

int tr_key(int key, int down)
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
    if (*TE_SHIFT_HELD || !engine_screen_trio())
        return 0;
    tr_page = (uint8_t)(tr_page + 1 < PAGES ? tr_page + 1 : 0);
    consumed |= (uint8_t)(1 << b);
    return 1;
}

/* built-in bank: set, cutoff %, res %, pw %, int2 / int3 semitones, detune %, mode, fenv %,
 * fdecay %, drive %, noise % */
typedef struct {
    const char *name;
    int8_t set, cut, res, pw, int2, int3, det, mode, fenv, fdec, drive, noise;
} bank_entry;

enum { LP, BP, HP, NOTCH };

static const bank_entry bank[] = {
    { "C64 BASS",   3,  35, 40, 40, -12, 0,   10, LP,    60, 20, 20, 0 },
    { "SID LEAD",   1,  60, 30, 30, 0,   12,  20, LP,    30, 40, 10, 0 },
    { "ARP CHIP",   1,  80, 10, 60, 12,  7,   0,  LP,    0,  0,  0,  0 },
    { "SYNC SCREAM", 10, 70, 50, 50, 19, 0,   0,  LP,    50, 50, 40, 0 },
    { "RING BELL",  13, 85, 20, 50, 0,   11,  0,  LP,    20, 60, 0,  0 },
    { "METAL",      14, 70, 40, 50, 12,  5,   10, BP,    40, 30, 20, 0 },
    { "HOOVER",     0,  70, 20, 50, -12, 0,   80, LP,    20, 60, 30, 0 },
    { "ACID",       4,  25, 85, 50, 0,   0,   0,  LP,    80, 25, 50, 0 },
    { "PWM PAD",    1,  50, 20, 30, 0,   12,  60, LP,    0,  0,  0,  0 },
    { "NOISE HAT",  9,  85, 30, 50, 0,   0,   0,  HP,    0,  0,  0,  0 },
    { "SNARE",      8,  60, 20, 50, 0,   0,   0,  BP,    70, 15, 0,  40 },
    { "THUMP",      2,  20, 50, 50, -12, -24, 0,  LP,    90, 10, 0,  0 },
    { "BRASS",      0,  40, 15, 50, 0,   12,  15, LP,    50, 50, 10, 0 },
    { "STRINGS",    0,  55, 10, 50, 0,   12,  40, LP,    10, 70, 0,  0 },
    { "GRIT BASS",  6,  45, 60, 50, 0,   0,   0,  LP,    40, 30, 80, 0 },
    { "NOTCH SWEEP", 7, 50, 70, 50, 0,   0,   0,  NOTCH, 70, 70, 0,  0 },
};
#define BANK            ((int)(sizeof(bank) / sizeof(bank[0])))

static int16_t zone1(int z, int n) { return (int16_t)(((2 * z + 1) * 32768 / (2 * n)) >> 1); }

static void bank_load(volatile int16_t *k, int slot, int e)
{
    const bank_entry *b = &bank[e];

    k[0] = zone1(b->set, TR_SETS);
    k[1] = (int16_t)(b->cut * EH_PAGE1_MAX / 100);
    k[2] = (int16_t)(b->res * EH_PAGE1_MAX / 100);
    k[3] = (int16_t)(b->pw * EH_PAGE1_MAX / 100);
    k[4] = pack(b->int2 + 24, b->fenv * 63 / 100);
    k[5] = pack(b->int3 + 24, b->fdec * 63 / 100);
    k[6] = pack(b->det * 63 / 100, b->drive * 63 / 100);
    k[7] = pack(b->mode, b->noise * 63 / 100);
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
    tr_bank = (int8_t)e;
    bank_slot = (int8_t)slot;
    bank_edited = 0;
}

int tr_knob(int knob, int16_t *diff)
{
    int slot, v, d = *diff;
    volatile int16_t *k;

    if ((unsigned int)knob > 3 || !engine_screen_trio())
        return 0;
    k = current(&slot);
    if (*TE_SHIFT_HELD) {                       /* drwave's shift + knob: not on a TRIO sound */
        if (knob == 0 && d) {
            int e = bank_slot == slot ? tr_bank : -1, s = d > 0 ? 1 : -1;

            bank_load(k, slot, e < 0 ? (s > 0 ? 0 : BANK - 1) : (e + s + BANK) % BANK);
        }
        return 1;
    }
    if (tr_page == 3)
        return eh_fx_knob(slot, knob, (int16_t)d);
    bank_edited = 1;
    if (tr_page == 0) {
        v = k[knob] + ((d * (STEP_SCALE >> 1)) >> 15);
        k[knob] = (int16_t)(v < 0 ? 0 : v > EH_PAGE1_MAX ? EH_PAGE1_MAX : v);
    } else {                                    /* 6-bit fields: one step per detent, more when fast */
        int hi = tr_page == 1, max = hi ? hi_max[knob] : 63;
        int step = (d < 0 ? -d : d) >> 10;

        step = step < 1 ? 1 : step;
        v = field(k, knob, hi) + (d > 0 ? step : -step);
        v = v < 0 ? 0 : v > max ? max : v;
        k[4 + knob] = hi ? pack(v, field(k, knob, 0)) : pack(field(k, knob, 1), v);
    }
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
    return 1;
}

/* ------------------------------------------------------------------ GUI thread */

static const char *const labels[12] = {
    "WAVE", "CUTOFF", "RES", "PW", "INT 2", "INT 3", "DETUNE", "MODE",
    "F.ENV", "F.DECAY", "DRIVE", "NOISE",
};
static const char *const page_names[PAGES] = { "OSC+FILTER", "PITCH", "ENV+GRIT", "FX" };

void tr_frame(uint16_t *buf)
{
    static const uint16_t col[4] = {
        GFX_RGB(0x69, 0x8e, 0xff), GFX_RGB(0x00, 0xed, 0x95),   /* blue, green */
        GFX_RGB(0xe6, 0xe6, 0xe6), GFX_RGB(0xff, 0x6a, 0x3d),   /* white, orange */
    };
    static const uint16_t dim = GFX_RGB(0x5a, 0x58, 0x66), track = GFX_RGB(0x35, 0x32, 0x38);
    static const uint8_t page_param[3][4] = {
        { TR_WAVE, TR_CUTOFF, TR_RES, TR_PW }, { TR_INT2, TR_INT3, TR_DETUNE, TR_MODE },
        { TR_FENV, TR_FDECAY, TR_DRIVE, TR_NOISE },
    };
    int16_t p[TR_PARAMS];
    volatile int16_t *k;
    int slot, i, x, pg = tr_page, s2, s3;

    if (!engine_screen_trio())
        return;
    k = current(&slot);
    unpack(k, p);

    gfx_begin(buf);
    gfx_rect(0, 0, gfx_w, 140, GFX_BLACK);
    x = gfx_text(4, 4, "TRIO", col[1], 2);
    if (tr_bank >= 0 && bank_slot == slot) {
        x = gfx_text(x + 8, 4, bank[(int)tr_bank].name, GFX_WHITE, 2);
        if (bank_edited)
            gfx_text(x, 4, "*", dim, 2);
    } else {
        gfx_text(x + 8, 4, "SHIFT+BLUE: PRESETS", dim, 1);
    }
    x = gfx_text(240, 4, "P", GFX_WHITE, 2);
    gfx_dec(x, 4, pg + 1, GFX_WHITE, 2);

    /* the oscillator set, the intervals, the filter */
    gfx_text(4, 24, tr_set_names[tr_set_of(p[TR_WAVE])], GFX_WHITE, 3);
    s2 = tr_semis_of(p[TR_INT2]);
    s3 = tr_semis_of(p[TR_INT3]);
    x = gfx_text(4, 50, "OSC2 ", dim, 2);
    x = gfx_text(x, 50, s2 > 0 ? "+" : "", col[0], 2);
    gfx_dec(x, 50, s2, col[0], 2);
    x = gfx_text(110, 50, "OSC3 ", dim, 2);
    x = gfx_text(x, 50, s3 > 0 ? "+" : "", col[1], 2);
    gfx_dec(x, 50, s3, col[1], 2);
    x = gfx_text(4, 68, "FILTER ", dim, 2);
    x = gfx_text(x, 68, tr_mode_names[tr_mode_of(p[TR_MODE])], col[3], 2);
    gfx_rect(x + 8, 72, 100, 4, track);
    gfx_rect(x + 8, 72, p[TR_CUTOFF] * 100 / 32767, 4, col[3]);

    if (pg == 3) {                              /* page 4: the sound's FX knobs (engine host) */
        eh_fx_draw(slot, 90);
        return;
    }
    for (i = 0; i < 4; i++) {
        int q = page_param[pg][i];

        x = 4 + i * 80;
        gfx_text(x, 96, labels[q], col[i], 2);
        gfx_rect(x, 110, 72, 6, track);
        gfx_rect(x, 110, p[q] * 72 / 32767, 6, col[i]);
    }
    x = gfx_text(4, 124, "PUSH: ", dim, 1);
    gfx_text(x, 124, page_names[(pg + 1) % PAGES], dim, 1);
}
