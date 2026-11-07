/* MONO 3 synth engine (spec: README.md): a classic 70s mono synth voice (three oscillators with range,
 * wave and level, noise, the 4-pole ladder with its contour, the loudness contour, glide),
 * made polyphonic.
 *
 * Runs on the engine host (features/enginehost, tag 2). 24 parameters on 6 pages + the FX page,
 * stored in the sound's 8 knob slots (80 bits, every value inside drwave's ranges: presets load on
 * stock as drwave): slots 1-3 = CUTOFF EMPHASIS CONTOUR in their upper 8 bits (TE's LFO moves them)
 * and 6 bits of fields below; slot 4 = 15 bits of fields; slots 5-8 = 0x2000 | 12 bits of fields.
 * The render takes the fields from the sound's stored knobs (TE's LFO adds to the knobs it hands
 * over: their low bits would wobble), the three continuous values from TE's.
 *
 * Poly: every TE voice has its own MONO voice (mo_core.c); the 4 newest notes sound (an older one
 * fades out within a block: the cost stays at TRIO's level); a new note glides from where the last
 * one was (poly portamento). Key events (on_key observe) pair a held key with the TE voice it starts;
 * with RELEASE on its key up releases that voice's contours (a request the render thread applies).
 * RELEASE off, and notes without a key (sequencer, MIDI, unison's extra voices), keep their
 * contours; TE's envelope ends them.
 *
 * mo_setup / mo_engine_name / mo_key / mo_note_key / mo_knob / mo_frame: as STATION 1 (m1.c);
 * mo_tick (seq_tick) is the clock of the sound's name over the panel. */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "mo_core.h"
#include "../enginehost/eh_host.h"

#define KEY_KNOB_PUSH   59                      /* blue 59, green 60, white 61, orange 62 */
#define STEP_SCALE      10485                   /* ~100 clicks full range (as engine_plus) */
#define TAG             2
#define PAGES           7                       /* 6 parameter pages + FX */
#define FX_PAGE         6
#define HELD            8
#define FLASH           100                     /* audio blocks: ~145 ms */
#define TITLE_TIME      TE_SAMPLE_RATE          /* the sound's name over the panel: 1 s */

#define MAX_SOUNDING 4                           /* the 4 newest notes sound (cost: as TRIO's 6) */

/* the parameters, page by page (page = index / 4, knob = index % 4) */
enum {
    P_CUTOFF, P_EMPH, P_CONTOUR, P_GLIDE,       /* FILTER */
    P_RANGE1, P_WAVE1, P_VOL1, P_NOISE,         /* OSC 1 (+ noise: 0 off, 1..15 white, 16..30 pink) */
    P_RANGE2, P_FREQ2, P_WAVE2, P_VOL2,         /* OSC 2 */
    P_RANGE3, P_FREQ3, P_WAVE3, P_VOL3,         /* OSC 3 */
    P_FATT, P_FDEC, P_FSUS, P_KBD,              /* FILTER CONTOUR + keyboard control */
    P_LATT, P_LDEC, P_LSUS, P_RELEASE,          /* LOUDNESS CONTOUR + the decay switch */
    NP,
    S_PINK = NP                                 /* stored on its own, shown with P_NOISE */
};

static const uint8_t pmax[NP] = {
    255, 255, 255, MO_GLIDES - 1,
    MO_RANGES - 1, MO_WAVES - 1, MO_LEVELS - 1, 2 * (MO_LEVELS - 1),
    MO_RANGES - 1, MO_FREQS - 1, MO_WAVES - 1, MO_LEVELS - 1,
    MO_RANGES - 1, MO_FREQS - 1, MO_WAVES - 1, MO_LEVELS - 1,
    MO_TIMES - 1, MO_TIMES - 1, 15, MO_KBDS - 1,
    MO_TIMES - 1, MO_TIMES - 1, 15, 1,
};

/* where the fields live, by parameter: container 0-2 = slot 1-3 bits 0..5, 3 = slot 4 bits 0..14,
 * 4-7 = slots 5-8 bits 0..11 (under the tag); bits 0 = CUTOFF EMPHASIS CONTOUR (slots 1-3, 6..13) */
