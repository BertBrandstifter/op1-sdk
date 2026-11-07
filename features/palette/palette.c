/* PALETTE synth engine (spec: README.md): a four-tone layered digital synth in the style of the
 * JD-800 / JD-08; the OP-1's four knobs are the palette (blue / green / white / orange = tone A-D),
 * each page one parameter row.
 *
 * Runs on the engine host (features/enginehost, extended tag 9). The sound = a patch (bank entry,
 * pl_core.c) + the palette, stored in the sound's 8 knob slots (every value inside drwave's ranges:
 * presets load on stock as drwave): slots 1-4 (tone A-D) = LEVEL in bits 9..13 (TE's LFO moves it),
 * WAVE (5) + CUTOFF (4) below; slots 5-8 (tone A-D) = tag | PITCH (5) + TIME (3) + 4 bits: slot 5 the
 * patch's bits 0-3, slot 6 its bit 4. The render takes the fields from the sound's stored knobs (TE's
 * LFO adds to the knobs it hands over: their low bits would wobble), the levels from TE's.
 *
 * Poly: every TE voice has its own PALETTE voice (pl_core.c); polyphony counts tones, as the JD's:
 * the newest 12 / (tones at LEVEL > 0) notes sound (1-2 tones: all 6, 3 tones: 4, 4 tones: 3; an older
 * one fades out within a block). Key events (on_key observe) pair a held key with the TE voice it starts;
 * its key up releases that voice's TVA / TVF envelopes (a request the render thread applies). Notes
 * without a key (sequencer, MIDI) hold; TE's envelope ends them.
 *
 * pl_setup / pl_engine_name / pl_key / pl_note_key / pl_knob / pl_frame / pl_tick: as MONO 3. */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "pl_core.h"
#include "../enginehost/eh_host.h"

#define KEY_KNOB_PUSH   59                      /* blue 59, green 60, white 61, orange 62 */
#define TAG             9
#define PAGES           6                       /* LEVEL WAVE PITCH CUTOFF TIME + FX */
#define FX_PAGE         5
#define HELD            8
#define FLASH           100                     /* audio blocks: ~145 ms */
#define TITLE_TIME      TE_SAMPLE_RATE          /* the patch's name over the display: 1 s */
#define TONE_BUDGET     12                      /* tones sounding at once (JD style): 4 tones = 3 notes */

/* parameter ids: page * 4 + tone */
enum { P_LEVEL = 0, P_WAVE = 4, P_PITCH = 8, P_CUT = 12, P_TIME = 16, NP = 20 };

static const uint8_t pmax[5] = { PL_LEVELS - 1, PL_WAVES - 1, PL_PITCHES - 1, PL_CUTS - 1, PL_TIMES - 1 };

static pl_voice voices[EH_VOICES];
static uint32_t order[EH_VOICES], notes;
static uint8_t gone;
static volatile int8_t newest = -1;
static volatile uint8_t req_release;            /* KeyHandler -> render, a bit per voice */
static volatile uint32_t blocks;
volatile uint8_t pl_page;                       /* 0..5 = page 1..6 (bench reads) */
static volatile uint8_t consumed;
volatile int8_t pl_bank = -1;                   /* bench reads; -1 = P-- */
static volatile uint8_t bank_edited;
static volatile int8_t bank_slot = -1;
static int16_t own_knobs[8][8];
static volatile uint32_t now;
static volatile uint32_t title_until;
volatile uint8_t pl_title_on;                   /* bench reads */
volatile int8_t pl_note_shown = -1;             /* bench reads */
static volatile uint8_t touched;

static volatile int8_t held_id[HELD];
static volatile int8_t held_note[HELD];
static volatile int8_t held_voice[HELD];
static volatile int8_t held_key[HELD];
static volatile uint8_t held_seq[HELD];
static volatile uint8_t press_seq;
static volatile int8_t flash_key[EH_VOICES];
static volatile uint32_t flash_until[EH_VOICES];

