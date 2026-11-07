/* MALLET synth engine (spec: README.md): struck bars, pans and bells hosted on TE's drwave engine.
 *
 * Runs on the engine host (features/enginehost, tag 7): a drwave sound is MALLET when knob slots
 * 5-8 all hold 0x7000 | v (v = 0..4095, page 2). Page 1 lives in slots 1-4 as 0..16383. Every value
 * is inside drwave's own ranges, so a MALLET sound passes TE's boot-time patch validator unchanged
 * and loads on stock firmware as a drwave sound.
 *
 * ml_init         registers the voice renderer with the engine host (tag 7)
 * ml_engine_name  engine browser (shared point engine_name): the "mallet" row becomes a drwave
 *                 sound with MALLET's defaults
 * ml_key          on_key: synth engine screen, MALLET sound: knob push cycles page 1 / 2 / 3
 * ml_knob         on_knob: synth engine screen, MALLET sound: pages 1-2 edit MALLET's 8 values and
 *                 save them through TE (TE_PATCH_SET_KNOBS); page 3 edits the sound's FX knobs 1-4
 *                 (engine host); drwave's knobs never see them. Shift + blue turn browses MALLET's
 *                 built-in bank (16 sounds): loads all 8 values
 * ml_note_key     on_key observe: the keyboard's held keys (the screen lights their bars)
 * ml_frame        MALLET screen over drwave's on the engine screen (kept above y 140, where
 *                 engine_plus draws its knob row)
 * ml_tick         seq_tick: the clock of the sound's name over the instrument
 * TE's envelope, velocity, volume, FX and LFO apply as for any engine. */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "mallet_core.h"
#include "../enginehost/eh_host.h"

#define KEY_KNOB_PUSH   59                      /* blue 59, green 60, white 61, orange 62 */
#define STEP_SCALE      10485                   /* ~100 clicks full range (as engine_plus) */
#define VOICES          EH_VOICES
#define TAG             7
#define MARK            (TAG << 12)
#define P2_MAX          0x0fff
#define PAGE1_MAX       16383                   /* drwave slots 1-4: 0..24575 / 16383 / 16383 / 32767 */
#define HELD            8
#define FLASH           100                     /* audio blocks: ~145 ms */
#define TITLE_TIME      TE_SAMPLE_RATE          /* the sound's name over the instrument: 1 s */
#define PAGES           3

static ml_voice voices[VOICES];
static uint32_t motor_ph;                       /* the vibraphone motor (render thread) */
static int32_t motor_gain = 32768;
volatile uint8_t ml_page;                       /* 0..2 = page 1..3 (bench reads) */
static volatile uint8_t consumed;               /* pushes whose key-up we eat */
volatile int8_t ml_bank = -1;                   /* last loaded bank entry (-1 none), bench reads */
static volatile uint8_t bank_edited;            /* knobs changed after loading it */
static volatile int8_t bank_slot = -1;          /* sound it was loaded into */
static volatile uint32_t now;                   /* samples, advanced by ml_tick */
static volatile uint32_t title_until;           /* the sound's name shown while now < this */
volatile uint8_t ml_title_on;                   /* drawn in the last frame (bench reads) */

/* the screen's bars (as STATION 1's keys): a held key lights its own note (TE plays note index +
 * 53 + 12 x octave), the octave from the voice it starts (rounded); voices no held key accounts for
 * (sequencer, MIDI, CHORD+) flash at their pitch for FLASH blocks */
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
        v = v < 0 ? 0 : v > PAGE1_MAX ? PAGE1_MAX : v;
        p[i] = (int16_t)(v << 1);
        p[4 + i] = (int16_t)((k[4 + i] & P2_MAX) << 3);
    }
}