static const struct { uint8_t c, sh, bits; } fields[NP + 1] = {
    [P_FREQ2] = { 0, 0, 5 }, [P_RELEASE] = { 0, 5, 1 },
    [P_FREQ3] = { 1, 0, 5 }, [S_PINK] = { 1, 5, 1 },
    [P_NOISE] = { 2, 0, 4 }, [P_KBD] = { 2, 4, 2 },
    [P_FATT] = { 3, 0, 5 }, [P_FDEC] = { 3, 5, 5 }, [P_LATT] = { 3, 10, 5 },
    [P_LDEC] = { 4, 0, 5 }, [P_GLIDE] = { 4, 5, 4 }, [P_RANGE1] = { 4, 9, 3 },
    [P_FSUS] = { 5, 0, 4 }, [P_LSUS] = { 5, 4, 4 }, [P_VOL1] = { 5, 8, 4 },
    [P_VOL2] = { 6, 0, 4 }, [P_VOL3] = { 6, 4, 4 }, [P_RANGE2] = { 6, 8, 3 },
    [P_RANGE3] = { 7, 0, 3 }, [P_WAVE1] = { 7, 3, 3 }, [P_WAVE2] = { 7, 6, 3 }, [P_WAVE3] = { 7, 9, 3 },
};

static mo_voice voices[EH_VOICES];
static uint32_t order[EH_VOICES], notes;        /* note-on order per voice (render thread) */
static uint8_t gone;                            /* voices past the newest 4: faded out (a bit each) */
static int32_t last_pitch;                      /* the newest note's pitch now (log2, Q16): glides start there */
static volatile int8_t newest = -1;             /* the TE voice of the newest note */
static volatile uint8_t req_release;            /* KeyHandler -> render: contour release, a bit per voice */
static volatile uint32_t blocks;                /* audio blocks (render thread) */
volatile uint8_t mo_page;                       /* 0..6 = page 1..7 (bench reads) */
static volatile uint8_t consumed;
volatile int8_t mo_bank = -1;                   /* bench reads; -1 = P-- */
static volatile uint8_t bank_edited;
static volatile int8_t bank_slot = -1;
static int16_t own_knobs[8][8];                 /* per sound slot: P--, the sound before the bank */
static volatile uint32_t now;                   /* samples, advanced by mo_tick */
static volatile uint32_t title_until;           /* the sound's name shown while now < this */
volatile uint8_t mo_title_on;                   /* drawn in the last frame (bench reads) */
volatile int8_t mo_note_shown = -1;             /* the lowest sounding note shown last frame (bench) */
static volatile uint8_t touched;                /* the page's knob turned last (the display shows it) */

/* the sounding note (as STATION 1's LCD): a held key is its own note (TE plays note index + 53 +
 * 12 x octave), the octave from the voice it starts (rounded: TE's glide / LFO / bend at the start
 * move it by less than 6 semitones); new voices no held key accounts for (sequencer, MIDI, CHORD+'s
 * extra notes) flash at their pitch for FLASH blocks */
static volatile int8_t held_id[HELD];           /* key id held, 0 = free (KeyHandler thread) */
static volatile int8_t held_note[HELD];         /* its note index 0..23 */
static volatile int8_t held_voice[HELD];        /* the TE voice it started, -1 = none yet (render) */
static volatile int8_t held_key[HELD];          /* its MIDI key (render) */
static volatile uint8_t held_seq[HELD];
static volatile uint8_t press_seq;
static volatile int8_t flash_key[EH_VOICES];
static volatile uint32_t flash_until[EH_VOICES];

static int marked(const volatile int16_t *k) { return eh_tag(k) == TAG; }

static int container(const volatile int16_t *k, int c)
{
    return c < 3 ? k[c] & 63 : c == 3 ? k[3] & 0x7fff : k[c] & EH_P2_MAX;
}

/* a stored parameter (S_PINK: the noise colour; P_NOISE: its level 0..15) */
static int field(const volatile int16_t *k, int id)
{
    return (container(k, fields[id].c) >> fields[id].sh) & ((1 << fields[id].bits) - 1);
}

static void field_set(volatile int16_t *k, int id, int v)
{
    int c = fields[id].c, m = ((1 << fields[id].bits) - 1) << fields[id].sh;
    int raw = (container(k, c) & ~m) | ((v << fields[id].sh) & m);

    if (c < 3)
        k[c] = (int16_t)((k[c] & ~63) | raw);
    else if (c == 3)
        k[3] = (int16_t)raw;
    else
        k[c] = eh_p2(TAG, raw);
}