static int marked(const volatile int16_t *k) { return eh_tag(k) == TAG; }

/* ------------------------------------------------------------------ the knob slots */

static int lo9(const volatile int16_t *k, int t) { return k[t] & 511; }
static int lo12(const volatile int16_t *k, int t) { return k[4 + t] & EH_P2_MAX; }

int pl_patch_of(const volatile int16_t *k)
{
    int pa = (lo12(k, 0) >> 8) | ((lo12(k, 1) >> 8 & 1) << 4);

    return pa < pl_npatches ? pa : pl_npatches - 1;
}

/* a parameter as the page shows it (also the bench's) */
int pl_param(const volatile int16_t *k, int id)
{
    int t = id & 3, v;

    switch (id >> 2) {
    case 0:
        v = k[t];
        v = v < 0 ? 0 : v > 16383 ? 16383 : v;
        return v >> 9;
    case 1: return lo9(k, t) & 31;
    case 2: return lo12(k, t) & 31;
    case 3: return lo9(k, t) >> 5;
    default: return (lo12(k, t) >> 5) & 7;
    }
}

void pl_param_set(volatile int16_t *k, int id, int v)
{
    int t = id & 3, pg = id >> 2, w;

    v = v < 0 ? 0 : v > pmax[pg] ? pmax[pg] : v;
    switch (pg) {
    case 0:
        k[t] = (int16_t)((v << 9) | lo9(k, t));
        break;
    case 1:
        k[t] = (int16_t)((pl_param(k, t) << 9) | (lo9(k, t) & ~31) | v);
        break;
    case 3:
        k[t] = (int16_t)((pl_param(k, t) << 9) | (lo9(k, t) & 31) | (v << 5));
        break;
    default:
        w = pg == 2 ? (lo12(k, t) & ~31) | v : (lo12(k, t) & ~(7 << 5)) | (v << 5);
        k[4 + t] = eh_p2s(TAG, t, w);
        break;
    }
}

static void patch_set(volatile int16_t *k, int pa)
{
    k[4] = eh_p2s(TAG, 0, (lo12(k, 0) & 0xff) | ((pa & 15) << 8));
    k[5] = eh_p2s(TAG, 1, (lo12(k, 1) & 0xff) | ((pa >> 4 & 1) << 8));
}

/* core parameters: the fields from the stored knobs st, the levels from TE's (+ LFO) */
static void params(const volatile int16_t *te, const volatile int16_t *st, pl_params *p)
{
    int t;

    p->patch = (uint8_t)pl_patch_of(st);
    for (t = 0; t < PL_TONES; t++) {
        p->level[t] = (uint8_t)pl_param(te, P_LEVEL + t);
        p->wave[t] = (uint8_t)pl_param(st, P_WAVE + t);
        p->pitch[t] = (uint8_t)pl_param(st, P_PITCH + t);
        p->cut[t] = (uint8_t)pl_param(st, P_CUT + t);
        p->time[t] = (uint8_t)pl_param(st, P_TIME + t);
    }
}

/* the stored knobs of the sound a voice plays: slots 5-8 never carry TE's LFO, so they find it */
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

/* a bank entry into a knob block (bench: the default preset is entry 0) */
void pl_bank_knobs(int e, int16_t *k)
{
    pl_params p;
    int t;

    pl_patch_params(e, &p);
    for (t = 0; t < 8; t++)
        k[t] = t < 4 ? 0 : eh_p2s(TAG, t - 4, 0);
    patch_set(k, p.patch);
    for (t = 0; t < PL_TONES; t++) {
        pl_param_set(k, P_LEVEL + t, p.level[t]);
        pl_param_set(k, P_WAVE + t, p.wave[t]);
        pl_param_set(k, P_PITCH + t, p.pitch[t]);
        pl_param_set(k, P_CUT + t, p.cut[t]);
        pl_param_set(k, P_TIME + t, p.time[t]);
    }
}

