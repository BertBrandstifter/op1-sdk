/* ACID 3 synth engine (spec: README.md): a TD-3-MO-style bassline voice (303 type with the
 * Devil-Fish-style mods), monophonic, with accent and slide.
 *
 * Runs on the engine host (features/enginehost, extended tag 8: slots 5-7 carry tag 7, slot 8
 * tag 1). 20 parameters on 5 pages + the FX page, stored in the sound's 8 knob slots (every value
 * inside drwave's ranges: presets load on stock as drwave): slots 1-4 = CUTOFF RESONANCE ENV MOD
 * DECAY in bits 6..13 (TE's LFO moves them) and 6 bits of fields below; slots 5-8 = tag | 12 bits of
 * fields. The render takes the fields from the sound's stored knobs (TE's LFO adds to the knobs it
 * hands over: their low bits would wobble), the four continuous values from TE's.
 *
 * Mono: one voice state (ac_core.c) carried by the TE voice of the newest note; it is rendered once
 * per audio block (the voice before fades it out over one block). A note-on in a block that was
 * already rendered for the note before rewinds the state to the block's start first. Slide and
 * accent of a new note come from the note observers (kb / MIDI note on / off: a key still held =
 * legato, MIDI velocity) or the SLIDES / ACCENTS modes.
 *
 * A key up that leaves no key / MIDI note held closes the gate (Open303: the amp releases in 0.5 ms,
 * accented notes 50 ms); notes without a key keep it open until TE's envelope ends them.
 *
 * ac_setup / ac_engine_name / ac_key / ac_knob / ac_frame / ac_tick: as MONO 3 (mono.c). */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "ac_core.h"
#include "../enginehost/eh_host.h"

#define KEY_KNOB_PUSH   59                      /* blue 59, green 60, white 61, orange 62 */
#define STEP_SCALE      10485                   /* ~100 clicks full range (as engine_plus) */
#define TAG             8
#define PAGES           6                       /* 5 parameter pages + FX */
#define FX_PAGE         5
#define TITLE_TIME      TE_SAMPLE_RATE          /* the sound's name on the display: 1 s */
#define PAUSE_BLOCKS    1378                    /* 2 s of audio blocks: the accent count restarts */
#define LAMPS           16
#define FLASH           100                     /* audio blocks: a note without a key lit ~145 ms */

/* the parameters, page by page (page = index / 4, knob = index % 4) */
enum {
    P_CUTOFF, P_RES, P_ENVMOD, P_DECAY,         /* FILTER (continuous, slots 1-4) */
    P_ACCENT, P_ACCDEC, P_SWEEP, P_SPEED,       /* ACCENT */
    P_WAVE, P_SUB, P_TUNE, P_SLIDE,             /* VCO */
    P_SOFT, P_TRACK, P_FM, P_DRIVE,             /* MODS */
    P_MUFF, P_ACCS, P_SLIDES, P_LEVEL,          /* PLAY */
    NP
};
enum { ACCS_OFF, ACCS_ALL, ACCS_VEL, ACCS_2ND, ACCS_3RD, ACCS_4TH };
enum { SLIDES_LEGATO, SLIDES_ALL, SLIDES_OFF };

static const uint8_t pmax[NP] = {
    255, 255, 255, 255,
    63, 31, 2, 2,
    1, 3, 24, 31,
    31, 31, 31, 31,
    2, 5, 2, 15,
};

/* where the fields live: container 0-3 = slot 1-4 bits 0..5, 4-7 = slots 5-8 bits 0..11 */
static const struct { uint8_t c, sh, bits; } fields[NP] = {
    [P_ACCENT] = { 0, 0, 6 },
    [P_ACCDEC] = { 1, 0, 5 }, [P_WAVE] = { 1, 5, 1 },
    [P_SWEEP] = { 2, 0, 2 }, [P_SPEED] = { 2, 2, 2 }, [P_SUB] = { 2, 4, 2 },
    [P_TUNE] = { 3, 0, 5 },
    [P_SLIDE] = { 4, 0, 5 }, [P_SOFT] = { 4, 5, 5 }, [P_MUFF] = { 4, 10, 2 },
    [P_TRACK] = { 5, 0, 5 }, [P_FM] = { 5, 5, 5 }, [P_SLIDES] = { 5, 10, 2 },
    [P_DRIVE] = { 6, 0, 5 }, [P_ACCS] = { 6, 5, 3 }, [P_LEVEL] = { 6, 8, 4 },
};