/* a parameter as the page shows it (also the bench's) */
int mo_param(const volatile int16_t *k, int id)
{
    int v;

    if (id <= P_CONTOUR)
        return (k[id] >> 6) & 255;
    if (id == P_NOISE) {
        v = field(k, P_NOISE);
        return v && field(k, S_PINK) ? v + 15 : v;
    }
    return field(k, id);
}

void mo_param_set(volatile int16_t *k, int id, int v)
{
    v = v < 0 ? 0 : v > pmax[id] ? pmax[id] : v;
    if (id <= P_CONTOUR) {
        k[id] = (int16_t)((v << 6) | (k[id] & 63));
    } else if (id == P_NOISE) {
        field_set(k, P_NOISE, v > 15 ? v - 15 : v);
        field_set(k, S_PINK, v > 15);
    } else {
        field_set(k, id, v);
    }
}

/* core parameters: the fields from the stored knobs st, the continuous ones from TE's (+ LFO) */
static int32_t page1(const volatile int16_t *te, int i)
{
    int v = te[i];                              /* TE's LFO adds to slots 1-4 (saturating) */

    return (v < 0 ? 0 : v > EH_PAGE1_MAX ? EH_PAGE1_MAX : v) << 1;
}

static void params(const volatile int16_t *te, const volatile int16_t *st, mo_params *p)
{
    p->cutoff = page1(te, P_CUTOFF);
    p->emph = page1(te, P_EMPH);
    p->contour = page1(te, P_CONTOUR);
    p->glide = (uint8_t)field(st, P_GLIDE);
    p->range[0] = (uint8_t)field(st, P_RANGE1);
    p->range[1] = (uint8_t)field(st, P_RANGE2);
    p->range[2] = (uint8_t)field(st, P_RANGE3);
    p->wave[0] = (uint8_t)field(st, P_WAVE1);
    p->wave[1] = (uint8_t)field(st, P_WAVE2);
    p->wave[2] = (uint8_t)field(st, P_WAVE3);
    p->freq[0] = (uint8_t)field(st, P_FREQ2);
    p->freq[1] = (uint8_t)field(st, P_FREQ3);
    p->vol[0] = (uint8_t)field(st, P_VOL1);
    p->vol[1] = (uint8_t)field(st, P_VOL2);
    p->vol[2] = (uint8_t)field(st, P_VOL3);
    p->vol[3] = (uint8_t)field(st, P_NOISE);
    p->pink = (uint8_t)field(st, S_PINK);
    p->fatt = (uint8_t)field(st, P_FATT);
    p->fdec = (uint8_t)field(st, P_FDEC);
    p->fsus = (uint8_t)field(st, P_FSUS);
    p->latt = (uint8_t)field(st, P_LATT);
    p->ldec = (uint8_t)field(st, P_LDEC);
    p->lsus = (uint8_t)field(st, P_LSUS);
    p->kbd = (uint8_t)field(st, P_KBD);
    p->release = (uint8_t)field(st, P_RELEASE);
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

static void mo_eh_render(int vi, int16_t *out, const int16_t *knobs, const uint32_t *inc, int flags)
{
    mo_params p;
    mo_voice *v = &voices[vi];
    int i;

    params(knobs, stored(knobs), &p);
    if (flags & EH_BLOCK)
        blocks++;
    if (flags & EH_FRESH) {                     /* a new note: pair it with its key, glide from the last */
        int oldest = -1;

        for (i = 0; i < HELD; i++)
            if (held_id[i] && held_voice[i] < 0
                && (oldest < 0 || (uint8_t)(held_seq[i] - held_seq[oldest]) > 127))
                oldest = i;
        req_release &= (uint8_t)~(1 << vi);
        v->cur = last_pitch;
        v->sounding = newest >= 0;              /* a glide needs a note before */
        mo_note(v, inc[0], &p);
        if (oldest >= 0) {
            int n = held_note[oldest] + 53, d = (int)v->key - n;

            d = d >= 0 ? (d + 6) / 12 : -((5 - d) / 12);    /* octaves, rounded */
            n += 12 * d;
            held_key[oldest] = (int8_t)(n < 0 ? 0 : n > 127 ? 127 : n);
            held_voice[oldest] = (int8_t)vi;
        } else {                                /* no key: sequencer / MIDI */
            flash_key[vi] = (int8_t)(v->key < 0 ? 0 : v->key > 127 ? 127 : v->key);
            flash_until[vi] = blocks + FLASH;
        }
        newest = (int8_t)vi;
        order[vi] = ++notes;
        gone &= (uint8_t)~(1 << vi);
    }
    if (req_release & (1 << vi)) {
        req_release &= (uint8_t)~(1 << vi);
        mo_release(v, &p);
    }
    {                                           /* past the newest MAX_SOUNDING: fade out over a block */
        int younger = 0;

        for (i = 0; i < EH_VOICES; i++)
            younger += order[i] > order[vi];
        if (younger >= MAX_SOUNDING || (gone & (1 << vi))) {
            if (!(gone & (1 << vi))) {
                mo_render(v, out, TE_VOICE_SAMPLES, inc, &p);
                for (i = 0; i < TE_VOICE_SAMPLES; i++)
                    out[i] = (int16_t)((out[i] * (TE_VOICE_SAMPLES - 1 - i)) / TE_VOICE_SAMPLES);
                gone |= (uint8_t)(1 << vi);
            } else {
                for (i = 0; i < TE_VOICE_SAMPLES; i++)
                    out[i] = 0;
            }
            return;
        }
    }
    mo_render(v, out, TE_VOICE_SAMPLES, inc, &p);
    if (vi == newest)
        last_pitch = v->cur;
}

void mo_setup(void)
{
    int i;

    for (i = 0; i < EH_VOICES; i++)
        mo_voice_init(&voices[i]);
    eh_register(TAG, mo_eh_render);
}

/* Engine browser row "mono 3" (content_sql) -> "drwave" (TE knows no such type: fatal) */
void mo_engine_name(te_string *out, void *db, const char *sql)
{
    static const char name[] = "mono 3";
    char *d = (char *)te_str_data(out);
    unsigned int i;

    (void)db;
    (void)sql;
    for (i = 0; i < sizeof(name) - 1 && i < out->size && d[i] == name[i]; i++)
        ;
    if (out->size == sizeof(name) - 1 && i == out->size) {
        d[0] = 'd'; d[1] = 'r'; d[2] = 'w'; d[3] = 'a'; d[4] = 'v'; d[5] = 'e'; d[6] = 0; /* cap >= 15 */
        out->size = 6;
        mo_page = 0;
        bank_slot = -1;
    }
}

/* ------------------------------------------------------------------ KeyHandler thread */

static volatile int16_t *current(int *slot)
{
    *slot = *TE_SYNTH_SLOT & 7;
    return TE_SYNTH_KNOBS + *slot * 8;
}

static int engine_screen_mono(void)
{
    int slot;

    return *TE_EDIT_MODE == 0 && *TE_SYNTH_SUBSCREEN == 0
           && TE_SYNTH_TYPE[*TE_SYNTH_SLOT & 7] == TE_INSTR_DRWAVE && marked(current(&slot));
}

/* on_key observe (sees every key first): the keyboard's keys, held or released */
void mo_note_key(int key, int down)
{
    static const int8_t note_ids[24] = TE_NOTE_KEY_IDS;
    int i, note, free = -1, was = -1;

    for (note = 0; note < 24 && note_ids[note] != key; note++)
        ;
    if (note == 24)
        return;
    for (i = 0; i < HELD; i++) {
        if (held_id[i] == key) {                /* up (or a repeated down): this press is over */
            was = held_voice[i];
            held_id[i] = 0;
            held_voice[i] = -1;
        }
        if (!held_id[i] && free < 0)
            free = i;
    }
    if (!down && was >= 0)                      /* its voice's contours release */
        req_release |= (uint8_t)(1 << was);
    if (down && free >= 0) {
        held_voice[free] = -1;
        held_note[free] = (int8_t)note;
        held_seq[free] = press_seq++;
        held_id[free] = (int8_t)key;
    }
}

int mo_key(int key, int down)
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
    if (*TE_SHIFT_HELD || !engine_screen_mono())
        return 0;
    mo_page = (uint8_t)(mo_page + 1 < PAGES ? mo_page + 1 : 0);
    touched = 0;
    consumed |= (uint8_t)(1 << b);
    return 1;
}

