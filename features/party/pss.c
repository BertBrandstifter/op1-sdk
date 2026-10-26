/* PSS-11 synth engine (feature pss; spec: README.md): two multisounds layered, into a
 * low-pass filter with its own envelope.
 *
 * Runs on the engine host (features/enginehost, tag 1). Page 1 (WAVE A, WAVE B, MIX, CUTOFF) lives
 * in slots 1-4 as 0..16383, so TE's LFO can move them. Slots 5-8 hold 0x1000 | two 6-bit fields
 * each: the high field = page 2 (B PITCH, DETUNE, F.ENV, F.DECAY), the low one = page 3 (RES,
 * KEY TRK, A DECAY, B FADE). Every value is inside drwave's ranges (boot check passes, loads on
 * stock as drwave).
 *
 * pss_init / pss_engine_name / pss_key / pss_knob / pss_frame: as TRIO (trio.c), with 4 pages (page 4 =
 * the sound's FX knobs), WAVE A / B one multisound per detent, and a bank on shift + blue turn
 * (39 base sounds with ROM data, 8 with the built-in waves), with P-- (the sound as it was before
 * browsing) in front of P01.
 * pss_main (main loop): loads the PSS-11 ROM's sample data (a file of the update) into TE's sample
 * memory the first time a sound slot holds a PSS-11 sound.
 * pss_tick (seq_tick) is the clock of the sound's name over the voice list. */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "pss_core.h"
#include "../enginehost/eh_host.h"

#define KEY_KNOB_PUSH   59                      /* blue 59, green 60, white 61, orange 62 */
#define STEP_SCALE      10485                   /* ~100 clicks full range (as engine_plus) */
#define TAG             1
#define PAGES           4
#define OFF             (-1)
#define TITLE_TIME      TE_SAMPLE_RATE          /* the sound's name over the voice list: 1 s */

static pss_voice voices[EH_VOICES];
/* the keyboard picture: a key held on the OP-1 (pss_note_key) is its note (TE plays note index + 53 +
 * 12 x octave); the next new voice tells the octave (its pitch rounded to the nearest octave of the
 * note: glide / LFO / bend at the start move it by less than 6 semitones). New voices no held key
 * accounts for (sequencer, MIDI, CHORD+'s extra notes) flash at their pitch for FLASH blocks. TE keeps
 * rendering a voice through its release and beyond, so rendering does not mean sounding. */
#define HELD        8
#define FLASH       100                         /* audio blocks: ~145 ms */
static volatile uint32_t blocks;                /* audio blocks with a PSS voice (render thread) */
static volatile int8_t held_id[HELD];           /* key id held, 0 = free (KeyHandler thread) */
static volatile int8_t held_note[HELD];         /* its note index 0..23 */
static volatile int8_t held_key[HELD];          /* its MIDI key, -1 = no voice yet (render thread) */
static volatile uint8_t held_seq[HELD];         /* press order */
static volatile uint8_t press_seq;
static volatile int8_t flash_key[EH_VOICES];
static volatile uint32_t flash_until[EH_VOICES];
volatile uint8_t pss_page;                       /* 0..3 = page 1..4 (bench reads) */
static volatile uint8_t consumed;
volatile int16_t pss_bank = -1;                 /* bench reads; the ROM bank has up to 512 */
static volatile uint8_t bank_edited;
static volatile int8_t bank_slot = -1;
static volatile uint8_t own_edited;             /* per sound slot (bit): P-- changed since shown */
static int16_t own_knobs[8][8];                 /* per sound slot: P--, the sound before the bank */
static volatile uint32_t now;                   /* samples, advanced by pss_tick */
static volatile uint32_t title_until;           /* the sound's name shown while now < this */
volatile uint8_t pss_title_on;                  /* drawn in the last frame (bench reads) */

static const uint8_t hi_max[4] = { 48, 63, 63, 63 };   /* B PITCH = semitones + 24 */

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
    static const uint8_t lo_param[4] = { PSS_RES, PSS_KEYTRK, PSS_ADECAY, PSS_BFADE };
    int i, v;

    for (i = 0; i < 4; i++) {
        v = k[i];                               /* TE's LFO adds to slots 1-4 (saturating) */
        v = v < 0 ? 0 : v > EH_PAGE1_MAX ? EH_PAGE1_MAX : v;
        p[i] = (int16_t)(v << 1);
        p[lo_param[i]] = (int16_t)(field(k, i, 0) * 520);
    }
    v = field(k, 0, 1);
    p[PSS_BPITCH] = (int16_t)(((2 * (v > 48 ? 48 : v) + 1) * 32768) / 98);   /* zone centre of 49 */
    p[PSS_DETUNE] = (int16_t)(field(k, 1, 1) * 520);
    p[PSS_FENV] = (int16_t)(field(k, 2, 1) * 520);
    p[PSS_FDECAY] = (int16_t)(field(k, 3, 1) * 520);
}