static ac_voice st, snap;                       /* the mono voice; its state at the block's start */
static int16_t blk[TE_VOICE_SAMPLES];           /* the block rendered (newest + the fading voice) */
static uint32_t ninc[TE_VOICE_SAMPLES];         /* the newest voice's pitch (first / last used) */
static uint32_t blocks, rendered = 0xffffffffu; /* audio blocks (render thread); block in blk */
static volatile int8_t newest = -1;             /* the TE voice of the newest note */
static int8_t prev = -1;                        /* the voice before it: fades out once */
static uint32_t count, last_note;               /* accent count, block of the last note */
volatile uint8_t ac_page;                       /* 0..5 = page 1..6 (bench reads) */
static volatile uint8_t consumed;
volatile int8_t ac_bank = -1;                   /* bench reads; -1 = P-- */
static volatile uint8_t bank_edited;
static volatile int8_t bank_slot = -1;
static int16_t own_knobs[8][8];                 /* per sound slot: P--, the sound before the bank */
static volatile uint32_t now;                   /* samples, advanced by ac_tick */
static volatile uint32_t title_until;
volatile uint8_t ac_title_on;                   /* drawn in the last frame (bench reads) */
static volatile uint8_t touched;                /* the page's knob turned last (the display shows it) */

/* note observers (KeyHandler / MIDI threads) -> render thread */
static volatile uint32_t held[4];               /* MIDI notes held (kb + MIDI), a bit each */
static volatile uint8_t pend_legato, pend_vel, pend_valid;
static volatile int8_t pend_note = -1;
static volatile uint8_t req_release;            /* the last held note is up: the gate closes */

/* the last 16 notes for the lamps (render writes, GUI reads) */
static volatile int8_t lamp_note[LAMPS];        /* MIDI note, -1 = none */
static volatile uint8_t lamp_flags[LAMPS];      /* 1 accent, 2 slid */
static volatile uint8_t lamp_head;              /* index of the newest */
volatile int8_t ac_note_shown = -1;             /* the note shown last frame (bench reads) */
static volatile uint32_t flash_until;
static volatile uint8_t keyed;                  /* the newest note came from a key / MIDI note still held */

static int marked(const volatile int16_t *k) { return eh_tag(k) == TAG; }

static int container(const volatile int16_t *k, int c)
{
    return c < 4 ? k[c] & 63 : k[c] & EH_P2_MAX;
}

static int field(const volatile int16_t *k, int id)
{
    return (container(k, fields[id].c) >> fields[id].sh) & ((1 << fields[id].bits) - 1);
}

static void field_set(volatile int16_t *k, int id, int v)
{
    int c = fields[id].c, m = ((1 << fields[id].bits) - 1) << fields[id].sh;
    int raw = (container(k, c) & ~m) | ((v << fields[id].sh) & m);

    if (c < 4)
        k[c] = (int16_t)((k[c] & ~63) | raw);
    else
        k[c] = eh_p2s(TAG, c - 4, raw);
}

/* a parameter as the page shows it (also the bench's) */
__attribute__((noinline)) int ac_param(const volatile int16_t *k, int id)
{
    return id <= P_DECAY ? (k[id] >> 6) & 255 : field(k, id);
}

__attribute__((noinline)) void ac_param_set(volatile int16_t *k, int id, int v)
{
    v = v < 0 ? 0 : v > pmax[id] ? pmax[id] : v;
    if (id <= P_DECAY)
        k[id] = (int16_t)((v << 6) | (k[id] & 63));
    else
        field_set(k, id, v);
}

static int32_t page1(const volatile int16_t *te, int i)
{
    int v = te[i];                              /* TE's LFO adds to slots 1-4 (saturating) */

    v = v < 0 ? 0 : v > EH_PAGE1_MAX ? EH_PAGE1_MAX : v;
    return (v >> 6) & 255;
}

