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
 * wh_note_key     on_key observe: the keyboard's held keys (the screen names their notes)
 * wh_frame        WHEEL screen over drwave's on the engine screen (kept above y 140, where
 *                 engine_plus draws its knob row)
 * wh_tick         seq_tick: the clock of the sound's name on the lamps
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
volatile int8_t wh_note_shown = -1;             /* the lowest sounding note shown last frame (bench) */
static volatile uint8_t touched;                /* the page's knob turned last (the lamps show it) */

/* the sounding note (as STATION 1's LCD): a held key is its own note (TE plays note index + 53 +
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
    touched = 0;
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
    touched = (uint8_t)knob;
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

/* Screen (spec: README.md "Screen"): after "CHORD · display overview": the 8 x 2 lamp display (the
 * registration over the knob turned last; the sound's name for 1 s after it is picked or the screen
 * opens), the fan = the rotary speaker (turning at the horn's speed), the grille = the nine
 * drawbars, the six buttons PERC OFF 2ND 3RD and ROTARY OFF SLW FST; below it the page's four knobs:
 * name and bar (as STATION 1). */

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
#define C_GOLD      GFX_RGB(0xe6, 0xc0, 0x50)   /* the frame, lit buttons */
#define C_AMBER     GFX_RGB(0xff, 0xb4, 0x30)   /* the lamps: lit, unlit, the glass */
#define C_AMBER_OFF GFX_RGB(0x30, 0x22, 0x0a)
#define C_AMBER_BG  GFX_RGB(0x12, 0x0c, 0x04)

static const eh_lcd lamps = { 74, 7, 8, 4, 3, 22, 33, C_AMBER, C_AMBER_OFF };

/* sin(5 deg * i) x 100, i = 0 .. 18 */
static const int8_t sn[19] = { 0, 9, 17, 26, 34, 42, 50, 57, 64, 71, 77, 82, 87, 91, 94, 97, 98, 100, 100 };

/* the direction of a (degrees, any) x 100, 0 = up, clockwise */
static void dir(int a, int *dx, int *dy)
{
    int i;

    a %= 360;
    a = a < 0 ? a + 360 : a;
    a = a > 180 ? a - 360 : a;
    i = a < 0 ? -a : a;
    *dx = (i >= 90 ? sn[(180 - i) / 5] : sn[i / 5]) * (a < 0 ? -1 : 1);
    *dy = i >= 90 ? sn[(i - 90) / 5] : -sn[(90 - i) / 5];
}

/* the fan: a white ring, three red blades at angle a (degrees), the hub */
static void fan(int cx, int cy, int a)
{
    int i, j, dx, dy;

    gfx_disc(cx, cy, 14, C_WHITE);
    gfx_disc(cx, cy, 13, GFX_BLACK);
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) {               /* a blade: three discs along it, widening */
            dir(a + i * 120 + j * 8, &dx, &dy);
            gfx_disc(cx + dx * (3 + 3 * j) / 100, cy + dy * (3 + 3 * j) / 100, j ? 2 : 1, C_RED);
        }
    gfx_disc(cx, cy, 2, C_WHITE);
}

/* a button: a ring, lit in c (style 0 full, 1 a small dot, 2 a thick ring) */
static void button(int x, int y, int lit, int style, uint16_t c)
{
    if (!lit || style == 1) {
        gfx_disc(x, y, 5, C_WHITE);
        gfx_disc(x, y, 4, GFX_BLACK);
        if (lit)
            gfx_disc(x, y, 2, c);
    } else {
        gfx_disc(x, y, 5, c);
        if (style == 2)
            gfx_disc(x, y, 2, GFX_BLACK);
    }
}

static int len(const char *s)
{
    int n = 0;

    while (s[n])
        n++;
    return n;
}

static char *put(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    *d = 0;
    return d;
}

static char *put_dec(char *d, int v)
{
    char t[8];
    int n = 0;

    if (v < 0) {
        *d++ = '-';
        v = -v;
    }
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *d++ = t[--n];
    *d = 0;
    return d;
}

static void centred(int row, const char *s)
{
    int n = len(s);

    eh_lcd_text(&lamps, row, n < 8 ? (8 - n) / 2 : 0, s, C_AMBER);
}

/* the lowest sounding note (held keys with their voice's pitch, the notes without a key for FLASH
 * blocks), -1 = none */
static int sounding(void)
{
    uint32_t b = blocks;
    int i, k = -1;

    for (i = 0; i < HELD; i++)
        if (held_id[i] && held_key[i] > 0 && (k < 0 || held_key[i] < k))
            k = held_key[i];
    for (i = 0; i < VOICES; i++)
        if ((int32_t)(flash_until[i] - b) > 0 && flash_key[i] > 0 && (k < 0 || flash_key[i] < k))
            k = flash_key[i];
    return k > 127 ? -1 : k;
}

/* the lamps: the sound's name while the title time runs (a word a line: SOUL / JAZZ, * = edited;
 * P--), else the registration over the knob turned last (PERC 3RD, DRIVE 25, BASS +1; FX on page 3) */