static void pss_eh_render(int vi, int16_t *out, const int16_t *knobs, const uint32_t *inc, int flags)
{
    int16_t p[PSS_PARAMS];

    unpack(knobs, p);
    if (flags & EH_BLOCK)
        blocks++;
    if (flags & EH_FRESH) {
        int i, oldest = -1;

        pss_note_on(&voices[vi], inc[0], p);
        for (i = 0; i < HELD; i++)              /* the oldest held key without a voice gets this one */
            if (held_id[i] && held_key[i] < 0
                && (oldest < 0 || (uint8_t)(held_seq[i] - held_seq[oldest]) > 127))
                oldest = i;
        if (oldest >= 0) {
            int n = held_note[oldest] + 53, d = voices[vi].key - n;

            d = d >= 0 ? (d + 6) / 12 : -((5 - d) / 12);    /* octaves, rounded */
            n += 12 * d;
            held_key[oldest] = (int8_t)(n < 0 ? 0 : n > 127 ? 127 : n);
        } else {
            flash_key[vi] = (int8_t)voices[vi].key;
            flash_until[vi] = blocks + FLASH;
        }
    }
    pss_render(&voices[vi], out, TE_VOICE_SAMPLES, inc, p);
}

void pss_init(void)
{
    eh_register(TAG, pss_eh_render);
}

/* Engine browser row "party" (content_sql) -> "drwave" (TE knows no such type: fatal) */
void pss_engine_name(te_string *out, void *db, const char *sql)
{
    static const char name[] = "party";
    char *d = (char *)te_str_data(out);
    unsigned int i;

    (void)db;
    (void)sql;
    for (i = 0; i < sizeof(name) - 1 && i < out->size && d[i] == name[i]; i++)
        ;
    if (out->size == sizeof(name) - 1 && i == out->size) {
        d[0] = 'd'; d[1] = 'r'; d[2] = 'w'; d[3] = 'a'; d[4] = 'v'; d[5] = 'e'; d[6] = 0; /* cap >= 15 */
        out->size = 6;
        pss_page = 0;
        bank_slot = -1;
    }
}

/* ------------------------------------------------------------------ main thread: the sample data */

/* The PSS-11 ROM's samples are not code: they are a file of the update (content/
 * pss_rom.bin, gen_pss.py) read at runtime into TE's sample memory (VisualDSP heap 1, as
 * SampleBuffer.cpp allocates a sample), so they cost nothing of the 2 MB SDK region. The first
 * time a sound slot holds a PSS-11 sound the file is opened, the buffer allocated, then read CHUNK
 * bytes per main-loop pass (the UI keeps running), checked against the build's sum and handed to
 * pss_core; until then notes are silent and the screen says why. The buffer stays until power-off.
 * No room in TE's sample memory (samples fill it): retried every ~2 s. */
enum { LD_IDLE, LD_READING, LD_READY, LD_NOFILE, LD_NOMEM, LD_BAD };
#define CHUNK       0x10000                     /* bytes per main-loop pass */
#define RETRY       250                         /* main-loop passes (~8 ms each) */
volatile uint8_t pss_load = LD_IDLE;             /* bench reads */
static uint8_t *ld_buf;
static int ld_fd = -1;
static uint32_t ld_pos;
static uint16_t ld_wait;

static int any_pss_sound(void)
{
    int s;

    for (s = 0; s < 8; s++)
        if (TE_SYNTH_TYPE[s] == TE_INSTR_DRWAVE && marked(TE_SYNTH_KNOBS + s * 8))
            return 1;
    return 0;
}

static uint32_t rom_sum(const uint8_t *d, uint32_t n)  /* as gen_pss.py rom_sum */
{
    const uint32_t *w = (const uint32_t *)(const void *)d;
    uint32_t h = 0x811c9dc5u, i;

    for (i = 0; i < n / 4; i++)
        h = ((h << 5) | (h >> 27)) ^ w[i];
    return h;
}