/* MIDI key of a phase increment (A4 = 21426141 at 88.2 kHz): octave, then the nearest semitone
 * (bounds a quarter tone below each, from A) */
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
static void ml_eh_render(int vi, int16_t *out, const int16_t *knobs, const uint32_t *inc, int flags)
{
    int16_t p[ML_PARAMS];
    int i, oldest = -1;

    unpack(knobs, p);
    if (flags & EH_BLOCK) {
        motor_gain = ml_motor(&motor_ph, p, TE_VOICE_SAMPLES);
        blocks++;
    }
    if (flags & EH_FRESH) {                     /* a new note: pair it with its key (screen) */
        int key = inc_key(inc[0]);

        ml_note_on(&voices[vi]);
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
    ml_render(&voices[vi], out, TE_VOICE_SAMPLES, inc, p, motor_gain);
}

void ml_init(void)
{
    eh_register(TAG, ml_eh_render);
}

/* Engine browser row "mallet" (content_sql adds it to TE's synth_types with MALLET's defaults as
 * its default_params): right after TE reads the picked row's type name, "mallet" becomes "drwave"
 * (same length), so the sound is a drwave sound with MALLET's marker. */
void ml_engine_name(te_string *out, void *db, const char *sql)
{
    static const char n[] = "mallet", host[] = "drwave";
    char *d = (char *)te_str_data(out);
    unsigned int i;

    (void)db;
    (void)sql;
    if (out->size != sizeof(n) - 1)
        return;
    for (i = 0; i < sizeof(n) - 1; i++)
        if (d[i] != n[i])
            return;
    for (i = 0; i < sizeof(host) - 1; i++)
        d[i] = host[i];
    ml_page = 0;
    bank_slot = -1;
}

/* ------------------------------------------------------------------ KeyHandler thread */

static volatile int16_t *current(int *slot)
{
    *slot = *TE_SYNTH_SLOT & 7;
    return TE_SYNTH_KNOBS + *slot * 8;
}

static int engine_screen_mallet(void)
{
    int slot;

    return *TE_EDIT_MODE == 0 && *TE_SYNTH_SUBSCREEN == 0
           && TE_SYNTH_TYPE[*TE_SYNTH_SLOT & 7] == TE_INSTR_DRWAVE && marked(current(&slot));
}

/* on_key observe (sees every key first): the keyboard's keys, held or released */
void ml_note_key(int key, int down)
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

int ml_key(int key, int down)
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
    if (*TE_SHIFT_HELD || !engine_screen_mallet())
        return 0;
    ml_page = (uint8_t)(ml_page + 1 < PAGES ? ml_page + 1 : 0);
    consumed |= (uint8_t)(1 << b);
    return 1;
}

/* built-in bank: instrument, then MALLET STRIKE MOTOR TUBE TONE in %, DECAY and DETUNE -100..100
 * (0 = the instrument's own) */
typedef struct {
    const char *name;
    int8_t instr, mallet, decay, strike, motor, tube, detune, tone;
} bank_entry;

static const bank_entry bank[] = {
    { "MARIMBA",      ML_MARIMBA,  45,  0,   50, 0,  50, 0,   85 },
    { "SOFT MARIMBA", ML_MARIMBA,  15,  10,  40, 0,  70, 0,   70 },
    { "BASS MARIMBA", ML_MARIMBA,  30,  30,  45, 0,  90, 0,   60 },
    { "VIBES",        ML_VIBES,    40,  0,   45, 0,  40, 0,   80 },
    { "VIBES MOTOR",  ML_VIBES,    40,  0,   45, 45, 60, 0,   80 },
    { "DARK VIBES",   ML_VIBES,    15,  20,  40, 25, 50, 0,   45 },
    { "XYLOPHONE",    ML_XYLO,     85,  0,   60, 0,  20, 0,   100 },
    { "WOOD BLOCK",   ML_XYLO,     100, -70, 80, 0,  0,  0,   90 },
    { "GLOCKENSPIEL", ML_GLOCK,    80,  0,   55, 0,  0,  0,   100 },
    { "MUSIC BOX",    ML_GLOCK,    95,  -30, 70, 0,  0,  10,  90 },
    { "STEELPAN",     ML_STEELPAN, 40,  0,   50, 0,  0,  0,   90 },
    { "TENOR PAN",    ML_STEELPAN, 25,  20,  45, 0,  20, -10, 75 },
    { "HANDPAN",      ML_HANDPAN,  10,  0,   40, 0,  30, 0,   80 },
    { "KALIMBA",      ML_KALIMBA,  70,  0,   60, 0,  0,  0,   90 },
    { "TUBULAR BELL", ML_TUBULAR,  75,  0,   60, 0,  0,  0,   100 },
    { "CHURCH BELL",  ML_TUBULAR,  60,  60,  55, 0,  0,  20,  85 },
};
#define BANK            ((int)(sizeof(bank) / sizeof(bank[0])))

/* centre of zone z of n, stored as a page-1 value (0..16383) */
static int16_t zone1(int z, int n) { return (int16_t)(((2 * z + 1) * 32768 / (2 * n)) >> 1); }

/* how many values a parameter has (its zones), 0 = continuous */
static int values(int id)
{
    return id == ML_INSTR ? ML_INSTRS : 0;
}

static int16_t pct1(int v) { return (int16_t)(v * PAGE1_MAX / 100); }                  /* page 1 */
static int16_t bip1(int v) { return (int16_t)(8192 + v * 8191 / 100); }                /* page 1, -100..100 */
static int16_t pct2(int v) { return (int16_t)(MARK | (v * P2_MAX / 100)); }           /* page 2 */
static int16_t bip2(int v) { return (int16_t)(MARK | (2048 + v * 2047 / 100)); }      /* page 2, -100..100 */