/* ------------------------------------------------------------------ render thread */

static void pl_eh_render(int vi, int16_t *out, const int16_t *knobs, const uint32_t *inc, int flags)
{
    pl_params p;
    pl_voice *v = &voices[vi];
    int i;

    params(knobs, stored(knobs), &p);
    if (flags & EH_BLOCK)
        blocks++;
    if (flags & EH_FRESH) {                     /* a new note: pair it with its key */
        int oldest = -1;

        for (i = 0; i < HELD; i++)
            if (held_id[i] && held_voice[i] < 0
                && (oldest < 0 || (uint8_t)(held_seq[i] - held_seq[oldest]) > 127))
                oldest = i;
        req_release &= (uint8_t)~(1 << vi);
        pl_note(v, inc[0], &p);
        if (oldest >= 0) {
            int n = held_note[oldest] + 53, d = ((v->key + 128) >> 8) - n;

            d = d >= 0 ? (d + 6) / 12 : -((5 - d) / 12);
            n += 12 * d;
            held_key[oldest] = (int8_t)(n < 0 ? 0 : n > 127 ? 127 : n);
            held_voice[oldest] = (int8_t)vi;
        } else {
            int n = (v->key + 128) >> 8;

            flash_key[vi] = (int8_t)(n < 0 ? 0 : n > 127 ? 127 : n);
            flash_until[vi] = blocks + FLASH;
        }
        newest = (int8_t)vi;
        order[vi] = ++notes;
        gone &= (uint8_t)~(1 << vi);
    }
    if (req_release & (1 << vi)) {
        req_release &= (uint8_t)~(1 << vi);
        pl_release(v, &p);
    }
    {                                           /* past the newest notes the budget allows: fade out */
        int younger = 0, tones = 0, max_sounding;

        for (i = 0; i < PL_TONES; i++)
            tones += p.level[i] > 0;
        max_sounding = tones ? TONE_BUDGET / tones : EH_VOICES;
        max_sounding = max_sounding > EH_VOICES ? EH_VOICES : max_sounding;
        for (i = 0; i < EH_VOICES; i++)
            younger += order[i] > order[vi];
        if (younger >= max_sounding || (gone & (1 << vi))) {
            if (!(gone & (1 << vi))) {
                pl_render(v, out, TE_VOICE_SAMPLES, inc, &p);
                for (i = 0; i < TE_VOICE_SAMPLES; i++)
                    out[i] = (int16_t)((out[i] * (TE_VOICE_SAMPLES - 1 - i)) / TE_VOICE_SAMPLES);
                gone |= (uint8_t)(1 << vi);
                v->on = 0;
            } else {
                for (i = 0; i < TE_VOICE_SAMPLES; i++)
                    out[i] = 0;
            }
            return;
        }
    }
    pl_render(v, out, TE_VOICE_SAMPLES, inc, &p);
}

void pl_setup(void)
{
    int i;

    for (i = 0; i < EH_VOICES; i++)
        pl_voice_init(&voices[i]);
    eh_register(TAG, pl_eh_render);
}

/* Engine browser row "palette" (content_sql) -> "drwave" (TE knows no such type: fatal) */
void pl_engine_name(te_string *out, void *db, const char *sql)
{
    static const char name[] = "palette";
    char *d = (char *)te_str_data(out);
    unsigned int i;

    (void)db;
    (void)sql;
    for (i = 0; i < sizeof(name) - 1 && i < out->size && d[i] == name[i]; i++)
        ;
    if (out->size == sizeof(name) - 1 && i == out->size) {
        d[0] = 'd'; d[1] = 'r'; d[2] = 'w'; d[3] = 'a'; d[4] = 'v'; d[5] = 'e'; d[6] = 0; /* cap >= 15 */
        out->size = 6;
        pl_page = 0;
        bank_slot = -1;
    }
}