void pss_main(void)
{
    uint32_t n = pss_rom_file_bytes();
    int r;

    if (!pss_rom_file() || pss_load == LD_READY || pss_load == LD_NOFILE || pss_load == LD_BAD)
        return;
    if (pss_load != LD_READING) {
        if (pss_load == LD_NOMEM && ++ld_wait < RETRY)
            return;
        ld_wait = 0;
        if (!any_pss_sound())
            return;
        ld_fd = TE_FILE_OPEN(pss_rom_file(), TE_FILE_RDONLY, TE_FILE_MODE);
        if (ld_fd < 0) {
            pss_load = LD_NOFILE;
            return;
        }
        if (!ld_buf)
            ld_buf = TE_HEAP_MALLOC(TE_HEAP_SAMPLES, n);
        if (!ld_buf) {
            TE_FILE_CLOSE(ld_fd);
            pss_load = LD_NOMEM;
            return;
        }
        ld_pos = 0;
        pss_load = LD_READING;
    }
    r = TE_FILE_READ(ld_fd, ld_buf + ld_pos, n - ld_pos < CHUNK ? n - ld_pos : CHUNK);
    if (r > 0)
        ld_pos += (uint32_t)r;
    if (r > 0 && ld_pos < n)
        return;
    TE_FILE_CLOSE(ld_fd);
    TE_DCACHE_FLUSH(ld_buf, ld_buf + n, 1);     /* the reader may have used DMA */
    if (ld_pos != n || rom_sum(ld_buf, n) != pss_rom_file_sum()) {
        pss_load = LD_BAD;
        return;
    }
    pss_set_rom(ld_buf);
    pss_load = LD_READY;
}

/* the loader's state on the LCD's second line (0 = nothing to say) */
static const char *load_text(char *t)
{
    static const char *const msg[] = {
        "LOADING SAMPLES", 0, 0, "SAMPLE FILE MISSING", "NO SAMPLE MEMORY FREE", "SAMPLE FILE DAMAGED",
    };
    uint32_t pc;
    int i = 0;

    if (!pss_rom_file() || pss_load == LD_READY)
        return 0;
    if (pss_load != LD_READING)
        return msg[pss_load];
    pc = ld_pos / (pss_rom_file_bytes() / 100 + 1);
    for (; msg[0][i]; i++)
        t[i] = msg[0][i];
    t[i++] = ' ';
    if (pc >= 10)
        t[i++] = (char)('0' + pc / 10 % 10);
    t[i++] = (char)('0' + pc % 10);
    t[i++] = '/';                               /* the font has no % */
    t[i++] = '1';
    t[i++] = '0';
    t[i++] = '0';
    t[i] = 0;
    return t;
}

/* ------------------------------------------------------------------ KeyHandler thread */

static volatile int16_t *current(int *slot)
{
    *slot = *TE_SYNTH_SLOT & 7;
    return TE_SYNTH_KNOBS + *slot * 8;
}

static int engine_screen_pss(void)
{
    int slot;

    return *TE_EDIT_MODE == 0 && *TE_SYNTH_SUBSCREEN == 0
           && TE_SYNTH_TYPE[*TE_SYNTH_SLOT & 7] == TE_INSTR_DRWAVE && marked(current(&slot));
}

/* on_key observe (KeyHandler thread, sees every key first): the keyboard's keys, held or released */
void pss_note_key(int key, int down)
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

int pss_key(int key, int down)
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
    if (*TE_SHIFT_HELD || !engine_screen_pss())
        return 0;
    pss_page = (uint8_t)(pss_page + 1 < PAGES ? pss_page + 1 : 0);
    consumed |= (uint8_t)(1 << b);
    return 1;
}

/* built-in bank: name, wave A, wave B (OFF), mix %, cutoff %, B pitch (semitones), detune, F.ENV %,
 * F.DECAY %, res %, key track %, A decay (amount: 0 = hold, 63 = fastest), B fade (0 = instant)
 * (6-bit steps), over the built-in waves. With a GEW7 ROM's samples the bank is the ROM's sounds
 * themselves, one per sample-table entry (S000 ..), each as the keyboard plays it: wave A alone with
 * its own AWM envelope and LFO, through the keyboard's output low-pass (which voice uses which entry
 * is in the firmware). */