static void bank_load(volatile int16_t *k, int slot, int e)
{
    const bank_entry *b = &bank[e];

    k[ML_INSTR] = zone1(b->instr, ML_INSTRS);
    k[ML_MALLET] = pct1(b->mallet);
    k[ML_DECAY] = bip1(b->decay);
    k[ML_STRIKE] = pct1(b->strike);
    k[ML_MOTOR] = pct2(b->motor);
    k[ML_TUBE] = pct2(b->tube);
    k[ML_DETUNE] = bip2(b->detune);
    k[ML_TONE] = pct2(b->tone);
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
    ml_bank = (int8_t)e;
    bank_slot = (int8_t)slot;
    bank_edited = 0;
    title_until = now + TITLE_TIME;
}

/* shift + blue turn: one bank entry per knob event (one detent), wrapping */
static void bank_browse(volatile int16_t *k, int slot, int16_t diff)
{
    int e = bank_slot == slot ? ml_bank : -1, d = diff > 0 ? 1 : -1;

    if (!diff)
        return;
    e = e < 0 ? (d > 0 ? 0 : BANK - 1) : (e + d + BANK) % BANK;
    bank_load(k, slot, e);
}

int ml_knob(int knob, int16_t *diff)
{
    int slot, i, v;
    volatile int16_t *k;

    if ((unsigned int)knob > 3 || !engine_screen_mallet())
        return 0;
    k = current(&slot);
    if (*TE_SHIFT_HELD) {                       /* drwave's shift + knob: not on a MALLET sound */
        if (knob == 0)
            bank_browse(k, slot, *diff);
        return 1;
    }
    if (ml_page < 2)
        bank_edited = 1;
    if (ml_page == 2)
        return eh_fx_knob(slot, knob, *diff);
    i = ml_page ? 4 + knob : knob;
    if (values(i)) {                            /* a value per detent, more when fast */
        int n = values(i), step = (*diff < 0 ? -*diff : *diff) >> 10;
        int z = (i < 4 ? k[i] << 1 : (k[i] & P2_MAX) << 3) * n >> 15;

        step = step < 1 ? 1 : step;
        z += *diff > 0 ? step : -step;
        z = z < 0 ? 0 : z >= n ? n - 1 : z;
        k[i] = i < 4 ? zone1(z, n) : (int16_t)(MARK | (zone1(z, n) >> 2));
    } else if (i < 4) {
        v = k[i] + ((*diff * (STEP_SCALE >> 1)) >> 15);
        k[i] = (int16_t)(v < 0 ? 0 : v > PAGE1_MAX ? PAGE1_MAX : v);
    } else {
        v = (k[i] & P2_MAX) + ((*diff * (STEP_SCALE >> 3)) >> 15);
        k[i] = (int16_t)(MARK | (v < 0 ? 0 : v > P2_MAX ? P2_MAX : v));
    }
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
    return 1;
}

/* seq_tick (Seq thread, once per 64-frame audio block): the clock of the name over the instrument */
void ml_tick(void)
{
    now += TE_AUDIO_FRAMES;
}

/* ------------------------------------------------------------------ GUI thread */

/* Screen (spec: README.md "Screen"), after the "MALLET · <instrument>" art (1280 x 640 = 4 x the
 * screen), drawn from antialiased lines, ellipses and rounded boxes (no bitmaps), without the
 * art's header: the instrument with the sounding notes lit and the mallet on the newest one,
 * centred above the knob row; for 1 s after a sound is picked (or the screen opens) its name over
 * the instrument, as STATION 1's LCD; below it the page's four knobs: name and bar (as STATION 1).
 * Bars (marimba, vibraphone, glockenspiel), pans (steelpan, handpan) and the rest share their
 * drawing code. */

static const char *const labels[ML_PARAMS] = {
    "INSTR", "MALLET", "DECAY", "STRIKE", "MOTOR", "TUBE", "DETUNE", "TONE",
};
static const char *const page_names[PAGES] = { "MALLET", "BAR", "FX" };

#define C_BLUE      GFX_RGB(0x6e, 0x8b, 0xff)
#define C_GREEN     GFX_RGB(0x3d, 0xdc, 0x97)
#define C_WHITE     GFX_RGB(0xe6, 0xe6, 0xe6)
#define C_ORANGE    GFX_RGB(0xff, 0x6a, 0x3d)
#define C_RED       GFX_RGB(0xff, 0x4f, 0x5e)
#define C_GREY      GFX_RGB(0x8a, 0x8a, 0x99)
#define C_DIM       GFX_RGB(0x5a, 0x58, 0x66)
#define C_LINE      GFX_RGB(0x48, 0x46, 0x50)
#define C_TRACK     GFX_RGB(0x35, 0x32, 0x38)
#define C_PURPLE    GFX_RGB(0xa7, 0x8b, 0xfa)
#define C_TEAL      GFX_RGB(0x2d, 0xd4, 0xcf)
#define C_YELLOW    GFX_RGB(0xe2, 0xc2, 0x5a)

