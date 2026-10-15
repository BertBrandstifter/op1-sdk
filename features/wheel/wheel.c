/* WHEEL synth engine (spec: README.md): tonewheel organ hosted on TE's drwave engine.
 *
 * Runs on the engine host (features/enginehost, tag 5): a drwave sound is WHEEL when knob slots
 * 5-8 all hold 0x5000 | v (v = 0..4095, page 2). Page 1 lives in slots 1-4 as 0..16383. Every value
 * is inside drwave's own ranges, so a WHEEL sound passes TE's boot-time patch validator unchanged
 * and loads on stock firmware as a drwave sound.
 *
 * wh_init         registers the voice renderer with the engine host (tag 5)
 * wh_engine_name  engine browser (shared point engine_name): the "wheel" row becomes a drwave
 *                 sound with WHEEL's defaults
 * wh_key          on_key: synth engine screen, WHEEL sound: knob push cycles page 1 / 2 / 3
 * wh_knob         on_knob: synth engine screen, WHEEL sound: pages 1-2 edit WHEEL's 8 values and
 *                 save them through TE (TE_PATCH_SET_KNOBS); page 3 edits the sound's FX knobs 1-4
 *                 (what TE's FX screen edits), clamped to TE's range for that FX, saved through
 *                 TE's FX save; drwave's knobs never see them
 *                 Shift + blue turn browses WHEEL's built-in bank (24 organ sounds): loads all 8
 *                 values (no TE preset rows: the factory list is capped at 176)
 * wh_frame        WHEEL screen over drwave's on the engine screen (kept above y 140, where
 *                 engine_plus draws its knob row)
 * TE's envelope, velocity, volume, FX and LFO apply as for any engine. */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "wheel_core.h"
#include "wh_te246.h"
#include "../enginehost/eh_host.h"

#define KEY_KNOB_PUSH   59                      /* blue 59, green 60, white 61, orange 62 */
#define STEP_SCALE      10485                   /* ~100 clicks full range (as engine_plus) */
#define VOICES          EH_VOICES
#define TAG             5
#define MARK            (TAG << 12)
#define MARK_MASK       0xf000
#define P2_MAX          0x0fff

static wh_voice voices[VOICES];
static wh_rotor rotor;
volatile uint8_t wh_page;                       /* 0..2 = page 1..3 (bench reads) */
#define PAGES           3
static volatile uint8_t consumed;               /* pushes whose key-up we eat */
volatile int8_t wh_bank = -1;                   /* last loaded bank entry (-1 none), bench reads */
static volatile uint8_t bank_edited;            /* knobs changed after loading it */
static volatile int8_t bank_slot = -1;          /* sound it was loaded into */

static int marked(const volatile int16_t *k)
{
    return eh_tag(k) == TAG;
}

/* stored knob slots -> core parameters (0..32767) */
static void unpack(const volatile int16_t *k, int16_t *p)
{
    int i, v;

    for (i = 0; i < 4; i++) {
        v = k[i];                               /* TE's LFO adds to slots 1-4 (saturating) */
        v = v < 0 ? 0 : v > WH_TE_PAGE1_MAX ? WH_TE_PAGE1_MAX : v;
        p[i] = (int16_t)(v << 1);
        p[4 + i] = (int16_t)((k[4 + i] & P2_MAX) << 3);
    }
}

/* engine host: one voice block (the host checked the tag) */
static void wh_eh_render(int vi, int16_t *out, const int16_t *knobs, const uint32_t *inc, int flags)
{
    int16_t p[WH_PARAMS];

    unpack(knobs, p);
    if (flags & EH_BLOCK)
        wh_rotor_block(&rotor, p, TE_VOICE_SAMPLES);
    if (flags & EH_FRESH)
        wh_note_on(&voices[vi]);
    wh_render(&voices[vi], &rotor, out, TE_VOICE_SAMPLES, inc, p);
}

void wh_init(void)
{
    eh_register(TAG, wh_eh_render);
}

/* Engine browser row "wheel" (content_sql adds it to TE's synth_types with WHEEL's defaults as its
 * default_params): right after TE reads the picked row's type name, "wheel" becomes "drwave", so
 * the sound is a drwave sound with WHEEL's marker (TE knows no type "wheel": that would be fatal). */
void wh_engine_name(te_string *out, void *db, const char *sql)
{
    char *d = (char *)te_str_data(out);

    (void)db;
    (void)sql;
    if (out->size == 5 && d[0] == 'w' && d[1] == 'h' && d[2] == 'e' && d[3] == 'e' && d[4] == 'l') {
        d[0] = 'd'; d[1] = 'r'; d[2] = 'w'; d[3] = 'a'; d[4] = 'v'; d[5] = 'e'; d[6] = 0; /* cap >= 15 */
        out->size = 6;
        wh_page = 0;
        bank_slot = -1;
    }
}

/* ------------------------------------------------------------------ KeyHandler thread */

static volatile int16_t *current(int *slot)
{
    *slot = *TE_SYNTH_SLOT & 7;
    return TE_SYNTH_KNOBS + *slot * 8;
}