static void params(const volatile int16_t *te, const volatile int16_t *s, ac_params *p)
{
    p->cutoff = page1(te, P_CUTOFF);
    p->res = page1(te, P_RES);
    p->envmod = page1(te, P_ENVMOD);
    p->decay = page1(te, P_DECAY);
    p->accent = (uint8_t)field(s, P_ACCENT);
    p->accdec = (uint8_t)field(s, P_ACCDEC);
    p->sweep = (uint8_t)field(s, P_SWEEP);
    p->speed = (uint8_t)field(s, P_SPEED);
    p->wave = (uint8_t)field(s, P_WAVE);
    p->sub = (uint8_t)field(s, P_SUB);
    p->tune = (int8_t)(field(s, P_TUNE) - 12);
    p->slide = (uint8_t)field(s, P_SLIDE);
    p->soft = (uint8_t)field(s, P_SOFT);
    p->track = (uint8_t)field(s, P_TRACK);
    p->fm = (uint8_t)field(s, P_FM);
    p->drive = (uint8_t)field(s, P_DRIVE);
    p->muff = (uint8_t)field(s, P_MUFF);
    p->level = (uint8_t)field(s, P_LEVEL);
}

/* the stored knobs of the sound a voice plays: slots 5-8 never carry TE's LFO, so they find it
 * (the current sound first); none found: TE's own */
static const volatile int16_t *stored(const int16_t *k)
{
    int s0 = *TE_SYNTH_SLOT & 7, j, i;

    for (j = 0; j < 8; j++) {
        const volatile int16_t *t = TE_SYNTH_KNOBS + ((s0 + j) & 7) * 8;

        for (i = 4; i < 8 && t[i] == k[i]; i++)
            ;
        if (i == 8)
            return t;
    }
    return k;
}

/* ------------------------------------------------------------------ render thread */

static int key_of(uint32_t inc)                 /* MIDI key of a pitch increment, rounded */
{
    int32_t k = 69 * 65536 + (ac_log2(inc) - ac_log2(21426141u)) * 12;

    k = (k + 32768) >> 16;
    return k < 0 ? 0 : k > 127 ? 127 : k;
}

static void new_note(int vi, const uint32_t *inc, const volatile int16_t *s)
{
    int slides = field(s, P_SLIDES), accs = field(s, P_ACCS), slide, accent, n, was_on = st.on;
    uint8_t h;

    slide = slides == SLIDES_ALL ? 1 : slides == SLIDES_LEGATO ? pend_valid && pend_legato : 0;
    if (blocks - last_note > PAUSE_BLOCKS)
        count = 0;
    last_note = blocks;
    n = accs == ACCS_2ND ? 2 : accs == ACCS_3RD ? 3 : accs == ACCS_4TH ? 4 : 0;
    accent = accs == ACCS_ALL ? 1 : accs == ACCS_VEL ? pend_valid && pend_vel : n ? count % n == 0 : 0;
    count++;
    if (rendered == blocks) {                   /* this block was the note before's: rewind */
        st = snap;
        rendered = 0xffffffffu;
    }
    if (newest >= 0 && newest != vi)
        prev = newest;
    newest = (int8_t)vi;
    ac_note(&st, slide, accent);
    h = (uint8_t)((lamp_head + 1) % LAMPS);
    lamp_note[h] = (int8_t)(pend_valid && pend_note >= 0 ? pend_note : key_of(inc[0]));
    lamp_flags[h] = (uint8_t)((accent ? 1 : 0) | (slide && was_on ? 2 : 0));
    lamp_head = h;
    keyed = pend_valid;
    if (!pend_valid)
        flash_until = blocks + FLASH;
    pend_valid = 0;
}

static void ac_eh_render(int vi, int16_t *out, const int16_t *knobs, const uint32_t *inc, int flags)
{
    ac_params p;
    const volatile int16_t *s = stored(knobs);
    int i;

    params(knobs, s, &p);
    if (flags & EH_BLOCK)
        blocks++;
    if (req_release) {                          /* before a new note of the same block */
        req_release = 0;
        if (keyed)
            ac_release(&st);
    }
    if (flags & EH_FRESH)
        new_note(vi, inc, s);
    if (vi == newest) {
        ninc[0] = inc[0];
        ninc[TE_VOICE_SAMPLES - 1] = inc[TE_VOICE_SAMPLES - 1];
    }
    if (vi != newest && vi != prev) {
        for (i = 0; i < TE_VOICE_SAMPLES; i++)
            out[i] = 0;
        return;
    }
    if (rendered != blocks) {
        snap = st;
        ac_render(&st, blk, TE_VOICE_SAMPLES, ninc, &p);
        rendered = blocks;
    }
    if (vi == newest) {
        for (i = 0; i < TE_VOICE_SAMPLES; i++)
            out[i] = blk[i];
    } else {                                    /* the voice before: fades out over this block, once */
        for (i = 0; i < TE_VOICE_SAMPLES; i++)
            out[i] = (int16_t)((blk[i] * (TE_VOICE_SAMPLES - 1 - i)) / TE_VOICE_SAMPLES);
        prev = -1;
    }
}