/* built-in bank: name + the 24 parameters in page order (as the pages show them) */
typedef struct {
    const char *name;
    uint8_t v[NP];
} bank_entry;

static const bank_entry bank[] = {
    /*                cut emph cont gl   r1 w1 v1 nz   r2 f2 w2 v2   r3 f3 w3 v3   fa fd fs kb   la ld ls rl */
    { "FAT BASS",    { 64, 40, 150, 0,   2, 2, 15, 0,  1, 18, 2, 13,  2, 13, 3, 10,  0, 19, 5, 1,   0, 23, 12, 0 } },
    { "PLUCK BASS",  { 45, 70, 165, 0,   2, 3, 15, 0,  2, 18, 2, 12,  0, 15, 0, 0,   0, 17, 1, 1,   0, 22, 0, 1 } },
    { "SUB BASS",    { 110, 0, 60, 0,    1, 0, 15, 0,  2, 15, 0, 11,  0, 15, 0, 0,   0, 20, 8, 2,   0, 24, 14, 0 } },
    { "RESO BASS",   { 30, 190, 175, 4,  2, 2, 15, 0,  2, 17, 2, 15,  0, 15, 0, 0,   0, 17, 2, 1,   0, 21, 9, 0 } },
    { "FUNK BASS",   { 40, 110, 190, 0,  2, 5, 15, 0,  1, 16, 2, 14,  2, 13, 3, 10,  0, 14, 3, 1,   0, 20, 7, 0 } },
    { "TAURUS",      { 52, 40, 115, 5,   1, 2, 15, 0,  1, 17, 2, 14,  2, 13, 2, 12,  8, 21, 6, 1,   4, 24, 13, 1 } },
    { "LUCKY LEAD",  { 115, 90, 90, 9,   3, 3, 15, 0,  3, 17, 3, 13,  4, 14, 3, 8,   10, 20, 10, 2, 0, 24, 15, 1 } },
    { "SAW LEAD",    { 128, 64, 100, 6,  3, 2, 15, 0,  3, 18, 2, 14,  2, 12, 2, 10,  0, 20, 8, 2,   0, 24, 15, 0 } },
    { "SHARK LEAD",  { 115, 100, 105, 7, 3, 1, 15, 0,  3, 12, 1, 13,  2, 18, 1, 11,  4, 19, 7, 2,   0, 24, 15, 0 } },
    { "OCTAVE LEAD", { 122, 60, 95, 6,   2, 2, 14, 0,  3, 17, 2, 14,  4, 14, 3, 11,  0, 21, 9, 2,   0, 24, 15, 0 } },
    { "WHISTLE",     { 88, 236, 40, 8,   3, 0, 4, 0,   3, 15, 0, 0,   3, 15, 0, 0,   12, 22, 12, 3, 10, 20, 4, 1 } },
    { "BRASS",       { 77, 25, 130, 3,   3, 2, 15, 0,  3, 18, 2, 14,  3, 12, 2, 13,  16, 22, 9, 2,  10, 22, 13, 1 } },
    { "FLUTE",       { 118, 38, 45, 4,   3, 0, 15, 20, 4, 15, 0, 5,   0, 15, 0, 0,   14, 22, 12, 3, 15, 22, 15, 1 } },
    { "SOFT PAD",    { 90, 30, 90, 0,    3, 2, 13, 0,  3, 19, 2, 13,  2, 11, 4, 10,  24, 26, 6, 2,  22, 26, 13, 1 } },
    { "ZAP",         { 24, 205, 255, 0,  3, 2, 15, 0,  3, 18, 2, 12,  0, 15, 0, 0,   0, 19, 0, 3,   0, 22, 0, 1 } },
    { "WIND",        { 128, 190, 110, 0, 3, 2, 0, 30,  0, 15, 0, 0,   0, 15, 0, 0,   26, 28, 4, 0,  24, 27, 15, 1 } },
};
#define BANK            ((int)(sizeof(bank) / sizeof(bank[0])))