static int engine_screen_wheel(void)
{
    int slot;

    return *TE_EDIT_MODE == 0 && *TE_SYNTH_SUBSCREEN == 0
           && TE_SYNTH_TYPE[*TE_SYNTH_SLOT & 7] == TE_INSTR_DRWAVE && marked(current(&slot));
}

int wh_key(int key, int down)
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
    if (*TE_SHIFT_HELD || !engine_screen_wheel())
        return 0;
    wh_page = (uint8_t)(wh_page + 1 < PAGES ? wh_page + 1 : 0);
    consumed |= (uint8_t)(1 << b);
    return 1;
}

/* built-in bank: registration, percussion, drive %, rotary, bass/body/top -8..8, click % */
typedef struct {
    const char *name;
    int8_t reg, perc, drive, rot, bass, body, top, click;
} bank_entry;

enum { FLUTE, SOFT, BALLAD, JAZZ, BLUES, SMOOTH, CLARINET, GOSPEL, ROCK, SHRILL, REED, THEATRE,
       SPARKLE, BRIGHT, HIGH, FULL };
enum { P_OFF, P_2ND, P_3RD, P_2SOFT, P_3SOFT, P_2SLOW, P_3SLOW };
enum { R_OFF, R_SLOW, R_FAST };

static const bank_entry bank[] = {
    { "JAZZ TRIO",  JAZZ,     P_3RD,   0,   R_SLOW, 0,  0,  0,  40 },
    { "SOUL JAZZ",  JAZZ,     P_3RD,   25,  R_FAST, 0,  0,  0,  50 },
    { "BLUES BAR",  BLUES,    P_2ND,   35,  R_SLOW, 0,  0,  0,  40 },
    { "GOSPEL",     GOSPEL,   P_OFF,   20,  R_FAST, 0,  0,  0,  30 },
    { "CATHEDRAL",  FULL,     P_OFF,   0,   R_OFF,  1,  0,  -2, 0 },
    { "BALLAD",     BALLAD,   P_OFF,   0,   R_SLOW, 0,  0,  0,  10 },
    { "FLUTE",      FLUTE,    P_OFF,   0,   R_SLOW, 0,  0,  0,  0 },
    { "SOFT PAD",   SOFT,     P_OFF,   0,   R_SLOW, 0,  0,  -2, 0 },
    { "ROCK",       ROCK,     P_OFF,   60,  R_FAST, 0,  0,  0,  60 },
    { "OVERDRIVE",  FULL,     P_OFF,   85,  R_FAST, -2, 0,  0,  70 },
    { "SHRILL",     SHRILL,   P_OFF,   30,  R_FAST, 0,  0,  0,  60 },
    { "REED",       REED,     P_OFF,   10,  R_SLOW, 0,  0,  0,  20 },
    { "CLARINET",   CLARINET, P_OFF,   0,   R_OFF,  0,  0,  0,  10 },
    { "THEATRE",    THEATRE,  P_OFF,   0,   R_SLOW, 0,  0,  0,  0 },
    { "SPARKLE",    SPARKLE,  P_2SOFT, 0,   R_SLOW, 0,  0,  0,  30 },
    { "ORGAN BASS", BALLAD,   P_3SOFT, 15,  R_OFF,  2,  0,  -4, 50 },
    { "SMOOTH",     SMOOTH,   P_3SOFT, 0,   R_SLOW, 0,  0,  0,  20 },
    { "BRIGHT",     BRIGHT,   P_2ND,   20,  R_FAST, 0,  0,  0,  40 },
    { "HIGH",       HIGH,     P_OFF,   0,   R_FAST, 0,  0,  0,  20 },
    { "SKA",        JAZZ,     P_2ND,   10,  R_OFF,  -2, 0,  0,  100 },
    { "GRIT",       BLUES,    P_3RD,   100, R_FAST, 0,  0,  0,  60 },
    { "DREAMY",     SOFT,     P_2SLOW, 0,   R_SLOW, 0,  0,  -3, 0 },
    { "TOY",        SPARKLE,  P_3SLOW, 0,   R_OFF,  0,  0,  0,  100 },
    { "FULL ON",    FULL,     P_3RD,   50,  R_FAST, 0,  0,  0,  50 },
};
#define BANK            ((int)(sizeof(bank) / sizeof(bank[0])))


/* centre of zone z of n, stored as a page-1 value (0..16383) */
static int16_t zone1(int z, int n) { return (int16_t)(((2 * z + 1) * 32768 / (2 * n)) >> 1); }

static void bank_load(volatile int16_t *k, int slot, int e)
{
    const bank_entry *b = &bank[e];
    int i, off[3] = { b->bass, b->body, b->top };

    k[0] = zone1(b->reg, WH_REGS);
    k[1] = zone1(b->perc, WH_PERCS);
    k[2] = (int16_t)(b->drive * WH_TE_PAGE1_MAX / 100);
    k[3] = zone1(b->rot, WH_ROTS);
    for (i = 0; i < 3; i++)
        k[4 + i] = (int16_t)(MARK | (zone1(off[i] + 8, 17) >> 2));    /* page 2: 12 bits */
    k[7] = (int16_t)(MARK | (b->click * P2_MAX / 100));
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
    wh_bank = (int8_t)e;
    bank_slot = (int8_t)slot;
    bank_edited = 0;
}