void ac_setup(void)
{
    int i;

    ac_init(&st);
    for (i = 0; i < LAMPS; i++)
        lamp_note[i] = -1;
    eh_register(TAG, ac_eh_render);
}

/* Engine browser row "acid 3" (content_sql) -> "drwave" (TE knows no such type: fatal) */
void ac_engine_name(te_string *out, void *db, const char *sql)
{
    static const char name[] = "acid 3";
    char *d = (char *)te_str_data(out);
    unsigned int i;

    (void)db;
    (void)sql;
    for (i = 0; i < sizeof(name) - 1 && i < out->size && d[i] == name[i]; i++)
        ;
    if (out->size == sizeof(name) - 1 && i == out->size) {
        d[0] = 'd'; d[1] = 'r'; d[2] = 'w'; d[3] = 'a'; d[4] = 'v'; d[5] = 'e'; d[6] = 0; /* cap >= 15 */
        out->size = 6;
        ac_page = 0;
        bank_slot = -1;
    }
}

/* ------------------------------------------------------------------ note observers */

static int any_held(void) { return (held[0] | held[1] | held[2] | held[3]) != 0; }

static void note_on(const te_note *n, int midi)
{
    int k = n->note;

    if (k < 0 || k > 127)
        return;
    pend_legato = (uint8_t)any_held();
    pend_vel = (uint8_t)(midi && n->vel >= 100);
    pend_note = (int8_t)k;
    pend_valid = 1;
    held[k >> 5] |= 1u << (k & 31);
}

static void note_off(const te_note *n)
{
    int k = n->note;

    if (k >= 0 && k <= 127)
        held[k >> 5] &= ~(1u << (k & 31));
    if (!any_held())
        req_release = 1;
}

void ac_kb_note_on(void *note, int extra) { (void)extra; note_on((const te_note *)note, 0); }
void ac_kb_note_off(void *note) { note_off((const te_note *)note); }
void ac_midi_note_on(void *note, int extra) { (void)extra; note_on((const te_note *)note, 1); }
void ac_midi_note_off(void *note) { note_off((const te_note *)note); }

/* ------------------------------------------------------------------ KeyHandler thread */

static volatile int16_t *current(int *slot)
{
    *slot = *TE_SYNTH_SLOT & 7;
    return TE_SYNTH_KNOBS + *slot * 8;
}

static int engine_screen_acid(void)
{
    int slot;

    return *TE_EDIT_MODE == 0 && *TE_SYNTH_SUBSCREEN == 0
           && TE_SYNTH_TYPE[*TE_SYNTH_SLOT & 7] == TE_INSTR_DRWAVE && marked(current(&slot));
}

int ac_key(int key, int down)
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
    if (*TE_SHIFT_HELD || !engine_screen_acid())
        return 0;
    ac_page = (uint8_t)(ac_page + 1 < PAGES ? ac_page + 1 : 0);
    touched = 0;
    consumed |= (uint8_t)(1 << b);
    return 1;
}

/* built-in bank: name + the 20 parameters in page order (as stored: TUNE = semitones + 12) */
typedef struct {
    const char *name;
    uint8_t v[NP];
} bank_entry;