typedef struct {
    const char *name;
    int16_t a;
    int8_t b, mix, cut, bpitch, det, fenv, fdec, res, ktrk, adec, bfade;
} bank_entry;

static const bank_entry bank_builtin[] = {
    { "PSS PIANO",    6, OFF, 0,  100, 0,   0,  0,  0,  0,  20, 28, 0 },
    { "E.PIANO",      6, 4,   20, 90,  12,  0,  0,  0,  0,  20, 28, 0 },
    { "ORGAN",        5, 5,   50, 100, -12, 0,  0,  0,  0,  0,  0,  0 },
    { "STRINGS",      0, 0,   50, 70,  0,   20, 0,  0,  0,  0,  0,  34 },
    { "BRASS",        0, OFF, 0,  60,  0,   0,  45, 30, 10, 30, 0,  0 },
    { "E.BASS",       1, OFF, 0,  40, -12,  0,  60, 25, 35, 30, 0,  0 },
    { "FLUTE",        4, 4,   30, 85,  12,  0,  0,  0,  0,  0,  0,  20 },
    { "VOICES",       7, 7,   50, 85,  0,   20, 0,  0,  0,  0,  0,  20 },
};

static int bank_n(void)
{
    return pss_have_rom() ? pss_nwaves() : (int)(sizeof(bank_builtin) / sizeof(bank_builtin[0]));
}

/* entry e (0 .. bank_n() - 1); a ROM sound is made on the spot (GUI and KeyHandler threads: each its
 * own copy) */
static const bank_entry *entry(int e, bank_entry *tmp)
{
    if (!pss_have_rom())
        return &bank_builtin[e];
    tmp->name = pss_wave_name(e);
    tmp->a = (int16_t)e;
    tmp->b = OFF;
    tmp->mix = tmp->bpitch = tmp->det = tmp->fenv = tmp->fdec = tmp->ktrk = tmp->adec = tmp->bfade = 0;
    tmp->cut = 81;                              /* the keyboard's output low-pass (MAME ympsr150.cpp: RLC, */
    tmp->res = 29;                              /* R 150, L 3.3 mH, C 0.33 uF): 4.8 kHz, Q 0.67 */
    return tmp;
}

static const char *bank_name(int e)
{
    return pss_have_rom() ? pss_wave_name(e) : bank_builtin[e].name;
}

/* page-1 value at the centre of zone z of n (0..16383) */
static int16_t zone1(int z, int n) { return (int16_t)(((2 * z + 1) * 32768 / (2 * n)) >> 1); }

static void bank_load(volatile int16_t *k, int slot, int e)
{
    bank_entry tmp;
    const bank_entry *b = entry(e, &tmp);
    int nw = pss_nwaves();

    k[0] = zone1(b->a < nw ? b->a : 0, nw);
    k[1] = zone1(b->b == OFF || b->b >= nw ? 0 : b->b + 1, nw + 1);
    k[2] = (int16_t)(b->mix * EH_PAGE1_MAX / 100);
    k[3] = (int16_t)(b->cut * EH_PAGE1_MAX / 100);
    k[4] = pack(b->bpitch + 24, b->res * 63 / 100);
    k[5] = pack(b->det, b->ktrk * 63 / 100);
    k[6] = pack(b->fenv * 63 / 100, b->adec);
    k[7] = pack(b->fdec * 63 / 100, b->bfade);
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
    pss_bank = (int16_t)e;
    bank_slot = (int8_t)slot;
    bank_edited = 0;
}