#define KEY_LO      48                          /* notes from C3: 25 (bars) or 29 (kalimba, to E5) */
#define KEYS_MAX    29

/* everything an instrument draws goes through these: oy moves the drawing up or down (each
 * instrument centred between the screen's top and the knob row) */
static int oy;
static void rect(int x, int y, int w, int h, uint16_t c) { gfx_rect(x, y + oy, w, h, c); }
static void pixel(int x, int y, uint16_t c) { gfx_pixel(x, y + oy, c); }

/* the shapes: antialiased (engine host, eh_aa.c), oy as eh_aa_dy */
static int iabs(int v) { return v < 0 ? -v : v; }
static void seg(int x0, int y0, int x1, int y1, int hw, uint16_t c) { eh_aa_line(x0, y0, x1, y1, hw, c); }
static void plot(int x, int y, uint16_t c, int a) { eh_aa_plot(x, y, c, a); }
static void frame(int x, int y, int w, int h, int r, int t, uint16_t c) { eh_aa_frame(x, y, w, h, r, t, c); }
static void rbox(int x, int y, int w, int h, int r, uint16_t c) { eh_aa_box(x, y, w, h, r, c); }

static void ell(int cx, int cy, int a, int b, int hw, int s0, int s1, int dash, uint16_t c)
{
    eh_aa_ellipse(cx, cy, a, b, hw, s0, s1, dash, c);
}

/* the shapes in whole pixels: 1 px lines / outlines, 2 px (thick, ring2), filled (disc, blob) */
static void outline(int x, int y, int w, int h, int r, uint16_t c) { frame(x, y, w, h, r, 1, c); }
static void pill(int x, int y, int w, int h, uint16_t c) { frame(x, y, w, h, (w - 1) / 2, 1, c); }
static void line(int x0, int y0, int x1, int y1, uint16_t c)
{
    seg(x0 * 16, y0 * 16, x1 * 16, y1 * 16, 8, c);
}

static void thick(int x0, int y0, int x1, int y1, uint16_t c)
{
    int ox = iabs(y1 - y0) > iabs(x1 - x0) ? 8 : 0;    /* 2 px: below a flat line, right of a steep one */

    seg(x0 * 16 + ox, y0 * 16 + 8 - ox, x1 * 16 + ox, y1 * 16 + 8 - ox, 16, c);
}

static void arc(int cx, int cy, int rx, int ry, int a0, int a1, int dash, uint16_t c)
{
    ell(cx * 16, cy * 16, rx * 16, ry * 16, 8, a0, a1, dash, c);
}

static void ring(int cx, int cy, int rx, int ry, uint16_t c) { arc(cx, cy, rx, ry, 0, 128, 0, c); }
static void ring2(int cx, int cy, int rx, int ry, uint16_t c)
{
    ell(cx * 16, cy * 16, rx * 16 - 8, ry * 16 - 8, 16, 0, 128, 0, c);
}
static void blob(int cx, int cy, int rx, int ry, uint16_t c)
{
    ell(cx * 16, cy * 16, rx * 16 + 8, ry * 16 + 8, -1, 0, 128, 0, c);
}
static void disc(int x, int y, int r, uint16_t c) { blob(x, y, r, r, c); }

/* sin(90 deg * i / 32) x 256: a turn is 128 steps, 0 = right, 32 = down, 64 = left, 96 = up */
static const uint8_t qs[33] = {
    0, 13, 25, 38, 50, 62, 74, 86, 98, 109, 121, 132, 142, 152, 162, 172, 181, 190, 198, 206, 213,
    220, 226, 231, 237, 241, 245, 248, 251, 253, 255, 255, 255,
};

static int sin128(int a)
{
    a &= 127;
    return a < 32 ? qs[a] : a < 64 ? qs[64 - a] : a < 96 ? -qs[a - 64] : -qs[128 - a];
}

/* waves round a struck note: two dashed arcs above it (dir 0) or one either side (dir 1) */
static void waves(int cx, int cy, int r, int dir, uint16_t c)
{
    int i;

    if (dir)
        for (i = 0; i < 2; i++) {
            arc(cx, cy, r - 4 * i, r + 4 - 4 * i, 44, 84, 1, c);        /* left */
            arc(cx, cy, r - 4 * i, r + 4 - 4 * i, -20, 20, 1, c);       /* right */
        }
    else
        for (i = 0; i < 2; i++)
            arc(cx, cy, r - 4 * i, (r - 4 * i) / 2, 68, 124, 1, c);
}

/* the mallet: its head on (x, y), the stick out to (x + dx, y + dy) */
static void mallet(int x, int y, int dx, int dy, int r, uint16_t head)
{
    seg(x * 16 + 8, y * 16, (x + dx) * 16 + 8, (y + dy) * 16, 16, C_WHITE);
    disc(x, y, r, head);
}