static const bank_entry bank[] = {
    /*                cut  res  env  dec   acc ad sw sp   wv sb tn sl   so tr fm dr   mu as sd lv */
    { "ACID LINE",   { 100, 170, 140, 150, 45, 13, 1, 1,  0, 0, 12, 16,  15, 0, 0, 0,   0, 4, 0, 10 } },
    { "SQUELCH",     { 60, 235, 200, 120,  55, 13, 1, 1,  0, 0, 12, 16,  15, 0, 0, 0,   0, 3, 0, 9 } },
    { "ROLLER",      { 120, 140, 110, 90,  35, 10, 1, 1,  0, 0, 12, 14,  12, 0, 0, 0,   0, 5, 0, 14 } },
    { "SUB LINE",    { 80, 110, 90, 170,   30, 14, 0, 1,  1, 2, 12, 16,  15, 0, 0, 0,   0, 0, 0, 13 } },
    { "DEEP SQUARE", { 70, 160, 130, 180,  40, 14, 1, 1,  1, 1, 12, 16,  18, 4, 0, 0,   0, 5, 0, 12 } },
    { "WOW ACCENT",  { 50, 220, 90, 130,   63, 16, 2, 0,  0, 0, 12, 16,  15, 0, 0, 0,   0, 1, 0, 6 } },
    { "RUBBER",      { 90, 190, 170, 80,   50, 9, 1, 2,   0, 1, 12, 18,  8, 8, 0, 6,    1, 3, 0, 10 } },
    { "FM GROWL",    { 110, 190, 110, 150, 40, 12, 1, 1,  0, 0, 12, 16,  15, 10, 18, 8, 1, 4, 0, 13 } },
    { "MUFFLED",     { 130, 130, 90, 160,  35, 14, 1, 1,  0, 1, 12, 16,  20, 0, 0, 12,  2, 3, 0, 7 } },
    { "PLUCK",       { 40, 120, 220, 60,   30, 6, 0, 2,   0, 0, 12, 10,  4, 10, 0, 0,   0, 0, 2, 15 } },
    { "RESO RISE",   { 30, 250, 60, 230,   55, 20, 2, 0,  0, 0, 12, 20,  20, 4, 0, 0,   0, 3, 0, 8 } },
    { "OVERDRIVE",   { 110, 170, 140, 140, 45, 12, 1, 1,  0, 1, 12, 16,  15, 0, 4, 24,  1, 4, 0, 10 } },
    { "TRACK LEAD",  { 150, 160, 100, 180, 35, 14, 1, 1,  0, 0, 24, 22,  20, 28, 0, 6, 0, 5, 0, 15 } },
    { "BLIP",        { 140, 150, 240, 30,  40, 4, 0, 2,   1, 0, 24, 8,   2, 20, 0, 0,   0, 3, 2, 15 } },
    { "SLIDE LEAD",  { 130, 190, 120, 200, 40, 16, 1, 1,  0, 0, 24, 26,  22, 16, 6, 8, 1, 0, 1, 15 } },
    { "DRONE",       { 70, 225, 60, 255,   20, 31, 1, 0,  0, 3, 0, 22,   26, 0, 8, 10,  1, 0, 0, 11 } },
};
#define BANK            ((int)(sizeof(bank) / sizeof(bank[0])))

/* a bank entry into a knob block (bench: the default preset is entry 0) */
__attribute__((noinline)) void ac_bank_knobs(int e, int16_t *k)
{
    int i;

    for (i = 0; i < 8; i++)
        k[i] = i < 4 ? 0 : eh_p2s(TAG, i - 4, 0);
    for (i = 0; i < NP; i++)
        ac_param_set(k, i, bank[e].v[i]);
}

static void bank_load(volatile int16_t *k, int slot, int e)
{
    int16_t b[8];
    int i;

    ac_bank_knobs(e, b);
    for (i = 0; i < 8; i++)
        k[i] = b[i];
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
    ac_bank = (int8_t)e;
    bank_slot = (int8_t)slot;
    bank_edited = 0;
}