int pss_knob(int knob, int16_t *diff)
{
    int slot, v, d = *diff;
    volatile int16_t *k;

    if ((unsigned int)knob > 3 || !engine_screen_pss())
        return 0;
    k = current(&slot);
    if (*TE_SHIFT_HELD) {                       /* drwave's shift + knob: not on a PSS-11 sound */
        if (knob == 0 && d) {                   /* P-- (the sound as it was), P01 .. Pnn, P-- ... */
            int n, e = bank_slot == slot ? pss_bank : -1, s = d > 0 ? 1 : -1, i;

            n = bank_n();
            if (e < 0)                          /* leaving P--: keep the sound as it is now */
                for (i = 0; i < 8; i++)
                    own_knobs[slot][i] = k[i];
            e += s;
            e = e >= n ? -1 : e < -1 ? n - 1 : e;
            if (e >= 0) {
                bank_load(k, slot, e);
            } else {
                for (i = 0; i < 8; i++)
                    k[i] = own_knobs[slot][i];
                TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
                pss_bank = -1;
                bank_slot = (int8_t)slot;
                bank_edited = 0;
                own_edited &= (uint8_t)~(1u << slot);
            }
            title_until = now + TITLE_TIME;
        }
        return 1;
    }
    if (pss_page == 3)
        return eh_fx_knob(slot, knob, (int16_t)d);
    bank_edited = 1;
    if (!(pss_bank >= 0 && bank_slot == slot))
        own_edited |= (uint8_t)(1u << slot);
    if (pss_page == 0 && knob < 2) {             /* WAVE A / B: one multisound per detent */
        int n = pss_nwaves() + knob, z = (k[knob] < 0 ? 0 : k[knob]) * 2 * n >> 15;
        int step = (d < 0 ? -d : d) >> 10;

        step = step < 1 ? 1 : step;
        z += d > 0 ? step : -step;
        k[knob] = zone1(z < 0 ? 0 : z >= n ? n - 1 : z, n);
        title_until = now + TITLE_TIME;         /* the waves' names, as on STATION 1's LCD */
    } else if (pss_page == 0) {
        v = k[knob] + ((d * (STEP_SCALE >> 1)) >> 15);
        k[knob] = (int16_t)(v < 0 ? 0 : v > EH_PAGE1_MAX ? EH_PAGE1_MAX : v);
    } else {                                    /* 6-bit fields: one step per detent, more when fast */
        int hi = pss_page == 1, max = hi ? hi_max[knob] : 63;
        int step = (d < 0 ? -d : d) >> 10;

        step = step < 1 ? 1 : step;
        v = field(k, knob, hi) + (d > 0 ? step : -step);
        v = v < 0 ? 0 : v > max ? max : v;
        k[4 + knob] = hi ? pack(v, field(k, knob, 0)) : pack(field(k, knob, 1), v);
    }
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
    return 1;
}

/* seq_tick (Seq thread, once per 64-frame audio block): the clock of the name over the voice list */
void pss_tick(void)
{
    now += TE_AUDIO_FRAMES;
}

/* ------------------------------------------------------------------ GUI thread */

/* Screen (spec: README.md "Screen"): the PARTY keyboard after "PARTY · overview", drawn live: the
 * voice list (the bank, 30 sounds a window, the sound in blue), the strip (the page, the sound's
 * name), the knob = the blue knob of this page, the 12 buttons = the 12 parameters (lit when on, the
 * page's column outlined), B on / MIX, the speaker and a 32-key keyboard (F3 .. C6) lighting the
 * sounding keys; for 1 s after a sound or wave is picked (or the screen opens) the sound over the
 * voice list, as STATION 1's LCD; below it the page's four knobs as on every SDK engine. */

static const char *const labels[12] = {
    "WAVE A", "WAVE B", "MIX", "CUTOFF", "B PITCH", "DETUNE", "F.ENV", "F.DECAY",
    "RES", "KEY TRK", "A DECAY", "B FADE",
};
static const char *const page_names[PAGES] = { "WAVES", "PITCH+FILTER", "SHAPE", "FX" };

#define C_BLUE      GFX_RGB(0x6e, 0x8b, 0xff)
#define C_GREEN     GFX_RGB(0x3d, 0xdc, 0x97)
#define C_WHITE     GFX_RGB(0xe6, 0xe6, 0xe6)
#define C_ORANGE    GFX_RGB(0xff, 0x6a, 0x3d)
#define C_RED       GFX_RGB(0xff, 0x4f, 0x5e)
#define C_DIM       GFX_RGB(0x5a, 0x58, 0x66)
#define C_LINE      GFX_RGB(0x48, 0x46, 0x50)
#define C_TRACK     GFX_RGB(0x35, 0x32, 0x38)
#define C_TEAL      GFX_RGB(0x3f, 0xd0, 0xd4)   /* the case */

#define KEY_LO      53                          /* keyboard F3 .. C6: 32 keys, 19 white (PSS-11) */
#define KEYS        32
#define KEY_X       40
#define KEY_W       10
#define LIST        30                          /* voice list: 6 columns of 5 */