/* folds key k into [KEY_LO, KEY_LO + n) and lights it */
static void light(uint8_t *on, int k, int n)
{
    if (k <= 0)
        return;
    while (k < KEY_LO)
        k += 12;
    while (k >= KEY_LO + n)
        k -= 12;
    on[k - KEY_LO] = 1;
}

/* the notes that sound (held keys: their own note; notes without a key for FLASH blocks), folded
 * into n keys from C3; returns the newest one (the mallet's), -1 none */
static int sounding(uint8_t *on, int n)
{
    uint32_t t = blocks;
    int i, newest = -1, age = 256;

    for (i = 0; i < n; i++)
        on[i] = 0;
    for (i = 0; i < VOICES; i++)
        if ((int32_t)(flash_until[i] - t) > 0)
            light(on, flash_key[i], n);
    for (i = 0; i < HELD; i++)
        if (held_id[i] && held_key[i] > 0) {
            int a = (uint8_t)(press_seq - held_seq[i]), k = held_key[i];

            light(on, k, n);
            if (a < age) {
                age = a;
                while (k < KEY_LO)
                    k += 12;
                while (k >= KEY_LO + n)
                    k -= 12;
                newest = k - KEY_LO;
            }
        }
    if (newest < 0)
        for (i = 0; i < n; i++)
            if (on[i])
                newest = i;
    return newest;
}

static const int8_t white_of[12] = { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6 };

/* natural (column) of key i from C3; a sharp gives the natural below */
static int col_of(int i)
{
    int w = white_of[i % 12];

    return (i / 12) * 7 + (w >= 0 ? w : white_of[i % 12 - 1]);
}

/* marimba, vibraphone, glockenspiel: sharps raised behind the naturals inside a frame; marimba's
 * tubes below (as long as TUBE), the vibraphone's tubes with the motor's shaft and discs (open /
 * shut with its phase) and the pedal, the glockenspiel in a case with a handle */
static void bars(int instr, const uint8_t *on, int newest, int16_t tube, int motor)
{
    uint16_t fc = instr == ML_MARIMBA ? C_PURPLE : instr == ML_VIBES ? C_BLUE : C_TEAL;
    uint16_t lit = instr == ML_VIBES ? C_BLUE : C_GREEN;
    int glock = instr == ML_GLOCK, i, hx = 0, hy = 0;

    if (glock) {
        frame(35, 25, 251, 58, 5, 2, fc);
        outline(150, 21, 21, 5, 2, fc);         /* the handle */
        rect(36, 51, 249, 1, C_LINE);
    } else {
        frame(28, 23, 259, 56, 3, 2, fc);
        rect(28, 78, 2, 14, fc);            /* legs */
        rect(285, 78, 2, 14, fc);
        ring(29, 93, 2, 2, C_WHITE);
        ring(286, 93, 2, 2, C_WHITE);
    }
    rect(32, glock ? 34 : 31, 251, 1, C_LINE);  /* the rails */
    rect(32, glock ? 46 : 40, 251, 1, C_LINE);
    rect(32, glock ? 57 : 57, 251, 1, C_LINE);
    rect(32, glock ? 75 : 71, 251, 1, C_LINE);
    for (i = 0; i < 25; i++) {
        int col = col_of(i), sharp = white_of[i % 12] < 0, x = 34 + col * 33 / 2, w = 13, h, y;

        if (sharp) {
            x += 8;
            w = 11;
            h = glock ? 17 - col / 4 : 20 - col / 3;
            y = glock ? 29 : 26;
        } else {
            h = glock ? 26 - col * 6 / 14 : 28 - col * 10 / 14;
            y = glock ? 55 : 63 - h / 2;
            if (!glock) {                       /* tubes below the frame */
                int len = instr == ML_VIBES ? 11 : 2 + tube * (14 - col * 9 / 14) / 32767;

                pill(x + 3, 80, 7, len + 2, C_GREY);
                if (instr == ML_VIBES && motor)  /* the disc on the shaft: open or shut */
                    blob(x + 6, 85, ((col & 1) ^ (motor > 0)) ? 3 : 1, ((col & 1) ^ (motor > 0)) ? 1 : 2, C_BLUE);
            }
        }
        rbox(x, y, w, h, 2, on[i] ? lit : GFX_BLACK);   /* in front of the rails */
        outline(x, y, w, h, 2, on[i] ? lit : sharp ? C_GREY : C_WHITE);
        if (glock)
            rect(x + w / 2 - 1, y + 3, 2, 2, C_TEAL);
        if (i == newest) {
            hx = x + w / 2;
            hy = y + h / 2;
        }
    }
    if (instr == ML_MARIMBA) {
        thick(28, 87, 286, 87, fc);             /* the tubes' rail */
        disc(181, 24, 3, C_BLUE);           /* a second mallet, resting on the frame */
        line(181, 24, 204, 11, C_WHITE);
    } else if (instr == ML_VIBES) {
        line(40, 85, 274, 85, C_WHITE);         /* the motor's shaft, its pedal */
        line(274, 85, 278, 90, C_GREY);
        ring(279, 92, 3, 3, C_BLUE);
        pixel(279, 92, C_BLUE);
        rect(143, 94, 26, 1, C_WHITE);
    }
    if (newest >= 0) {
        if (glock) {
            waves(hx, hy, 10, 1, C_TEAL);
            mallet(hx, hy, 30, 22, 2, C_YELLOW);
        } else {
            waves(hx, hy - 10, 14, 0, instr == ML_VIBES ? C_BLUE : C_GREEN);
            mallet(hx, hy, 34, 40, 3, C_WHITE);     /* from below, as the xylophone's */
        }
    }
}