int ac_knob(int knob, int16_t *diff)
{
    int slot, d = *diff, i, id;
    volatile int16_t *k;

    if ((unsigned int)knob > 3 || !engine_screen_acid())
        return 0;
    k = current(&slot);
    if (*TE_SHIFT_HELD) {                       /* drwave's shift + knob: not on an ACID sound */
        if (knob == 0 && d) {                   /* P-- (the sound as it was), P01 .. P16, P-- ... */
            int e = bank_slot == slot ? ac_bank : -1, s = d > 0 ? 1 : -1;

            if (e < 0)
                for (i = 0; i < 8; i++)
                    own_knobs[slot][i] = k[i];
            e += s;
            e = e >= BANK ? -1 : e < -1 ? BANK - 1 : e;
            if (e >= 0) {
                bank_load(k, slot, e);
            } else {
                for (i = 0; i < 8; i++)
                    k[i] = own_knobs[slot][i];
                TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
                ac_bank = -1;
                bank_slot = (int8_t)slot;
                bank_edited = 0;
            }
            title_until = now + TITLE_TIME;
        }
        return 1;
    }
    if (ac_page == FX_PAGE)
        return eh_fx_knob(slot, knob, (int16_t)d);
    touched = (uint8_t)knob;
    bank_edited = 1;
    id = ac_page * 4 + knob;
    if (id <= P_DECAY) {                        /* 256 steps, ~100 clicks over the range */
        int step = (d * (STEP_SCALE >> 1)) >> 21;

        step = step ? step : d > 0 ? 1 : d < 0 ? -1 : 0;
        ac_param_set(k, id, ac_param(k, id) + step);
    } else {                                    /* one step per detent, more when fast */
        int step = (d < 0 ? -d : d) >> 10;

        step = step < 1 ? 1 : step;
        if (pmax[id] < 8)
            step = 1;
        ac_param_set(k, id, ac_param(k, id) + (d > 0 ? step : -step));
    }
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
    return 1;
}

/* seq_tick (Seq thread, once per 64-frame audio block): the clock of the sound's name */
void ac_tick(void)
{
    now += TE_AUDIO_FRAMES;
}

/* ------------------------------------------------------------------ GUI thread */

/* Screen (README.md "Screen"): a green (80s phosphor) LCD 16 x 2 (the knob turned last: name + value, over the
 * page and the note sounding; the sound's name for 1 s), a row of 16 step lamps (the last notes:
 * accent red, a slide a bar to the one before), the page's four knobs below (as MONO 3). */

static const char *const labels[NP] = {
    "CUTOFF", "RESONANCE", "ENV MOD", "DECAY", "ACCENT", "ACC DECAY", "SWEEP", "SPEED",
    "WAVE", "SUB", "TUNE", "SLIDE", "SOFT ATK", "TRACKING", "FILTER FM", "OVERDRIVE",
    "MUFFLER", "ACCENTS", "SLIDES", "LEVEL",
};
static const char *const page_names[PAGES] = { "FILTER", "ACCENT", "VCO", "MODS", "PLAY", "FX" };

#define C_BLUE      GFX_RGB(0x6e, 0x8b, 0xff)
#define C_GREEN     GFX_RGB(0x3d, 0xdc, 0x97)
#define C_WHITE     GFX_RGB(0xe6, 0xe6, 0xe6)
#define C_ORANGE    GFX_RGB(0xff, 0x6a, 0x3d)
#define C_DIM       GFX_RGB(0x5a, 0x58, 0x66)
#define C_TRACK     GFX_RGB(0x35, 0x32, 0x38)
#define C_LCD       GFX_RGB(0x4a, 0xff, 0x5a)   /* 80s green phosphor */
#define C_LCD_OFF   GFX_RGB(0x0c, 0x2c, 0x12)   /* the LCD: unlit dots, its glass, its frame */
#define C_LCD_BG    GFX_RGB(0x03, 0x12, 0x06)
#define C_FRAME     GFX_RGB(0x2c, 0x8a, 0x3c)
#define C_LAMP_OFF  GFX_RGB(0x3a, 0x36, 0x3e)
#define C_LAMP      GFX_RGB(0x3c, 0xd8, 0x50)
#define C_ACC       GFX_RGB(0xff, 0x3c, 0x3c)
#define C_NOW       GFX_RGB(0xdc, 0xff, 0xdc)

static const eh_lcd lcd = { 17, 13, 16, 3, 2, 18, 26, C_LCD, C_LCD_OFF };

/* text helpers (no printf) */
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

static int len(const char *s)
{
    int n = 0;

    while (s[n])
        n++;
    return n;
}

static char *put_ten(char *d, int v, int max)  /* 0..max on a 0..10 scale, one decimal */
{
    int t = (v * 100 + max / 2) / max;

    if (t >= 100)
        return put(d, "10");
    return put_dec(put(put_dec(d, t / 10), "."), t % 10);
}