/* 1-pixel outline with cut corners (r = 0: square) */
static void outline(int x, int y, int w, int h, int r, uint16_t c)
{
    int i;

    gfx_rect(x + r, y, w - 2 * r, 1, c);
    gfx_rect(x + r, y + h - 1, w - 2 * r, 1, c);
    gfx_rect(x, y + r, 1, h - 2 * r, c);
    gfx_rect(x + w - 1, y + r, 1, h - 2 * r, c);
    for (i = 1; i < r; i++) {                   /* the corners as short diagonals */
        gfx_pixel(x + r - i, y + i, c);
        gfx_pixel(x + w - 1 - r + i, y + i, c);
        gfx_pixel(x + r - i, y + h - 1 - i, c);
        gfx_pixel(x + w - 1 - r + i, y + h - 1 - i, c);
    }
}

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

static int text_w(const char *s, int scale)    /* 4 px a character at scale 1 */
{
    int n = 0;

    while (s[n])
        n++;
    return n ? n * 4 * scale - scale : 0;
}

static void light(uint8_t *on, int k)          /* folded into F3 .. C6 */
{
    if (k < 0)
        return;
    while (k < KEY_LO)
        k += 12;
    while (k >= KEY_LO + KEYS)
        k -= 12;
    on[k - KEY_LO] = 1;
}

/* the keys held (with their voice's pitch) and the voices flashing */
static void sounding(uint8_t *on)
{
    uint32_t now_b = blocks;
    int i;

    for (i = 0; i < KEYS; i++)
        on[i] = 0;
    for (i = 0; i < HELD; i++)
        if (held_id[i])
            light(on, held_key[i]);
    for (i = 0; i < EH_VOICES; i++)
        if ((int32_t)(flash_until[i] - now_b) > 0)
            light(on, flash_key[i]);
}

static void keyboard(int y0, int y1)
{
    static const int8_t white_of[12] = { 0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6 };   /* from F */
    uint8_t on[KEYS];
    int k, split = y0 + (y1 - y0) * 3 / 5;

    sounding(on);
    gfx_rect(KEY_X, y0, 19 * KEY_W + 1, 1, C_WHITE);
    for (k = 0; k <= 19; k++)                   /* white key borders */
        gfx_rect(KEY_X + k * KEY_W, y0 + 1, 1, y1 - y0 - 1, C_LINE);
    for (k = 0; k < KEYS; k++) {
        int o = k / 12, n = k % 12, w = white_of[n];

        if (w >= 0) {                           /* a sounding white key: its lower part */
            if (on[k])
                gfx_rect(KEY_X + (o * 7 + w) * KEY_W + 1, split, KEY_W - 1, y1 - split, C_GREEN);
        } else {                                /* black keys between the white ones */
            int x = KEY_X + (o * 7 + white_of[n - 1] + 1) * KEY_W - 3;

            gfx_rect(x, y0 + 1, 6, split - y0 - 1, on[k] ? C_GREEN : C_WHITE);
        }
    }
    for (k = 0; k < 19; k += 2)                 /* the dots above every other white key */
        gfx_pixel(KEY_X + k * KEY_W + KEY_W / 2, y0 - 2, C_DIM);
}

/* the voice list: the bank in columns of 5 (header line, a line per sound as long as its name), the
 * window of LIST holding the sound; the sound itself as a blue bar */
static void voice_list(int n, int e)
{
    int i, first = e < 0 ? 0 : e / LIST * LIST;

    outline(39, 8, 186, 27, 2, C_LINE);
    for (i = 0; i < 6; i++) {
        int x = 43 + i * 31;

        if (i)
            gfx_rect(39 + i * 31, 11, 1, 21, C_GREEN);
        gfx_rect(x, 12, 13, 1, C_GREEN);
    }
    for (i = 0; i < LIST && first + i < n; i++) {
        int x = 43 + i / 5 * 31, y = 17 + i % 5 * 3, w = text_w(bank_name(first + i), 1) / 2 + 4;

        if (first + i == e)
            gfx_rect(x, y - 1, 24, 3, C_BLUE);
        else
            gfx_rect(x, y, w > 23 ? 23 : w, 1, C_DIM);
    }
}