/* xylophone: one row of bars on two converging rails, mallet from below */
static void xylo(const uint8_t *on, int newest)
{
    int i, hx = 0, hy = 0;

    line(31, 30, 31, 82, C_YELLOW);             /* the ends */
    line(32, 30, 32, 82, C_YELLOW);
    line(286, 42, 290, 42, C_YELLOW);
    line(290, 42, 290, 70, C_YELLOW);
    line(286, 70, 290, 70, C_YELLOW);
    thick(32, 31, 286, 42, C_YELLOW);           /* the rails */
    thick(32, 81, 286, 70, C_YELLOW);
    for (i = 0; i < 15; i++) {
        int x = 36 + i * 17, yt = 27 + i * 11 / 14, yb = 86 - i * 12 / 14;
        int lit = 0, k;

        for (k = 0; k < 25; k++)
            if (on[k] && col_of(k) == i)
                lit = 1;
        rbox(x, yt, 11, yb - yt, 2, lit ? C_GREEN : GFX_BLACK);  /* in front of the rails */
        outline(x, yt, 11, yb - yt, 2, lit ? C_GREEN : C_WHITE);
        rect(x + 4, yt + 3, 2, 2, C_YELLOW);
        rect(x + 4, yb - 5, 2, 2, C_YELLOW);
        if (newest >= 0 && col_of(newest) == i) {
            hx = x + 5;
            hy = (yt + yb) / 2;
        }
    }
    if (newest >= 0) {
        waves(hx, hy, 12, 1, C_GREEN);
        mallet(hx, hy, 34, 40, 3, C_WHITE);
    }
}

/* steelpan / handpan: the pan from above; steelpan: 12 outer fields (the octave from C3 round the
 * circle of fifths, C at the top), 12 inner ones (the octave above), C5 in the middle; handpan: the
 * ding and 8 fields round it (key % 9). Beside it the pan from the side: the steelpan's drum on its
 * stand, the handpan's two shells */