static char *put_ms(char *d, int ms)            /* 250MS, 1.2S */
{
    if (ms < 1000)
        return put(put_dec(d, ms < 1 ? 1 : ms), "MS");
    return put(put_dec(put(put_dec(d, ms / 1000), "."), ms % 1000 / 100), "S");
}

static void value(char *d, int id, int v)
{
    static const char *const sweeps[3] = { "OFF", "NORM", "HIGH" };
    static const char *const speeds[3] = { "SLOW", "NORM", "FAST" };
    static const char *const subs[4] = { "OFF", "LO", "MID", "HI" };
    static const char *const muffs[3] = { "OFF", "SOFT", "HARD" };
    static const char *const accs[6] = { "OFF", "ALL", "VEL", "2ND", "3RD", "4TH" };
    static const char *const slides[3] = { "LEGATO", "ALL", "OFF" };
    int t;

    switch (id) {
    case P_DECAY:
        put_ms(d, ac_decay_ms(v));
        break;
    case P_ACCENT:
        put_ten(d, v, 63);
        break;
    case P_ACCDEC:
        put_ms(d, ac_accdec_ms(v));
        break;
    case P_SWEEP: put(d, sweeps[v > 2 ? 2 : v]); break;
    case P_SPEED: put(d, speeds[v > 2 ? 2 : v]); break;
    case P_WAVE: put(d, v ? "SQUARE" : "SAW"); break;
    case P_SUB: put(d, subs[v & 3]); break;
    case P_TUNE:
        put(put_dec(put(d, v > 12 ? "+" : ""), v - 12), "ST");
        break;
    case P_SLIDE:
        put_ms(d, ac_slide_ms(v));
        break;
    case P_SOFT:
        t = ac_soft_10us(v);
        if (t < 100)
            put(put_dec(put(put_dec(d, t / 10), "."), t % 10), "MS");
        else
            put_ms(d, t / 10);
        break;
    case P_TRACK: case P_FM: case P_DRIVE:
        put_ten(d, v, 31);
        break;
    case P_MUFF: put(d, muffs[v > 2 ? 2 : v]); break;
    case P_ACCS: put(d, accs[v > 5 ? 5 : v]); break;
    case P_SLIDES: put(d, slides[v > 2 ? 2 : v]); break;
    case P_LEVEL: put_dec(d, v); break;
    default:                                    /* CUTOFF RESONANCE ENV MOD */
        put_ten(d, v, 255);
        break;
    }
}

static int any_held_now(void) { return any_held(); }

/* the note sounding: the newest while its key / MIDI note is held, or for FLASH blocks without one */
static int sounding(void)
{
    int n = lamp_note[lamp_head];

    if (newest < 0 || n < 0)
        return -1;
    if (keyed ? any_held_now() : (int32_t)(flash_until - blocks) > 0)
        return n;
    return -1;
}

/* the acid-house smiley in display dots (15 x 15 at the LCD's dot pitch) over the right 5 cells:
 * the outline, eyes and smile lit (a line drawing reads best at this size) */
static void smiley(void)
{
    static const uint16_t face[15] = {
        0x07c0, 0x1830, 0x2008, 0x4004, 0x4444, 0x8442, 0x8442, 0x8002, 
        0x8002, 0x9012, 0x4824, 0x47c4, 0x2008, 0x1830, 0x07c0,
    };                                          /* bit 15 = left column */
    int r, c, x0 = 238, y0 = 13;

    gfx_rect(212, 10, 96, 50, C_LCD_BG);        /* the right 5 cells */
    for (r = 0; r < 15; r++)
        for (c = 0; c < 15; c++)
            gfx_rect(x0 + c * 3, y0 + r * 3, 2, 2, (face[r] >> (15 - c)) & 1 ? C_LCD : C_LCD_OFF);
}