/* the knob: the value 0..32767 as -135 .. +135 degrees */
static void knob(int cx, int cy, int q)
{
    static const int8_t sn[19] = { 0, 9, 17, 26, 34, 42, 50, 57, 64, 71, 77, 82, 87, 91, 94, 97, 98,
                                   100, 100 };   /* sin(5 deg * i) x 100, i = 0 .. 18 */
    int a = q * 270 / 32767 - 135, i = a < 0 ? -a : a, dx, dy;   /* 0 deg = up */

    dx = (i >= 90 ? sn[18] : sn[i / 5]) * (a < 0 ? -1 : 1);
    dy = i >= 90 ? sn[(i - 90) / 5] : -sn[(90 - i) / 5];
    gfx_disc(cx, cy, 10, C_WHITE);
    gfx_disc(cx, cy, 9, GFX_BLACK);
    gfx_disc(cx, cy, 7, C_LINE);
    gfx_disc(cx, cy, 6, GFX_BLACK);
    line(cx, cy, cx + dx * 7 / 100, cy + dy * 7 / 100, C_BLUE);
    gfx_disc(cx + dx * 7 / 100, cy + dy * 7 / 100, 1, C_BLUE);
    gfx_rect(cx - 6, cy - 13, 7, 1, C_GREEN);   /* the little arc above */
    gfx_pixel(cx - 7, cy - 12, C_GREEN);
}

/* text helpers (no printf) */
static char *put(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    *d = 0;
    return d;
}

static char *put_num(char *d, int v)           /* P01 .. P99, P100 .. */
{
    if (v >= 100)
        *d++ = (char)('0' + v / 100 % 10);
    *d++ = (char)('0' + v / 10 % 10);
    *d++ = (char)('0' + v % 10);
    *d = 0;
    return d;
}

/* the sound over the voice list (STATION 1's LCD: its place, size, colour and lines) while the title
 * time runs: P01 .. Pnn and the name (* = edited) above the waves, or P-- and wave A above how to
 * browse (once changed: wave B) */
static void title(int own, int slot, const int16_t *p)
{
    int x, wb = pss_waveb_of(p[PSS_WAVE_B]), s = pss_semis_of(p[PSS_BPITCH]);

    pss_title_on = (int32_t)(title_until - now) > 0;
    if (!pss_title_on)
        return;
    gfx_rect(74, 7, 142, 26, GFX_BLACK);
    outline(74, 7, 142, 26, 2, C_GREEN);
    if (own) {
        x = gfx_text(79, 11, "P", C_GREEN, 2);
        x = gfx_text(x, 11, pss_bank < 9 ? "0" : "", C_GREEN, 2);
        x = gfx_dec(x, 11, pss_bank + 1, C_GREEN, 2);
        x = gfx_text(x + 8, 11, bank_name(pss_bank), C_GREEN, 2);
        if (bank_edited)
            gfx_text(x, 11, "*", C_GREEN, 2);
    } else {
        x = gfx_text(79, 11, "P-- ", C_GREEN, 2);
        gfx_text(x, 11, pss_wave_name(pss_wave_of(p[PSS_WAVE_A])), C_GREEN, 2);
        if (!(own_edited >> slot & 1)) {
            gfx_text(79, 24, "SHIFT+BLUE: SOUNDS", C_GREEN, 1);
            return;
        }
    }
    if (own) {
        x = gfx_text(79, 24, "A ", C_GREEN, 1);
        x = gfx_text(x, 24, pss_wave_name(pss_wave_of(p[PSS_WAVE_A])), C_GREEN, 1);
        x = gfx_text(x + 6, 24, "B ", C_GREEN, 1);
    } else {
        x = gfx_text(79, 24, "B ", C_GREEN, 1);
    }
    x = gfx_text(x, 24, pss_wave_name(wb), C_GREEN, 1);
    if (wb >= 0 && s)
        gfx_dec(gfx_text(x + 4, 24, s > 0 ? "+" : "", C_GREEN, 1), 24, s, C_GREEN, 1);
}