/* a bank entry into a knob block (bench: the default preset is entry 0) */
void mo_bank_knobs(int e, int16_t *k)
{
    int i;

    for (i = 0; i < 8; i++)
        k[i] = i < 4 ? 0 : eh_p2(TAG, 0);
    for (i = 0; i < NP; i++)
        mo_param_set(k, i, bank[e].v[i]);
}

static void bank_load(volatile int16_t *k, int slot, int e)
{
    int16_t b[8];
    int i;

    mo_bank_knobs(e, b);
    for (i = 0; i < 8; i++)
        k[i] = b[i];
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
    mo_bank = (int8_t)e;
    bank_slot = (int8_t)slot;
    bank_edited = 0;
}

int mo_knob(int knob, int16_t *diff)
{
    int slot, d = *diff, i, id;
    volatile int16_t *k;

    if ((unsigned int)knob > 3 || !engine_screen_mono())
        return 0;
    k = current(&slot);
    if (*TE_SHIFT_HELD) {                       /* drwave's shift + knob: not on a MONO sound */
        if (knob == 0 && d) {                   /* P-- (the sound as it was), P01 .. P16, P-- ... */
            int e = bank_slot == slot ? mo_bank : -1, s = d > 0 ? 1 : -1;

            if (e < 0)                          /* leaving P--: keep the sound as it is now */
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
                mo_bank = -1;
                bank_slot = (int8_t)slot;
                bank_edited = 0;
            }
            title_until = now + TITLE_TIME;
        }
        return 1;
    }
    if (mo_page == FX_PAGE)
        return eh_fx_knob(slot, knob, (int16_t)d);
    touched = (uint8_t)knob;
    bank_edited = 1;
    id = mo_page * 4 + knob;
    if (id <= P_CONTOUR) {                      /* 256 steps, ~100 clicks over the range */
        int step = (d * (STEP_SCALE >> 1)) >> 21;

        step = step ? step : d > 0 ? 1 : d < 0 ? -1 : 0;
        mo_param_set(k, id, mo_param(k, id) + step);
    } else {                                    /* one step per detent, more when fast */
        int step = (d < 0 ? -d : d) >> 10;

        step = step < 1 ? 1 : step;
        mo_param_set(k, id, mo_param(k, id) + (d > 0 ? step : -step));
    }
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
    return 1;
}