static void pans(int instr, const uint8_t *on, int newest)
{
    static const int8_t fifth[12] = { 0, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10, 5 };   /* position -> note */
    uint16_t pc = instr == ML_STEELPAN ? C_GREEN : C_PURPLE;
    int i, cx = 95, cy = 57, hx = -1, hy = 0;

    ring2(cx, cy, 36, 36, pc);
    if (instr == ML_STEELPAN) {
        ring(cx, cy, 32, 32, C_LINE);
        arc(cx, cy, 19, 19, 0, 128, 1, C_LINE);
        for (i = 0; i < 25; i++) {
            int pos, rr, x, y, rx, ry;

            if (i < 24) {
                for (pos = 0; fifth[pos] != i % 12; pos++)
                    ;
                rr = i < 12 ? 25 : 13;
                x = cx + rr * sin128(pos * 128 / 12 + 96 + 32) / 256;
                y = cy + rr * sin128(pos * 128 / 12 + 96) / 256;
                rx = i < 12 ? 6 : 3;
                ry = i < 12 ? 5 : 3;
            } else {
                x = cx;
                y = cy;
                rx = ry = 3;
            }
            if (on[i])
                blob(x, y, rx, ry, C_GREEN);
            else
                ring(x, y, rx, ry, i < 12 ? C_WHITE : C_GREY);
            if (i == newest) {
                hx = x;
                hy = y;
            }
        }
        /* the drum from the side, on its stand */
        ring2(232, 34, 50, 9, pc);
        arc(232, 34, 34, 4, 0, 128, 1, C_GREY);
        thick(182, 34, 182, 70, pc);
        thick(281, 34, 281, 70, pc);
        ell(232 * 16, 70 * 16 + 8, 50 * 16, 9 * 16, 16, 0, 64, 0, pc);
        line(190, 74, 207, 91, C_GREY);
        line(274, 74, 257, 91, C_GREY);
        line(198, 84, 266, 84, C_GREY);
        line(203, 91, 212, 91, C_GREY);
        line(252, 91, 261, 91, C_GREY);
        if (hx >= 0) {
            arc(hx, hy, 9, 9, 0, 128, 1, C_GREEN);
            disc(hx + 1, hy - 1, 2, C_RED);   /* the stick's tip */
            line(hx + 2, hy - 2, hx + 30, hy - 26, C_WHITE);
        }
        return;
    }
    for (i = 0; i < 9; i++) {                   /* handpan: the ding, then 8 round it */
        int lit = 0, k, x = cx, y = cy, rx = 8, ry = 8;

        for (k = 0; k < 25; k++)
            if (on[k] && k % 9 == i)
                lit = 1;
        if (i) {
            x = cx + 24 * sin128((i - 1) * 16 + 96 + 32) / 256;
            y = cy + 24 * sin128((i - 1) * 16 + 96) / 256;
            rx = 7;
            ry = 6;
        }
        if (lit)
            blob(x, y, rx, ry, C_GREEN);
        ring(x, y, rx, ry, lit ? C_GREEN : C_WHITE);
        ring(x, y, rx / 2, ry / 2, lit ? GFX_BLACK : C_GREY);
        if (newest >= 0 && newest % 9 == i)
            arc(x, y, rx + 4, ry + 5, 0, 128, 1, C_GREEN);
    }
    /* the shells from the side: the ding on top, the port below, its sound */
    ring2(230, 56, 60, 17, pc);
    line(164, 56, 296, 56, pc);
    arc(230, 40, 11, 4, 64, 128, 0, C_WHITE);
    arc(196, 45, 7, 3, 72, 112, 0, C_GREY);
    arc(264, 45, 7, 3, 80, 120, 0, C_GREY);
    ring(230, 73, 11, 2, C_WHITE);
    arc(230, 74, 18, 6, 8, 56, 1, C_PURPLE);
    arc(230, 76, 28, 9, 8, 56, 1, C_PURPLE);
}

/* the kalimba's ringing at half pixel t2 (1/16 px): 12 px decaying to 1.5, 10 steps a pixel */
static int ring_y(int t2) { return 66 * 16 + (192 - t2 * 4 / 3) * sin128(t2 * 5) / 256; }

/* kalimba: the board with 17 tines (C3 .. E5's naturals: the lowest in the middle, then
 * alternating out), the sound hole; beside it a tine from the side and its ringing */
static void kalimba(const uint8_t *on, int newest)
{
    int n, i;

    frame(90, 22, 141, 71, 8, 2, C_WHITE);
    rect(100, 28, 121, 1, C_GREY);
    rect(97, 32, 127, 2, C_GREY);           /* the bridge */
    for (n = 0; n < 17; n++) {
        int j = 8 + (n & 1 ? (n + 1) / 2 : -(n / 2)), x = 104 + j * 7, len = 40 - (j < 8 ? 8 - j : j - 8) * 3;
        int lit = 0;

        for (i = 0; i < KEYS_MAX; i++)
            if (on[i] && col_of(i) == n)
                lit = 1;
        rect(x, 25, 2, len, lit ? C_RED : C_WHITE);
        if (newest >= 0 && col_of(newest) == n)
            for (i = 40; i < 66; i += 3) {      /* it rings: dashed either side */
                pixel(x - 3, i, C_RED);
                pixel(x + 4, i, C_RED);
            }
    }
    ring(160, 79, 8, 8, C_WHITE);
    arc(160, 79, 11, 11, 0, 128, 1, C_GREY);
    /* a tine from the side, lifted by the thumb */
    outline(22, 58, 57, 15, 4, C_WHITE);
    line(30, 57, 75, 53, C_WHITE);
    arc(56, 54, 19, 1, 64, 96, 1, C_RED);
    rect(38, 56, 2, 2, C_GREY);
    gfx_text(43, 78 + oy, "SIDE", C_GREY, 1);
    /* its ringing: a decaying sine */
    rect(240, 66, 63, 1, C_LINE);
    for (i = 0; i < 63; i++) {                  /* a span per column (1/16 px): no pixel twice */
        int y0 = ring_y(2 * i - 1), y1 = ring_y(2 * i), y2 = ring_y(2 * i + 1), lo, hi, py;

        lo = (y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2)) - 8;
        hi = (y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2)) + 8;
        for (py = (lo - 8) >> 4; py <= (hi + 23) >> 4; py++) {
            int t = py * 16 - 8 < lo ? lo : py * 16 - 8, b = py * 16 + 8 > hi ? hi : py * 16 + 8;

            plot(240 + i, py, C_RED, (b - t) * 16);
        }
    }
}