/* ------------------------------------------------------------------ KeyHandler thread */

static volatile int16_t *current(int *slot)
{
    *slot = *TE_SYNTH_SLOT & 7;
    return TE_SYNTH_KNOBS + *slot * 8;
}

static int engine_screen_palette(void)
{
    int slot;

    return *TE_EDIT_MODE == 0 && *TE_SYNTH_SUBSCREEN == 0
           && TE_SYNTH_TYPE[*TE_SYNTH_SLOT & 7] == TE_INSTR_DRWAVE && marked(current(&slot));
}

/* on_key observe: the keyboard's keys, held or released */
void pl_note_key(int key, int down)
{
    static const int8_t note_ids[24] = TE_NOTE_KEY_IDS;
    int i, note, free = -1, was = -1;

    for (note = 0; note < 24 && note_ids[note] != key; note++)
        ;
    if (note == 24)
        return;
    for (i = 0; i < HELD; i++) {
        if (held_id[i] == key) {
            was = held_voice[i];
            held_id[i] = 0;
            held_voice[i] = -1;
        }
        if (!held_id[i] && free < 0)
            free = i;
    }
    if (!down && was >= 0)                      /* its voice's envelopes release */
        req_release |= (uint8_t)(1 << was);
    if (down && free >= 0) {
        held_voice[free] = -1;
        held_note[free] = (int8_t)note;
        held_seq[free] = press_seq++;
        held_id[free] = (int8_t)key;
    }
}

int pl_key(int key, int down)
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
    if (*TE_SHIFT_HELD || !engine_screen_palette())
        return 0;
    pl_page = (uint8_t)(pl_page + 1 < PAGES ? pl_page + 1 : 0);
    consumed |= (uint8_t)(1 << b);
    return 1;
}

static void bank_load(volatile int16_t *k, int slot, int e)
{
    int16_t b[8];
    int i;

    pl_bank_knobs(e, b);
    for (i = 0; i < 8; i++)
        k[i] = b[i];
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
    pl_bank = (int8_t)e;
    bank_slot = (int8_t)slot;
    bank_edited = 0;
}

int pl_knob(int knob, int16_t *diff)
{
    int slot, d = *diff, i, id, step;
    volatile int16_t *k;

    if ((unsigned int)knob > 3 || !engine_screen_palette())
        return 0;
    k = current(&slot);
    if (*TE_SHIFT_HELD) {                       /* drwave's shift + knob: not on a PALETTE sound */
        if (knob == 0 && d) {                   /* P-- (the sound as it was), P01 .., P-- ... */
            int e = bank_slot == slot ? pl_bank : -1, s = d > 0 ? 1 : -1;

            if (e < 0)
                for (i = 0; i < 8; i++)
                    own_knobs[slot][i] = k[i];
            e += s;
            e = e >= pl_npatches ? -1 : e < -1 ? pl_npatches - 1 : e;
            if (e >= 0) {
                bank_load(k, slot, e);
            } else {
                for (i = 0; i < 8; i++)
                    k[i] = own_knobs[slot][i];
                TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
                pl_bank = -1;
                bank_slot = (int8_t)slot;
                bank_edited = 0;
            }
            title_until = now + TITLE_TIME;
        }
        return 1;
    }
    if (pl_page == FX_PAGE)
        return eh_fx_knob(slot, knob, (int16_t)d);
    touched = (uint8_t)knob;
    bank_edited = 1;
    id = pl_page * 4 + knob;
    step = (d < 0 ? -d : d) >> 10;              /* one step per detent, more when fast */
    step = step < 1 ? 1 : step;
    pl_param_set(k, id, pl_param(k, id) + (d > 0 ? step : -step));
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
    return 1;
}

/* seq_tick (once per 64-frame audio block): the clock of the patch's name */
void pl_tick(void)
{
    now += TE_AUDIO_FRAMES;
}

/* ------------------------------------------------------------------ GUI thread */