static void display(int slot, int pg, const int *u)
{
    static const char *const nn[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    char t[20];
    int own = ac_bank >= 0 && bank_slot == slot, id, note;

    ac_title_on = (int32_t)(title_until - now) > 0;
    note = sounding();
    ac_note_shown = (int8_t)note;
    eh_lcd_blank(&lcd, 2);
    if (ac_title_on) {                          /* loading: P03 / the name left, the smiley right */
        if (own) {
            put_dec(put(t, ac_bank < 9 ? "P0" : "P"), ac_bank + 1);
            eh_lcd_text(&lcd, 0, 0, t, C_LCD);
            if (bank_edited)
                eh_lcd_text(&lcd, 0, 3, "*", C_LCD);
            eh_lcd_text(&lcd, 1, 0, bank[(int)ac_bank].name, C_LCD);
        } else {
            eh_lcd_text(&lcd, 0, 0, "P--", C_LCD);
            eh_lcd_text(&lcd, 1, 0, "SHIFT+BLUE", C_LCD);
        }
        smiley();
        return;
    }
    if (pg == FX_PAGE) {
        if (own) {
            put_dec(put(t, ac_bank < 9 ? "P0" : "P"), ac_bank + 1);
            eh_lcd_text(&lcd, 0, 0, t, C_LCD);
            eh_lcd_text(&lcd, 0, 4, bank[(int)ac_bank].name, C_LCD);
            if (bank_edited)
                eh_lcd_text(&lcd, 0, 15, "*", C_LCD);
        } else {
            eh_lcd_text(&lcd, 0, 0, "P--", C_LCD);
        }
    } else {
        id = pg * 4 + touched;
        value(t, id, u[id]);
        eh_lcd_text(&lcd, 0, 0, labels[id], C_LCD);
        eh_lcd_text(&lcd, 0, 16 - len(t), t, C_LCD);
    }
    eh_lcd_text(&lcd, 1, 0, page_names[pg], C_LCD);
    if (note >= 0) {
        put_dec(put(t, nn[note % 12]), note / 12 - 1);
        eh_lcd_text(&lcd, 1, 16 - len(t), t, C_LCD);
    }
}

/* the step lamps: the last 16 notes, the newest right; accent red, slide a bar to the one before */
static void lamps(void)
{
    int i, h = lamp_head, lit = sounding() >= 0;

    for (i = 0; i < LAMPS; i++) {
        int e = (h + 1 + i) % LAMPS, x = 25 + i * 18, y = 76;
        int n = lamp_note[e], f = lamp_flags[e];
        uint16_t c = n < 0 ? C_LAMP_OFF : (f & 1) ? C_ACC : C_LAMP;

        if (n >= 0 && (f & 2) && i > 0 && lamp_note[(e + LAMPS - 1) % LAMPS] >= 0)
            eh_aa_line((x - 18 + 6) * 16, y * 16, (x - 6) * 16, y * 16, 20, C_LAMP);
        eh_aa_ellipse(x * 16, y * 16, 80, 80, -1, 0, 128, 0, c);
        if (i == LAMPS - 1 && n >= 0 && lit)
            eh_aa_ellipse(x * 16, y * 16, 36, 36, -1, 0, 128, 0, C_NOW);
    }
}

void ac_frame(uint16_t *buf)
{
    static const uint16_t col[4] = { C_BLUE, C_GREEN, C_WHITE, C_ORANGE };
    static int8_t shown = -1;                   /* the sound slot the screen showed last frame */
    volatile int16_t *k;
    int u[NP], slot, i, x, pg = ac_page, id;

    if (!engine_screen_acid()) {
        shown = -1;
        ac_title_on = 0;
        return;
    }
    k = current(&slot);
    if (slot != shown) {
        shown = (int8_t)slot;
        title_until = now + TITLE_TIME;
    }
    for (i = 0; i < NP; i++)
        u[i] = ac_param(k, i);

    gfx_begin(buf);
    gfx_rect(0, 0, gfx_w, 140, GFX_BLACK);
    eh_aa_begin(buf);
    eh_frame(8, 4, 304, 58, 6, 2, C_FRAME, C_LCD_BG);
    display(slot, pg, u);
    lamps();

    if (pg == FX_PAGE) {                        /* the sound's FX knobs (engine host) */
        eh_fx_draw(slot, 97);
        return;
    }
    for (i = 0; i < 4; i++) {
        id = pg * 4 + i;
        x = 4 + i * 80;
        gfx_text(x, 99, labels[id], col[i], 2);
        gfx_rect(x, 112, 72, 6, C_TRACK);
        gfx_rect(x, 112, u[id] * 72 / pmax[id], 6, col[i]);
    }
    x = gfx_text(4, 125, "PUSH: ", C_DIM, 1);
    gfx_text(x, 125, page_names[(pg + 1) % PAGES], C_DIM, 1);
}