/* tubular bells: 12 tubes (one octave, chromatic) hanging from the frame, the hammer on the newest */
static void tubular(const uint8_t *on, int newest)
{
    int k, i;

    thick(45, 27, 276, 27, C_RED);
    rect(45, 27, 2, 63, C_RED);
    rect(274, 27, 2, 63, C_RED);
    thick(33, 89, 57, 89, C_RED);
    thick(263, 89, 287, 89, C_RED);
    ring(36, 93, 2, 2, C_WHITE);
    ring(54, 93, 2, 2, C_WHITE);
    ring(266, 93, 2, 2, C_WHITE);
    ring(284, 93, 2, 2, C_WHITE);
    for (k = 0; k < 12; k++) {
        int x = 59 + k * 18, len = 52 - k * 2, lit = 0;

        for (i = 0; i < 25; i++)
            if (on[i] && i % 12 == k)
                lit = 1;
        rect(x + 4, 28, 1, 6, C_GREY);
        if (lit)
            rbox(x, 34, 9, len, 4, C_GREEN);
        pill(x, 34, 9, len, lit ? C_GREEN : C_WHITE);
        rect(x + 1, 37, 7, 1, lit ? GFX_BLACK : C_WHITE);   /* the cap */
        if (newest >= 0 && newest % 12 == k) {
            waves(x + 4, 50, 14, 1, C_GREEN);
            rect(x + 10, 35, 7, 5, C_WHITE);              /* the hammer */
            line(x + 16, 36, x + 34, 22, C_WHITE);
        }
    }
    thick(51, 45, 269, 45, C_GREY);
    for (i = 50; i < 86; i += 3)
        pixel(165, i, C_DIM);
    rect(150, 90, 31, 1, C_WHITE);
}

/* the sound's name over the instrument (STATION 1's LCD: its place, size and colour) while the
 * title time runs: P01 .. P16 and the name (* = edited), or P-- and how to browse */
static void title(int slot)
{
    char t[24];
    const char *s;
    int own = ml_bank >= 0 && bank_slot == slot, n = 0, w;

    ml_title_on = (int32_t)(title_until - now) > 0;
    if (!ml_title_on)
        return;
    t[n++] = 'P';
    if (own) {
        t[n++] = (char)('0' + (ml_bank + 1) / 10);
        t[n++] = (char)('0' + (ml_bank + 1) % 10);
        t[n++] = ' ';
        for (s = bank[(int)ml_bank].name; *s; s++)
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

void ml_frame(uint16_t *buf)
{
    static const uint16_t col[4] = { C_BLUE, C_GREEN, C_WHITE, C_ORANGE };
    static int8_t shown = -1;                   /* the sound slot the screen showed last frame */
    static int8_t drawn = -1;                   /* the instrument it drew (its shapes kept) */
    int16_t p[ML_PARAMS];
    uint8_t on[KEYS_MAX];
    volatile int16_t *k;
    int slot, i, b, x, pg = ml_page, instr, motor, newest;

    if (!engine_screen_mallet()) {
        shown = -1;
        ml_title_on = 0;
        return;
    }
    k = current(&slot);
    if (slot != shown) {                        /* the screen opens or shows another sound: its name */
        shown = (int8_t)slot;
        title_until = now + TITLE_TIME;
    }
    unpack(k, p);
    instr = ml_instr(p[ML_INSTR]);
    motor = p[ML_MOTOR] < 300 ? 0 : (int32_t)motor_ph < 0 ? 1 : -1;
    newest = sounding(on, instr == ML_KALIMBA ? KEYS_MAX : 25);
    if (instr != drawn) {
        drawn = (int8_t)instr;
        eh_aa_forget();
    }

    gfx_begin(buf);
    eh_aa_begin(buf);
    gfx_rect(0, 0, gfx_w, 140, GFX_BLACK);

    /* the instrument, centred between the top and the knob row (its body's middle at y 48) */
    oy = instr == ML_MARIMBA || instr == ML_VIBES ? -11 : instr == ML_GLOCK ? -4 : instr == ML_XYLO ? -8
         : instr == ML_TUBULAR || instr == ML_KALIMBA ? -10 : -9;
    eh_aa_dy = oy;
    switch (instr) {
    case ML_XYLO:     xylo(on, newest); break;
    case ML_STEELPAN:
    case ML_HANDPAN:  pans(instr, on, newest); break;
    case ML_KALIMBA:  kalimba(on, newest); break;
    case ML_TUBULAR:  tubular(on, newest); break;
    default:          bars(instr, on, newest, p[ML_TUBE], motor); break;
    }
    oy = eh_aa_dy = 0;
    title(slot);

    /* the page's four knobs: name and bar (as STATION 1), the page at the right */
    x = gfx_text(306, 125, "P", C_RED, 1);
    gfx_dec(x, 125, pg + 1, C_RED, 1);
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