/* Screen (spec: README.md "Screen"): a green LCD 20 x 2 (the patch or the knob turned last over
 * the page's value of the four tones), the page names under it, the four palette sliders A-D with
 * a lamp per tone to the right; below them the page's four knobs: name and bar (as MONO 3). */

#define C_BLUE      GFX_RGB(0x6e, 0x8b, 0xff)
#define C_GREEN     GFX_RGB(0x3d, 0xdc, 0x97)
#define C_WHITE     GFX_RGB(0xe6, 0xe6, 0xe6)
#define C_ORANGE    GFX_RGB(0xff, 0x6a, 0x3d)
#define C_GREY      GFX_RGB(0x8a, 0x88, 0x98)
#define C_DIM       GFX_RGB(0x5a, 0x58, 0x66)
#define C_TRACK     GFX_RGB(0x35, 0x32, 0x38)
#define C_LCD_ON    GFX_RGB(0xa6, 0xf2, 0x6e)   /* the LCD: lit dots, unlit dots, glass, bezel */
#define C_LCD_OFF   GFX_RGB(0x16, 0x2c, 0x14)
#define C_LCD_BG    GFX_RGB(0x08, 0x13, 0x08)
#define C_LCD_RIM   GFX_RGB(0x4e, 0x7a, 0x3a)

static const eh_lcd lcd = { 13, 15, 20, 2, 1, 10, 18, C_LCD_ON, C_LCD_OFF };
static const char *const page_names[PAGES] = { "LEVEL", "WAVE", "PITCH", "CUTOFF", "TIME", "FX" };
static const char *const tone_names[4] = { "TONE A", "TONE B", "TONE C", "TONE D" };

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

/* a parameter's value as the display shows it (at most 4 characters: a column of 5 with its gap) */
static void value(char *d, int pg, int v)
{
    static const char *const times[PL_TIMES] = { "x.25", "x0.5", "x0.7", "x1", "x1.4", "x2", "x3", "x4" };
    int x;

    switch (pg) {
    case 0:
        put_dec(d, pl_level_pct(v));
        break;
    case 1:
        put(d, pl_wave_short[v < PL_WAVES ? v : 0]);
        break;
    case 2:
        x = pl_pitch_semis(v);
        put_dec(put(d, x > 0 ? "+" : ""), x);
        break;
    case 3:
        x = pl_cut_offset(v);
        put_dec(put(d, x > 0 ? "+" : ""), x);
        break;
    default:
        put(d, times[v & 7]);
        break;
    }
}

/* the lowest sounding note (held keys, the notes without a key for FLASH blocks), -1 = none */
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

static void display(volatile int16_t *k, int slot, int pg)
{
    char t[24];
    int i, pa = pl_patch_of(k);

    pl_title_on = (int32_t)(title_until - now) > 0;
    eh_lcd_blank(&lcd, 2);
    if (pl_title_on || pg == FX_PAGE) {         /* P07 GLASS BELLS * */
        char *e = put(t, pl_bank >= 0 && bank_slot == slot ? (pl_bank < 9 ? "P0" : "P") : "P--");

        if (pl_bank >= 0 && bank_slot == slot)
            put_dec(e, pl_bank + 1);
        eh_lcd_text(&lcd, 0, 0, t, C_LCD_ON);
        eh_lcd_text(&lcd, 0, 4, pl_patches[pa].name, C_LCD_ON);
        if (bank_edited && pl_bank >= 0 && bank_slot == slot)
            eh_lcd_text(&lcd, 0, 19, "*", C_LCD_ON);
    } else {                                    /* CUTOFF B        +15 */
        put(put(t, page_names[pg]), " ");
        t[len(t) + 1] = 0;
        t[len(t)] = (char)('A' + touched);
        eh_lcd_text(&lcd, 0, 0, t, C_LCD_ON);
        value(t, pg, pl_param(k, pg * 4 + touched));
        eh_lcd_text(&lcd, 0, 20 - len(t), t, C_LCD_ON);
    }
    if (pg == FX_PAGE) {
        eh_lcd_text(&lcd, 1, 0, "FX: THE SOUND'S FX", C_LCD_ON);
        return;
    }
    for (i = 0; i < PL_TONES; i++) {            /* the page's value of the four tones */
        value(t, pg, pl_param(k, pg * 4 + i));
        eh_lcd_text(&lcd, 1, i * 5, t, C_LCD_ON);
    }
}