/* seq_tick (Seq thread, once per 64-frame audio block): the clock of the name over the panel */
void mo_tick(void)
{
    now += TE_AUDIO_FRAMES;
}

/* ------------------------------------------------------------------ GUI thread */

/* Screen (spec: README.md "Screen"): the pages down the left (the current one red, a dashed line to
 * the display), the 12 x 2 LED display (the knob turned last: its name and value; the oscillator:
 * OSC1 32' TRI; the sound's name for 1 s after it is picked or the screen opens), the DATA knob (that
 * value) with the sounding note; below it the page's four knobs: name and bar (as STATION 1). */

static const char *const labels[NP] = {
    "CUTOFF", "EMPHASIS", "CONTOUR", "GLIDE", "RANGE", "WAVE", "VOLUME", "NOISE",
    "RANGE", "FREQUENCY", "WAVE", "VOLUME", "RANGE", "FREQUENCY", "WAVE", "VOLUME",
    "ATTACK", "DECAY", "SUSTAIN", "KEYBOARD", "ATTACK", "DECAY", "SUSTAIN", "RELEASE",
};
static const char *const page_names[PAGES] = {
    "FILTER", "OSC 1", "OSC 2", "OSC 3", "FILTER CONTOUR", "LOUDNESS CONTOUR", "FX",
};
static const char *const page_list[PAGES] = { "FILTER", "OSC 1", "OSC 2", "OSC 3", "F.CONT", "L.CONT", "FX" };

#define C_BLUE      GFX_RGB(0x6e, 0x8b, 0xff)
#define C_GREEN     GFX_RGB(0x3d, 0xdc, 0x97)
#define C_WHITE     GFX_RGB(0xe6, 0xe6, 0xe6)
#define C_ORANGE    GFX_RGB(0xff, 0x6a, 0x3d)
#define C_RED       GFX_RGB(0xff, 0x4f, 0x5e)
#define C_GREY      GFX_RGB(0x8a, 0x88, 0x98)
#define C_DIM       GFX_RGB(0x5a, 0x58, 0x66)
#define C_LINE      GFX_RGB(0x48, 0x46, 0x50)
#define C_TRACK     GFX_RGB(0x35, 0x32, 0x38)
#define C_LED_OFF   GFX_RGB(0x2a, 0x0a, 0x0e)   /* the LED display: unlit dots, its glass */
#define C_LED_BG    GFX_RGB(0x10, 0x04, 0x05)

static const eh_lcd lcd = { 67, 14, 12, 3, 2, 16, 30, C_RED, C_LED_OFF };
static const char *const ranges[MO_RANGES] = { "LO", "32'", "16'", "8'", "4'", "2'" };

static void line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx - dy, e2;

    for (;;) {
        gfx_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        e2 = 2 * e;
        if (e2 > -dy) { e -= dy; x0 += sx; }
        if (e2 < dx) { e += dx; y0 += sy; }
    }
}

/* sin(5 deg * i) x 100, i = 0 .. 18 */
static const int8_t sn[19] = { 0, 9, 17, 26, 34, 42, 50, 57, 64, 71, 77, 82, 87, 91, 94, 97, 98, 100, 100 };

