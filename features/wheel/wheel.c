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
 * wh_note_key     on_key observe: the keyboard's held keys (the screen lights their notes)
 * wh_frame        WHEEL screen over drwave's on the engine screen (kept above y 140, where
 *                 engine_plus draws its knob row)
 * wh_tick         seq_tick: the clock of the sound's name over the organ
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
#define HELD            8
#define FLASH           100                     /* audio blocks: ~145 ms */
#define TITLE_TIME      TE_SAMPLE_RATE          /* the sound's name over the organ: 1 s */

static wh_voice voices[VOICES];
static wh_rotor rotor;
volatile uint8_t wh_page;                       /* 0..2 = page 1..3 (bench reads) */
#define PAGES           3
static volatile uint8_t consumed;               /* pushes whose key-up we eat */
volatile int8_t wh_bank = -1;                   /* last loaded bank entry (-1 none), bench reads */
static volatile uint8_t bank_edited;            /* knobs changed after loading it */
static volatile int8_t bank_slot = -1;          /* sound it was loaded into */
static volatile uint32_t now;                   /* samples, advanced by wh_tick */
static volatile uint32_t title_until;           /* the sound's name shown while now < this */
volatile uint8_t wh_title_on;                   /* drawn in the last frame (bench reads) */

/* the screen's keyboard (as STATION 1): a held key lights its own note (TE plays note index + 53 +
 * 12 x octave), the octave from the voice it starts (rounded: TE's glide / LFO / bend at the start
 * move it by less than 6 semitones); new voices no held key accounts for (sequencer, MIDI, CHORD+'s
 * extra notes) flash at their pitch for FLASH blocks */
static volatile uint32_t blocks;                /* audio blocks (render thread) */
static volatile int8_t held_id[HELD];           /* key id held, 0 = free (KeyHandler thread) */
static volatile int8_t held_note[HELD];         /* its note index 0..23 */
static volatile int8_t held_key[HELD];          /* its MIDI key, -1 = no voice yet (render thread) */
static volatile uint8_t held_seq[HELD];
static volatile uint8_t press_seq;
static volatile int8_t flash_key[VOICES];
static volatile uint32_t flash_until[VOICES];

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

/* MIDI key of an 8' phase increment (A4 = 21426141 at 88.2 kHz): octave, then the nearest
 * semitone (bounds a quarter tone below each, from A) */
static int inc_key(uint32_t inc)
{
    static const uint32_t lo[12] = {
        20816180, 22053975, 23365372, 24754750, 26226744, 27786267,
        29438525, 31189030, 33043627, 35008503, 37090217, 39295716,
    };
    int k = 69, s;

    if (!inc)
        return 0;
    for (; inc < lo[0]; inc <<= 1)
        k -= 12;
    for (; inc >= 2 * lo[0]; inc >>= 1)
        k += 12;
    for (s = 1; s < 12 && inc >= lo[s]; s++)
        k++;
    return k;
}