/* shift + blue turn: one bank entry per knob event (one detent), wrapping */
static void bank_browse(volatile int16_t *k, int slot, int16_t diff)
{
    int e = bank_slot == slot ? wh_bank : -1, d = diff > 0 ? 1 : -1;

    if (!diff)
        return;
    e = e < 0 ? (d > 0 ? 0 : BANK - 1) : (e + d + BANK) % BANK;
    bank_load(k, slot, e);
}

int wh_knob(int knob, int16_t *diff)
{
    int slot, i, v;
    volatile int16_t *k;

    if ((unsigned int)knob > 3 || !engine_screen_wheel())
        return 0;
    k = current(&slot);
    if (*TE_SHIFT_HELD) {                       /* drwave's shift + knob: not on a WHEEL sound */
        if (knob == 0)
            bank_browse(k, slot, *diff);
        return 1;
    }
    if (wh_page < 2)
        bank_edited = 1;
    if (wh_page == 2)
        return eh_fx_knob(slot, knob, *diff);
    i = wh_page ? 4 + knob : knob;
    if (i < 4) {
        v = k[i] + ((*diff * (STEP_SCALE >> 1)) >> 15);
        k[i] = (int16_t)(v < 0 ? 0 : v > WH_TE_PAGE1_MAX ? WH_TE_PAGE1_MAX : v);
    } else {
        v = (k[i] & P2_MAX) + ((*diff * (STEP_SCALE >> 3)) >> 15);
        k[i] = (int16_t)(MARK | (v < 0 ? 0 : v > P2_MAX ? P2_MAX : v));
    }
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
    return 1;
}

/* ------------------------------------------------------------------ GUI thread */

static const char *const labels[WH_PARAMS] = {
    "REG", "PERC", "DRIVE", "ROTARY", "BASS", "BODY", "TOP", "CLICK",
};

void wh_frame(uint16_t *buf)
{
    static const uint16_t col[4] = {
        GFX_RGB(0x69, 0x8e, 0xff), GFX_RGB(0x00, 0xed, 0x95),   /* blue, green */
        GFX_RGB(0xe6, 0xe6, 0xe6), GFX_RGB(0xff, 0x6a, 0x3d),   /* white, orange */
    };
    /* drawbar colours as on the organ: sub bars brown, octaves white, the rest black */
    static const uint16_t brown = GFX_RGB(0x8a, 0x4b, 0x2a), ivory = GFX_RGB(0xee, 0xe8, 0xd8),
                          ebony = GFX_RGB(0x50, 0x50, 0x58);
    static const uint16_t dim = GFX_RGB(0x5a, 0x58, 0x66), track = GFX_RGB(0x35, 0x32, 0x38);
    static const uint8_t white_bar[WH_BARS] = { 0, 0, 1, 1, 0, 1, 0, 0, 1 };
    int16_t p[WH_PARAMS];
    volatile int16_t *k;
    int slot, i, b, x, h, pg = wh_page, perc;

    if (!engine_screen_wheel())
        return;
    k = current(&slot);
    unpack(k, p);
    perc = wh_perc(p[WH_PERC]);

    gfx_begin(buf);
    gfx_rect(0, 0, gfx_w, 140, GFX_BLACK);
    x = gfx_text(4, 4, "WHEEL", col[1], 2);
    if (wh_bank >= 0 && bank_slot == slot) {     /* the bank entry this sound came from */
        x = gfx_text(x + 8, 4, bank[(int)wh_bank].name, GFX_WHITE, 2);
        if (bank_edited)
            gfx_text(x, 4, "*", dim, 2);
    } else {
        gfx_text(x + 8, 4, "SHIFT+BLUE: PRESETS", dim, 2);
    }
    gfx_text(272, 4, pg == 2 ? "P3" : pg ? "P2" : "P1", GFX_WHITE, 2);

    /* the nine drawbars, pulled out by their level (8 px a step) */
    for (b = 0; b < WH_BARS; b++) {
        x = 6 + b * 18;
        h = wh_bar(p, b);
        if (perc && b == WH_BARS - 1)
            h = 0;                              /* percussion takes the 1' bar */
        gfx_rect(x + 5, 20, 2, 66, track);
        gfx_rect(x, 20, 12, 2 + h * 8, b < 2 ? brown : white_bar[b] ? ivory : ebony);
    }

    /* the selected settings, in words */
    gfx_text(176, 22, wh_reg_name(wh_reg(p[WH_REG])), GFX_WHITE, 3);
    x = gfx_text(176, 46, "PERC ", dim, 2);
    gfx_text(x, 46, wh_perc_name(perc), col[1], 2);
    x = gfx_text(176, 64, "ROT ", dim, 2);
    gfx_text(x, 64, wh_rot_name(wh_rot(p[WH_ROTARY])), col[3], 2);

    if (pg == 2) {                              /* page 3: the sound's FX knobs (engine host) */
        eh_fx_draw(slot, 90);
        return;
    }

    /* the four knobs of this page, the other page's values dimmed below */
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