/* the direction of v (0..32767 = -135 .. +135 deg, 0 = up) x 100 */
static void dir(int v, int *dx, int *dy)
{
    int a = (v < 0 ? 0 : v > 32767 ? 32767 : v) * 270 / 32767 - 135, i = a < 0 ? -a : a;

    *dx = (i >= 90 ? sn[18] : sn[i / 5]) * (a < 0 ? -1 : 1);
    *dy = i >= 90 ? sn[(i - 90) / 5] : -sn[(90 - i) / 5];
}

/* the DATA knob: ticks around it, a white ring, a grey one inside, the red pointer at v */
static void data_knob(int x, int y, int v)
{
    int i, dx, dy;

    for (i = 0; i < 7; i++) {
        dir(i * 32767 / 6, &dx, &dy);
        line(x + dx * 15 / 100, y + dy * 15 / 100, x + dx * 18 / 100, y + dy * 18 / 100, C_DIM);
    }
    gfx_disc(x, y, 12, C_WHITE);
    gfx_disc(x, y, 11, GFX_BLACK);
    gfx_disc(x, y, 8, C_LINE);
    gfx_disc(x, y, 7, GFX_BLACK);
    dir(v, &dx, &dy);
    line(x + dx * 3 / 100, y + dy * 3 / 100, x + dx * 11 / 100, y + dy * 11 / 100, C_RED);
    line(x + dx * 3 / 100 + 1, y + dy * 3 / 100, x + dx * 11 / 100 + 1, y + dy * 11 / 100, C_RED);
}

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

/* 0..max on the panel's 0..10 scale, one decimal: 7.5, 10 */
static char *put_ten(char *d, int v, int max)
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

/* a parameter's value as the display shows it */
static void value(char *d, int id, int v)
{
    static const char *const waves[MO_WAVES] = { "TRI", "SHARK", "SAW", "SQUARE", "WIDE", "NARROW" };
    static const char *const kbds[MO_KBDS] = { "OFF", "1/3", "2/3", "FULL" };
    int c;

    switch (id) {
    case P_GLIDE:
        if (!v)
            put(d, "OFF");
        else
            put_ms(d, mo_glide_ms(v));
        break;
    case P_RANGE1: case P_RANGE2: case P_RANGE3:
        put(d, ranges[v]);
        break;
    case P_WAVE1: case P_WAVE2: case P_WAVE3:
        put(d, id == P_WAVE3 && v == MO_SHARK ? "RAMP" : waves[v]);
        break;
    case P_VOL1: case P_VOL2: case P_VOL3:
        put_ten(d, v, MO_LEVELS - 1);
        break;
    case P_NOISE:                               /* 0 off, 1..15 white, 16..30 pink */
        if (!v)
            put(d, "OFF");
        else
            put_ten(put(d, v > 15 ? "P " : "W "), v > 15 ? v - 15 : v, MO_LEVELS - 1);
        break;
    case P_FREQ2: case P_FREQ3:                 /* semitones, one decimal */
        c = mo_cents(v) / 10;
        d = put(d, c > 0 ? "+" : c < 0 ? "-" : "");
        c = c < 0 ? -c : c;
        put_dec(put(put_dec(d, c / 10), "."), c % 10);
        break;
    case P_FATT: case P_FDEC: case P_LATT: case P_LDEC:
        put_ms(d, (int)(mo_time_10us(v) / 100));
        break;
    case P_FSUS: case P_LSUS:
        put_ten(d, v, 15);
        break;
    case P_KBD:
        put(d, kbds[v]);
        break;
    case P_RELEASE:
        put(d, v ? "ON" : "OFF");
        break;
    default:                                    /* CUTOFF EMPHASIS CONTOUR */
        put_ten(d, v, 255);
        break;
    }
}

/* the lowest sounding note (held keys with their voice's pitch, the notes without a key for FLASH
 * blocks), -1 = none */
static int sounding(void)
{
    uint32_t b = blocks;
    int i, k = -1;

    for (i = 0; i < HELD; i++)
        if (held_id[i] && held_voice[i] >= 0 && (k < 0 || held_key[i] < k))
            k = held_key[i];
    for (i = 0; i < EH_VOICES; i++)
        if ((int32_t)(flash_until[i] - b) > 0 && (k < 0 || flash_key[i] < k))
            k = flash_key[i];
    return k < 0 || k > 127 ? -1 : k;
}