static void display(int slot, int pg, const int16_t *p)
{
    char t[24], v[12], *d;
    const char *s;
    int own = wh_bank >= 0 && bank_slot == slot, q = pg * 4 + touched, i;

    wh_title_on = (int32_t)(title_until - now) > 0;
    eh_lcd_blank(&lamps, 2);
    if (wh_title_on) {
        if (!own) {
            centred(0, "P--");
            return;
        }
        s = bank[(int)wh_bank].name;
        for (i = 0; s[i] && s[i] != ' '; i++)
            t[i] = s[i];
        t[i] = 0;
        centred(0, t);
        d = put(t, s[i] ? s + i + 1 : "");
        if (bank_edited)
            put(d, "*");
        centred(1, t);
        return;
    }
    centred(0, wh_reg_name(wh_reg(p[WH_REG])));
    if (pg == 2) {
        centred(1, "FX");
        return;
    }
    switch (q) {
    case WH_REG:
        put_dec(v, wh_reg(p[q]) + 1);
        break;
    case WH_PERC:
        put(v, wh_perc_name(wh_perc(p[q])));
        break;
    case WH_ROTARY:
        put(v, wh_rot_name(wh_rot(p[q])));
        break;
    case WH_DRIVE:
    case WH_CLICK:
        put_dec(v, p[q] * 100 >> 15);
        break;
    default:                                    /* BASS BODY TOP: -8 .. +8 */
        i = wh_offset(p[q]);
        put_dec(put(v, i > 0 ? "+" : ""), i);
        break;
    }
    d = put(t, q == WH_ROTARY ? "ROT" : labels[q]);
    if (len(t) + 1 + len(v) <= 8)
        put(put(d, " "), v);
    else
        put(t, v);
    centred(1, t);
}

void wh_frame(uint16_t *buf)
{
    static const uint16_t col[4] = { C_BLUE, C_GREEN, C_WHITE, C_ORANGE };
    static const int32_t spins[WH_ROTS] = { 0, 54395, 442368 };   /* horn, Q16 Hz (wheel_core) */
    static const char *const perc_labels[3] = { "OFF", "2ND", "3RD" };
    static const char *const rot_labels[3] = { "OFF", "SLW", "FST" };
    static const char *const nn[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static uint32_t angle;                      /* the fan's angle, 1/16 degree */
    static int32_t spin;                        /* horn speed shown: eases as the rotor does */
    static int8_t shown = -1;                   /* the sound slot the screen showed last frame */
    int16_t p[WH_PARAMS];
    int bar[WH_BARS];
    volatile int16_t *k;
    int slot, i, b, x, pg = wh_page, perc, rot, sel, note;
    char txt[16], *d;
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

    eh_frame(65, 1, 191, 72, 6, 2, C_GOLD, C_AMBER_BG);
    display(slot, pg, p);

    /* the fan = the rotary speaker: turns at the horn's speed (eased like the rotor), its name and
     * the sounding note under it */
    spin += (spins[rot] - spin) >> 4;
    angle += (uint32_t)spin >> 12;
    fan(32, 26, (int)(angle >> 4));
    x = len(wh_rot_name(rot)) * 4 - 1;
    gfx_text(32 - x / 2, 46, wh_rot_name(rot), pg == 0 ? C_ORANGE : C_GREY, 1);
    note = sounding();
    wh_note_shown = (int8_t)note;
    if (note >= 0) {
        d = put(txt, nn[note % 12]);
        put_dec(d, note / 12 - 1);
        x = len(txt) * 4 - 1;
        gfx_disc(30 - x / 2, 58, 2, C_RED);
        gfx_text(34 - x / 2, 56, txt, C_RED, 1);
    }

    /* the grille = the nine drawbars (BASS 16' 5 1/3', BODY 8' 4', TOP 2 2/3' .. 1'), each line
     * lit by its level, the registration's digits under it */
    d = txt;
    for (b = 0; b < WH_BARS; b++) {
        int g = b < 2 ? 0 : b < 4 ? 1 : 2;

        c = pg == 0 ? C_BLUE : pg == 1 ? col[g] : C_GREY;
        gfx_rect(266, 6 + b * 6, 46, 1, C_LINE);
        gfx_rect(266, 6 + b * 6, bar[b] * 46 / 8, 1, c);
        if (b == 2 || b == 4)
            *d++ = ' ';
        *d++ = (char)('0' + bar[b]);
    }
    *d = 0;
    gfx_text(267, 60, txt, C_GREY, 1);

    /* the buttons: PERC OFF 2ND 3RD (lit full = normal, a dot = soft, a ring = slow), ROTARY OFF
     * SLW FST */
    sel = perc ? (perc - 1) % 2 + 1 : 0;
    gfx_text(84, 78, "PERC", C_GREY, 1);
    gfx_text(221, 78, "ROTARY", C_GREY, 1);
    for (i = 0; i < 3; i++) {
        x = 112 + i * 18;
        c = pg == 0 ? C_GREEN : C_GOLD;
        button(x, 80, sel == i, perc ? (perc - 1) / 2 : 0, c);
        gfx_text(x - 5, 88, perc_labels[i], sel == i ? c : C_GREY, 1);
        x = 172 + i * 18;
        c = pg == 0 ? C_ORANGE : C_GOLD;
        button(x, 80, rot == i, 0, c);
        gfx_text(x - 5, 88, rot_labels[i], rot == i ? c : C_GREY, 1);
    }

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