void pl_frame(uint16_t *buf)
{
    static const uint16_t col[4] = { C_BLUE, C_GREEN, C_WHITE, C_ORANGE };
    static int8_t shown = -1;
    static const char *const nn[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    volatile int16_t *k;
    int slot, i, x, pg = pl_page, note, id, pv = pg < FX_PAGE ? pg : 0;

    if (!engine_screen_palette()) {
        shown = -1;
        pl_title_on = 0;
        return;
    }
    k = current(&slot);
    if (slot != shown) {
        shown = (int8_t)slot;
        title_until = now + TITLE_TIME;
    }

    gfx_begin(buf);
    eh_aa_begin(buf);
    gfx_rect(0, 0, gfx_w, 140, GFX_BLACK);

    eh_frame(4, 5, 214, 50, 6, 2, C_LCD_RIM, C_LCD_BG);
    display(k, slot, pg);

    for (i = 0, x = 6; i < PAGES; i++) {        /* the pages, the current one lit */
        x = gfx_text(x, 63, page_names[i], i == pg ? C_LCD_ON : C_DIM, 1) + 7;
    }
    note = sounding();
    pl_note_shown = (int8_t)note;
    x = gfx_text(6, 78, "KEY ", C_DIM, 1);
    if (note >= 0)
        gfx_dec(gfx_text(x, 78, nn[note % 12], C_LCD_ON, 1), 78, note / 12 - 1, C_LCD_ON, 1);
    else
        gfx_text(x, 78, "--", C_DIM, 1);
    x = gfx_text(80, 78, "PATCH ", C_DIM, 1);
    gfx_text(x, 78, pl_patches[pl_patch_of(k)].name, C_GREY, 1);

    for (i = 0; i < PL_TONES; i++) {            /* the palette: four sliders, a lamp per tone */
        int sx = 236 + i * 22, v = pl_param(k, pv * 4 + i), y = 70 - v * 58 / pmax[pv];
        int on = pl_param(k, P_LEVEL + i) > 0;
        char l[2] = { (char)('A' + i), 0 };

        eh_aa_box(sx - 1, 10, 4, 62, 2, C_TRACK);
        eh_aa_box(sx - 7, y - 3, 16, 7, 3, pg == FX_PAGE ? C_DIM : col[i]);
        gfx_rect(sx - 4, y, 10, 1, GFX_BLACK);
        gfx_text(sx - 2, 77, l, on ? C_WHITE : C_DIM, 1);
        eh_aa_ellipse((sx + 1) * 16, 89 * 16, 40, 40, -1, 0, 128, 0, on && note >= 0 ? C_LCD_ON : C_TRACK);
    }

    if (pg == FX_PAGE) {                        /* the sound's FX knobs (engine host) */
        eh_fx_draw(slot, 97);
        return;
    }
    for (i = 0; i < 4; i++) {                   /* the page's four knobs: name and bar */
        id = pg * 4 + i;
        x = 4 + i * 80;
        gfx_text(x, 99, tone_names[i], col[i], 2);
        gfx_rect(x, 112, 72, 6, C_TRACK);
        gfx_rect(x, 112, pl_param(k, id) * 72 / pmax[pg], 6, col[i]);
    }
    x = gfx_text(4, 125, "PUSH: ", C_DIM, 1);
    gfx_text(x, 125, page_names[(pg + 1) % PAGES], C_DIM, 1);
}