/* the display's lines: the sound while the title time runs and on the FX page (P02 / PLUCK BASS, *
 * = edited; P-- / SHIFT+BLUE), else the knob turned last (CUTOFF 7.5) over the page's oscillator
 * (OSC1 32' TRI) */
static void display(int slot, int pg, const int *u)
{
    static const char *const waves[MO_WAVES] = { "TRI", "SHRK", "SAW", "SQR", "WIDE", "NARW" };
    char t[12];
    int own = mo_bank >= 0 && bank_slot == slot, o = pg >= 1 && pg <= 3 ? pg - 1 : 0, w, id;

    mo_title_on = (int32_t)(title_until - now) > 0;
    eh_lcd_blank(&lcd, 2);
    if (mo_title_on || pg == FX_PAGE) {
        if (own) {
            put_dec(put(t, mo_bank < 9 ? "P0" : "P"), mo_bank + 1);
            eh_lcd_text(&lcd, 0, 0, t, C_RED);
            if (bank_edited)
                eh_lcd_text(&lcd, 0, 11, "*", C_RED);
            eh_lcd_text(&lcd, 1, 0, bank[(int)mo_bank].name, C_RED);
        } else {
            eh_lcd_text(&lcd, 0, 0, "P--", C_RED);
            if (mo_title_on)
                eh_lcd_text(&lcd, 1, 0, "SHIFT+BLUE", C_RED);
        }
        return;
    }
    id = pg * 4 + touched;
    value(t, id, u[id]);
    eh_lcd_text(&lcd, 0, 0, id == P_FREQ2 || id == P_FREQ3 ? "FREQ" : labels[id], C_RED);
    eh_lcd_text(&lcd, 0, 12 - len(t), t, C_RED);
    w = u[o == 0 ? P_WAVE1 : o == 1 ? P_WAVE2 : P_WAVE3];
    put_dec(put(t, "OSC"), o + 1);
    eh_lcd_text(&lcd, 1, 0, t, C_RED);
    eh_lcd_text(&lcd, 1, 5, ranges[u[P_RANGE1 + 4 * o]], C_RED);
    put(t, o == 2 && w == MO_SHARK ? "RAMP" : waves[w]);
    eh_lcd_text(&lcd, 1, 12 - len(t), t, C_RED);
}

void mo_frame(uint16_t *buf)
{
    static const uint16_t col[4] = { C_BLUE, C_GREEN, C_WHITE, C_ORANGE };
    static int8_t shown = -1;                   /* the sound slot the screen showed last frame */
    static const char *const nn[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    volatile int16_t *k;
    int u[NP], slot, i, x, y, pg = mo_page, note, id;

    if (!engine_screen_mono()) {
        shown = -1;
        mo_title_on = 0;
        return;
    }
    k = current(&slot);
    if (slot != shown) {                        /* the screen opens or shows another sound: its name */
        shown = (int8_t)slot;
        title_until = now + TITLE_TIME;
    }
    for (i = 0; i < NP; i++)
        u[i] = mo_param(k, i);

    gfx_begin(buf);
    gfx_rect(0, 0, gfx_w, 140, GFX_BLACK);

    for (i = 0; i < PAGES; i++) {               /* the pages: LED and name, the current one red */
        y = 9 + i * 9;
        gfx_disc(10, y + 2, 2, i == pg ? C_RED : C_GREY);
        x = gfx_text(16, y, page_list[i], i == pg ? C_RED : C_GREY, 1);
        if (i == pg)                            /* a dashed line to the display */
            for (x += 2; x < 58; x += 3)
                gfx_rect(x, y + 2, 2, 1, C_RED);
    }

    eh_frame(60, 4, 204, 70, 6, 2, C_RED, C_LED_BG);
    display(slot, pg, u);

    id = pg == FX_PAGE ? P_CUTOFF : pg * 4 + touched;  /* DATA: the value on the display */
    data_knob(290, 28, u[id] * 32767 / pmax[id]);
    gfx_text(283, 50, "DATA", C_GREY, 1);
    note = sounding();
    mo_note_shown = (int8_t)note;
    if (note >= 0) {                            /* the note sounding (the lowest) */
        gfx_disc(283, 63, 2, C_RED);
        gfx_dec(gfx_text(288, 61, nn[note % 12], C_RED, 1), 61, note / 12 - 1, C_RED, 1);
    } else {
        gfx_disc(283, 63, 2, C_GREY);
        gfx_disc(283, 63, 1, GFX_BLACK);
    }

    /* the page's four knobs: name and bar (as STATION 1) */
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