void pss_frame(uint16_t *buf)
{
    static const uint16_t col[4] = { C_BLUE, C_GREEN, C_WHITE, C_ORANGE };
    static const uint8_t page_param[3][4] = {
        { PSS_WAVE_A, PSS_WAVE_B, PSS_MIX, PSS_CUTOFF }, { PSS_BPITCH, PSS_DETUNE, PSS_FENV, PSS_FDECAY },
        { PSS_RES, PSS_KEYTRK, PSS_ADECAY, PSS_BFADE },
    };
    static int8_t shown = -1;                   /* the sound slot the screen showed last frame */
    int16_t p[PSS_PARAMS];
    uint8_t on[PSS_PARAMS];
    volatile int16_t *k;
    int slot, i, x, pg = pss_page, wb, n, own;
    const char *ld;
    char txt[24], *d;

    if (!engine_screen_pss()) {
        shown = -1;
        pss_title_on = 0;
        return;
    }
    k = current(&slot);
    if (slot != shown) {                        /* the screen opens or shows another sound: its name */
        shown = (int8_t)slot;
        title_until = now + TITLE_TIME;
    }
    unpack(k, p);
    n = bank_n();
    own = pss_bank >= 0 && pss_bank < n && bank_slot == slot;
    wb = pss_waveb_of(p[PSS_WAVE_B]);
    for (i = 0; i < PSS_PARAMS; i++)            /* what is on: B, its mix and interval, the filter ... */
        on[i] = p[i] > 0;
    on[PSS_WAVE_A] = 1;
    on[PSS_WAVE_B] = wb >= 0;
    on[PSS_MIX] = wb >= 0 && p[PSS_MIX] > 0;
    on[PSS_CUTOFF] = p[PSS_CUTOFF] < 32766;     /* top = filter off */
    on[PSS_BPITCH] = wb >= 0 && pss_semis_of(p[PSS_BPITCH]) != 0;

    gfx_begin(buf);
    gfx_rect(0, 0, gfx_w, 140, GFX_BLACK);

    /* the keyboard (253 x 81, centred): voice list and strip, knob and buttons; speaker and keys */
    outline(34, 4, 253, 81, 3, C_TEAL);         /* the case: a 2-pixel teal frame */
    outline(35, 5, 251, 79, 2, C_TEAL);
    voice_list(n, own ? pss_bank : -1);

    outline(39, 37, 186, 10, 1, C_LINE);        /* the strip: the page, the sound (or the loader) */
    for (i = 0; i < PAGES; i++)
        gfx_rect(43 + i * 14, 41, 10, 2, i == pg ? C_BLUE : C_DIM);
    if ((ld = load_text(txt)) != 0) {
        gfx_text(221 - text_w(ld, 1), 40, ld, pss_load == LD_READING || pss_load == LD_IDLE ? C_GREEN : C_RED, 1);
    } else {
        if (own) {
            d = put(put_num(put(txt, "P"), pss_bank + 1), " ");
            d = put(d, bank_name(pss_bank));
            if (bank_edited)
                put(d, "*");
        } else {
            put(put(txt, "P-- "), pss_wave_name(pss_wave_of(p[PSS_WAVE_A])));
        }
        gfx_text(221 - text_w(txt, 1), 40, txt, C_GREEN, 1);
    }

    knob(243, 21, pg < 3 ? p[page_param[pg][0]] : 16384);
    for (i = 0; i < 12; i++) {                  /* the 12 parameters: column = page, row = knob */
        int c = i / 4, bx = 258 + c * 8, by = 11 + i % 4 * 6;

        if (on[page_param[c][i % 4]])
            gfx_rect(bx, by, 6, 4, C_BLUE);
        outline(bx, by, 6, 4, 0, c == pg ? C_WHITE : C_DIM);
    }
    if (wb >= 0)                                /* B sounds, the MIX bar */
        gfx_rect(230, 39, 5, 5, C_GREEN);
    else
        outline(230, 39, 5, 5, 0, C_GREEN);
    outline(239, 39, 14, 5, 0, C_RED);
    if (wb >= 0)
        gfx_rect(240, 40, p[PSS_MIX] * 12 / 32767, 3, C_RED);
    for (i = 0; i < 3; i++)
        outline(258 + i * 8, 39, 6, 5, 0, C_DIM);

    gfx_rect(36, 48, 249, 1, C_LINE);
    for (i = 0; i < 9; i++)                     /* the speaker */
        gfx_rect(237, 55 + 3 * i, 45, 1, C_DIM);
    keyboard(52, 82);
    title(own, slot, p);

    /* the page's four knobs (as every SDK engine) */
    if (pg == 3) {                              /* page 4: the sound's FX knobs (engine host) */
        eh_fx_draw(slot, 97);
        return;
    }
    for (i = 0; i < 4; i++) {
        int q = page_param[pg][i];

        x = 4 + i * 80;
        gfx_text(x, 99, labels[q], col[i], 2);
        gfx_rect(x, 112, 72, 6, C_TRACK);
        gfx_rect(x, 112, p[q] * 72 / 32767, 6, col[i]);
    }
    x = gfx_text(4, 125, "PUSH: ", C_DIM, 1);
    gfx_text(x, 125, page_names[(pg + 1) % PAGES], C_DIM, 1);
}