/* engine host: one voice block (the host checked the tag) */
static void wh_eh_render(int vi, int16_t *out, const int16_t *knobs, const uint32_t *inc, int flags)
{
    int16_t p[WH_PARAMS];
    int i, oldest = -1;

    unpack(knobs, p);
    if (flags & EH_BLOCK) {
        wh_rotor_block(&rotor, p, TE_VOICE_SAMPLES);
        blocks++;
    }
    if (flags & EH_FRESH) {                     /* a new note: pair it with its key (screen) */
        int key = inc_key(inc[0]);

        wh_note_on(&voices[vi]);
        for (i = 0; i < HELD; i++)
            if (held_id[i] && held_key[i] < 0
                && (oldest < 0 || (uint8_t)(held_seq[i] - held_seq[oldest]) > 127))
                oldest = i;
        if (oldest >= 0) {
            int n = held_note[oldest] + 53, d = key - n;

            d = d >= 0 ? (d + 6) / 12 : -((5 - d) / 12);    /* octaves, rounded */
            n += 12 * d;
            held_key[oldest] = (int8_t)(n < 0 ? 0 : n > 127 ? 127 : n);
        } else {                                /* no key: sequencer / MIDI */
            flash_key[vi] = (int8_t)(key > 127 ? 127 : key);
            flash_until[vi] = blocks + FLASH;
        }
    }
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

/* on_key observe (sees every key first): the keyboard's keys, held or released */
void wh_note_key(int key, int down)
{
    static const int8_t note_ids[24] = TE_NOTE_KEY_IDS;
    int i, note, free = -1;

    for (note = 0; note < 24 && note_ids[note] != key; note++)
        ;
    if (note == 24)
        return;
    for (i = 0; i < HELD; i++) {
        if (held_id[i] == key) {                /* up (or a repeated down): this press is over */
            held_id[i] = 0;
            held_key[i] = -1;
        }
        if (!held_id[i] && free < 0)
            free = i;
    }
    if (down && free >= 0) {
        held_key[free] = -1;
        held_note[free] = (int8_t)note;
        held_seq[free] = press_seq++;
        held_id[free] = (int8_t)key;
    }
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

/* how many values a parameter has (its zones), 0 = continuous (DRIVE, CLICK) */
static int values(int id)
{
    static const uint8_t n[WH_PARAMS] = { WH_REGS, WH_PERCS, 0, WH_ROTS, 17, 17, 17, 0 };

    return n[id];
}

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
    title_until = now + TITLE_TIME;
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
    if (values(i)) {                            /* a value per detent, more when fast */
        int n = values(i), step = (*diff < 0 ? -*diff : *diff) >> 10;
        int z = (i < 4 ? k[i] << 1 : (k[i] & P2_MAX) << 3) * n >> 15;

        step = step < 1 ? 1 : step;
        z += *diff > 0 ? step : -step;
        z = z < 0 ? 0 : z >= n ? n - 1 : z;
        k[i] = i < 4 ? zone1(z, n) : (int16_t)(MARK | (zone1(z, n) >> 2));
    } else if (i < 4) {
        v = k[i] + ((*diff * (STEP_SCALE >> 1)) >> 15);
        k[i] = (int16_t)(v < 0 ? 0 : v > WH_TE_PAGE1_MAX ? WH_TE_PAGE1_MAX : v);
    } else {
        v = (k[i] & P2_MAX) + ((*diff * (STEP_SCALE >> 3)) >> 15);
        k[i] = (int16_t)(MARK | (v < 0 ? 0 : v > P2_MAX ? P2_MAX : v));
    }
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
    return 1;
}

/* seq_tick (Seq thread, once per 64-frame audio block): the clock of the name over the organ */
void wh_tick(void)
{
    now += TE_AUDIO_FRAMES;
}

/* ------------------------------------------------------------------ GUI thread */

/* Screen (spec: README.md "Screen"): the organ after "wheel · overview" (as MONO 3): the rotor's
 * air above the cabinet (moves with the horn's speed), the cabinet's grille = the nine drawbars
 * in their groups BASS / BODY / TOP, the drive pill, the registration's digits and the page, the
 * PERC and ROTARY buttons, the keyboard (the sounding keys lit); for 1 s after a sound is picked (or
 * the screen opens) its name over the organ, as STATION 1's LCD; below it the page's four knobs:
 * name and bar (as STATION 1). The current page's controls are in their knob colours. */

static const char *const labels[WH_PARAMS] = {
    "REG", "PERC", "DRIVE", "ROTARY", "BASS", "BODY", "TOP", "CLICK",
};
static const char *const page_names[PAGES] = { "ORGAN", "DRAWBARS", "FX" };

#define C_BLUE      GFX_RGB(0x6e, 0x8b, 0xff)
#define C_GREEN     GFX_RGB(0x3d, 0xdc, 0x97)
#define C_WHITE     GFX_RGB(0xe6, 0xe6, 0xe6)
#define C_ORANGE    GFX_RGB(0xff, 0x6a, 0x3d)
#define C_RED       GFX_RGB(0xff, 0x4f, 0x5e)
#define C_GREY      GFX_RGB(0x8a, 0x8a, 0x99)
#define C_DIM       GFX_RGB(0x5a, 0x58, 0x66)
#define C_LINE      GFX_RGB(0x48, 0x46, 0x50)
#define C_TRACK     GFX_RGB(0x35, 0x32, 0x38)

#define KEY_LO      48                          /* keyboard C3 .. C5, 15 white keys */
#define KEYS        25
#define KEY_X       105
#define KEY_W       12

static void outline(int x, int y, int w, int h, int r, uint16_t c)
{
    int i;

    gfx_rect(x + r, y, w - 2 * r, 1, c);
    gfx_rect(x + r, y + h - 1, w - 2 * r, 1, c);
    gfx_rect(x, y + r, 1, h - 2 * r, c);
    gfx_rect(x + w - 1, y + r, 1, h - 2 * r, c);
    for (i = 1; i < r; i++) {
        gfx_pixel(x + r - i, y + i, c);
        gfx_pixel(x + w - 1 - r + i, y + i, c);
        gfx_pixel(x + r - i, y + h - 1 - i, c);
        gfx_pixel(x + w - 1 - r + i, y + h - 1 - i, c);
    }
}

static void light(uint8_t *on, int k)          /* folded into C3 .. C5 */
{
    if (k <= 0)
        return;
    while (k < KEY_LO)
        k += 12;
    while (k >= KEY_LO + KEYS)
        k -= 12;
    on[k - KEY_LO] = 1;
}

/* the keys that sound: held ones (their own note), the notes without a key for FLASH blocks */
static void keyboard(int y0, int y1)
{
    static const int8_t white_of[12] = { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6 };
    uint8_t on[KEYS];
    uint32_t now = blocks;
    int i, split = y0 + (y1 - y0) * 3 / 5;

    for (i = 0; i < KEYS; i++)
        on[i] = 0;
    for (i = 0; i < HELD; i++)
        if (held_id[i])
            light(on, held_key[i]);
    for (i = 0; i < VOICES; i++)
        if ((int32_t)(flash_until[i] - now) > 0)
            light(on, flash_key[i]);

    outline(KEY_X, y0, 15 * KEY_W + 1, y1 - y0 + 1, 0, C_WHITE);
    for (i = 1; i < 15; i++)
        gfx_rect(KEY_X + i * KEY_W, y0 + 1, 1, y1 - y0 - 1, C_LINE);
    for (i = 0; i < KEYS; i++) {
        int o = i / 12, n = i % 12, w = white_of[n];

        if (w >= 0) {
            if (on[i])
                gfx_rect(KEY_X + (o * 7 + w) * KEY_W + 1, split + 1, KEY_W - 1, y1 - split - 2, C_GREEN);
        } else {
            int x = KEY_X + (o * 7 + white_of[n - 1] + 1) * KEY_W - 3;

            gfx_rect(x, y0 + 1, 7, split - y0 - 1, on[i] ? C_GREEN : C_WHITE);
        }
    }
}

/* a radio button: ring, or lit in c (style 0 full, 1 a small dot, 2 a thick ring) */
static void button(int x, int y, int lit, int style, uint16_t c)
{
    if (!lit || style == 1) {
        gfx_disc(x, y, 4, C_WHITE);
        gfx_disc(x, y, 3, GFX_BLACK);
        if (lit)
            gfx_disc(x, y, 2, c);
    } else {
        gfx_disc(x, y, 4, c);
        if (style == 2)
            gfx_disc(x, y, 2, GFX_BLACK);
    }
}

/* the sound's name over the organ (STATION 1's LCD: its place, size and colour) while the title
 * time runs: P01 .. P24 and the name (* = edited), or P-- and how to browse */
static void title(int slot)
{
    char t[24];
    const char *s;
    int own = wh_bank >= 0 && bank_slot == slot, n = 0, w;

    wh_title_on = (int32_t)(title_until - now) > 0;
    if (!wh_title_on)
        return;
    t[n++] = 'P';
    if (own) {
        t[n++] = (char)('0' + (wh_bank + 1) / 10);
        t[n++] = (char)('0' + (wh_bank + 1) % 10);
        t[n++] = ' ';
        for (s = bank[(int)wh_bank].name; *s; s++)
            t[n++] = *s;
        if (bank_edited)
            t[n++] = '*';
    } else {
        t[n++] = '-';
        t[n++] = '-';
    }
    t[n] = 0;
    w = n * 8 - 2;                              /* scale 2: 8 px a character */
    if (!own && w < 71)                         /* SHIFT+BLUE: SOUNDS */
        w = 71;
    gfx_rect(74, 7, w + 10, own ? 18 : 26, GFX_BLACK);
    outline(74, 7, w + 10, own ? 18 : 26, 2, C_GREEN);
    gfx_text(79, 11, t, C_GREEN, 2);
    if (!own)
        gfx_text(79, 24, "SHIFT+BLUE: SOUNDS", C_GREEN, 1);
}

void wh_frame(uint16_t *buf)
{
    static const uint16_t col[4] = { C_BLUE, C_GREEN, C_WHITE, C_ORANGE };
    static const int32_t spins[WH_ROTS] = { 0, 54395, 442368 };   /* horn, Q16 Hz (wheel_core) */
    static const uint8_t digit_x[WH_BARS] = { 112, 126, 148, 162, 184, 198, 212, 226, 240 };
    static const char *const perc_labels[3] = { "OFF", "2ND", "3RD" };
    static const char *const rot_labels[3] = { "OFF", "SLW", "FST" };
    static uint32_t air;                        /* the air's rise, 1/16 px */
    static int32_t spin;                        /* horn speed shown: eases as the rotor does */
    static int8_t shown = -1;                   /* the sound slot the screen showed last frame */
    int16_t p[WH_PARAMS];
    int bar[WH_BARS];
    volatile int16_t *k;
    int slot, i, b, x, y, pg = wh_page, perc, rot, sel;
    uint16_t c;

    if (!engine_screen_wheel()) {
        shown = -1;
        wh_title_on = 0;
        return;
    }
    k = current(&slot);
    if (slot != shown) {                        /* the screen opens or shows another sound: its name */
        shown = (int8_t)slot;
        title_until = now + TITLE_TIME;
    }
    unpack(k, p);
    perc = wh_perc(p[WH_PERC]);
    rot = wh_rot(p[WH_ROTARY]);
    for (b = 0; b < WH_BARS; b++)
        bar[b] = wh_bar(p, b);
    if (perc)
        bar[WH_BARS - 1] = 0;                   /* percussion takes the 1' bar */

    gfx_begin(buf);
    gfx_rect(0, 0, gfx_w, 140, GFX_BLACK);

    /* the rotor's air above the cabinet: three dashed zigzags rising with the horn's speed */
    spin += (spins[rot] - spin) >> 4;
    air += (uint32_t)spin >> 14;
    c = spin < 4096 ? C_LINE : pg == 0 ? C_ORANGE : C_RED;
    for (i = 0; i < 3; i++)
        for (y = 0; y < 10; y++) {
            int t = y + (int)(air >> 4) + i * 5;

            if (t % 4 != 3)
                gfx_pixel(150 + i * 10 + ((t >> 2) & 1), 1 + y, c);
        }

    /* the cabinet: its grille = the nine drawbars (BASS 16' 5 1/3', BODY 8' 4', TOP 2 2/3' .. 1'),
     * each pulled out by its level */
    outline(42, 12, 236, 23, 2, C_WHITE);
    gfx_rect(47, 15, 226, 1, C_LINE);
    for (b = 0; b < WH_BARS; b++) {
        int g = b < 2 ? 0 : b < 4 ? 1 : 2;

        x = 52 + g * 77;
        y = g < 2 ? 22 + (b & 1) * 6 : 19 + (b - 4) * 3;
        c = pg == 0 ? C_BLUE : pg == 1 ? col[g] : C_GREY;
        gfx_rect(x, y, 62, 1, C_LINE);
        gfx_rect(x, y, bar[b] * 62 / 8, 1, c);
    }

    /* the console: drive pill, the registration's digits + the page, buttons, keyboard */
    outline(30, 35, 261, 52, 2, C_WHITE);
    outline(35, 38, 66, 8, 0, C_GREY);
    outline(52, 40, 32, 4, 2, C_WHITE);
    gfx_rect(54, 41, p[WH_DRIVE] * 28 / 32767, 2, pg == 0 ? C_WHITE : C_GREY);
    outline(KEY_X, 38, 15 * KEY_W + 1, 8, 0, C_GREY);
    for (b = 0; b < WH_BARS; b++) {
        int g = b < 2 ? 0 : b < 4 ? 1 : 2;

        gfx_dec(digit_x[b], 40, bar[b], pg == 0 ? C_BLUE : pg == 1 ? col[g] : C_GREY, 1);
        if (b == 0 || b == 2 || (b >= 4 && b < WH_BARS - 1))    /* inside a group */
            gfx_rect((digit_x[b] + 3 + digit_x[b + 1]) / 2 - 2, 39, 4, 5, C_WHITE);
    }
    x = gfx_text(270, 40, "P", C_RED, 1);
    gfx_dec(x, 40, pg + 1, C_RED, 1);

    outline(35, 49, 66, 35, 2, C_GREY);
    sel = perc ? (perc - 1) % 2 + 1 : 0;        /* OFF 2ND 3RD; normal / soft / slow */
    for (i = 0; i < 3; i++) {
        x = 50 + i * 18;
        c = pg == 0 ? C_GREEN : C_WHITE;
        gfx_text(x - 5, 51, perc_labels[i], sel == i ? c : C_GREY, 1);
        button(x, 61, sel == i, perc ? (perc - 1) / 2 : 0, c);
        c = pg == 0 ? C_ORANGE : C_WHITE;
        button(x, 71, rot == i, 0, c);
        gfx_text(x - 5, 78, rot_labels[i], rot == i ? c : C_GREY, 1);
    }
    keyboard(50, 86);                           /* on the case's bottom edge */
    title(slot);

    /* the page's four knobs: name and bar (as STATION 1) */
    if (pg == 2) {                              /* page 3: the sound's FX knobs (engine host) */
        eh_fx_draw(slot, 97);
        return;
    }
    for (i = 0; i < 4; i++) {
        int id = pg * 4 + i;

        x = 4 + i * 80;
        gfx_text(x, 99, labels[id], col[i], 2);
        gfx_rect(x, 112, 72, 6, C_TRACK);
        b = values(id);                         /* a value per step: empty .. full */
        gfx_rect(x, 112, b ? (p[id] * b >> 15) * 72 / (b - 1) : p[id] * 72 / 32767, 6, col[i]);
    }
    x = gfx_text(4, 125, "PUSH: ", C_DIM, 1);
    gfx_text(x, 125, page_names[(pg + 1) % PAGES], C_DIM, 1);
}
