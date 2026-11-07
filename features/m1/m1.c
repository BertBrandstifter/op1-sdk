/* STATION 1 synth engine (feature m1; spec: README.md): two multisounds layered, into a
 * low-pass filter with its own envelope.
 *
 * Runs on the engine host (features/enginehost, tag 3). Page 1 (WAVE A, WAVE B, MIX, CUTOFF) lives
 * in slots 1-4 as 0..16383, so TE's LFO can move them. Slots 5-8 hold 0x3000 | two 6-bit fields
 * each: the high field = page 2 (B PITCH, DETUNE, F.ENV, F.DECAY), the low one = page 3 (RES,
 * KEY TRK, A DECAY, B FADE). Every value is inside drwave's ranges (boot check passes, loads on
 * stock as drwave).
 *
 * m1_init / m1_engine_name / m1_key / m1_knob / m1_frame: as TRIO (trio.c), with 4 pages (page 4 =
 * the sound's FX knobs), WAVE A / B one multisound per detent, and a bank on shift + blue turn
 * (28 sounds with SoundFont data, 8 with the built-in waves), with P-- (the sound as it was before
 * browsing) in front of P01.
 * m1_main (main loop): loads the SoundFont's sample data (a file of the update) into TE's sample
 * memory the first time a sound slot holds an M1 sound. */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "m1_core.h"
#include "../enginehost/eh_host.h"

#define KEY_KNOB_PUSH   59                      /* blue 59, green 60, white 61, orange 62 */
#define STEP_SCALE      10485                   /* ~100 clicks full range (as engine_plus) */
#define TAG             3
#define PAGES           4
#define OFF             (-1)

static m1_voice voices[EH_VOICES];
/* the sounding notes (the LCD names them): a key held on the OP-1 (m1_note_key) is its note (TE
 * plays note index + 53 + 12 x octave); the next new voice tells the octave (its pitch rounded to the nearest octave of the
 * note: glide / LFO / bend at the start move it by less than 6 semitones). New voices no held key
 * accounts for (sequencer, MIDI, CHORD+'s extra notes) flash at their pitch for FLASH blocks. TE keeps
 * rendering a voice through its release and beyond, so rendering does not mean sounding. */
#define HELD        8
#define FLASH       100                         /* audio blocks: ~145 ms */
static volatile uint32_t blocks;                /* audio blocks with an M1 voice (render thread) */
static volatile int8_t held_id[HELD];           /* key id held, 0 = free (KeyHandler thread) */
static volatile int8_t held_note[HELD];         /* its note index 0..23 */
static volatile int8_t held_key[HELD];          /* its MIDI key, -1 = no voice yet (render thread) */
static volatile uint8_t held_seq[HELD];         /* press order */
static volatile uint8_t press_seq;
static volatile int8_t flash_key[EH_VOICES];
static volatile uint32_t flash_until[EH_VOICES];
volatile uint8_t m1_page;                       /* 0..3 = page 1..4 (bench reads) */
static volatile uint8_t touched;                /* the page's knob turned last (the LCD's cursor) */
static volatile uint8_t consumed;
volatile int8_t m1_bank = -1;                   /* bench reads */
static volatile uint8_t bank_edited;
static volatile int8_t bank_slot = -1;
static volatile uint8_t hint_done;              /* a knob was turned since boot: no SHIFT+BLUE hint */
static int16_t own_knobs[8][8];                 /* per sound slot: P--, the sound before the bank */

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
    static const uint8_t lo_param[4] = { M1_RES, M1_KEYTRK, M1_ADECAY, M1_BFADE };
    int i, v;

    for (i = 0; i < 4; i++) {
        v = k[i];                               /* TE's LFO adds to slots 1-4 (saturating) */
        v = v < 0 ? 0 : v > EH_PAGE1_MAX ? EH_PAGE1_MAX : v;
        p[i] = (int16_t)(v << 1);
        p[lo_param[i]] = (int16_t)(field(k, i, 0) * 520);
    }
    v = field(k, 0, 1);
    p[M1_BPITCH] = (int16_t)(((2 * (v > 48 ? 48 : v) + 1) * 32768) / 98);   /* zone centre of 49 */
    p[M1_DETUNE] = (int16_t)(field(k, 1, 1) * 520);
    p[M1_FENV] = (int16_t)(field(k, 2, 1) * 520);
    p[M1_FDECAY] = (int16_t)(field(k, 3, 1) * 520);
}

static void m1_eh_render(int vi, int16_t *out, const int16_t *knobs, const uint32_t *inc, int flags)
{
    int16_t p[M1_PARAMS];

    unpack(knobs, p);
    if (flags & EH_BLOCK)
        blocks++;
    if (flags & EH_FRESH) {
        int i, oldest = -1;

        m1_note_on(&voices[vi], inc[0], p);
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
    m1_render(&voices[vi], out, TE_VOICE_SAMPLES, inc, p);
}

void m1_init(void)
{
    eh_register(TAG, m1_eh_render);
}

/* Engine browser row "station 1" (content_sql) -> "drwave" (TE knows no such type: fatal) */
void m1_engine_name(te_string *out, void *db, const char *sql)
{
    static const char name[] = "station 1";
    char *d = (char *)te_str_data(out);
    unsigned int i;

    (void)db;
    (void)sql;
    for (i = 0; i < sizeof(name) - 1 && i < out->size && d[i] == name[i]; i++)
        ;
    if (out->size == sizeof(name) - 1 && i == out->size) {
        d[0] = 'd'; d[1] = 'r'; d[2] = 'w'; d[3] = 'a'; d[4] = 'v'; d[5] = 'e'; d[6] = 0; /* cap >= 15 */
        out->size = 6;
        m1_page = 0;
        bank_slot = -1;
    }
}

/* ------------------------------------------------------------------ main thread: the sample data */

/* The SoundFont's samples (~1.4 MB) are not code: they are a file of the update (content/
 * m1_rom.bin, gen_m1.py) read at runtime into TE's sample memory (VisualDSP heap 1, as
 * SampleBuffer.cpp allocates a sample), so they cost nothing of the 2 MB SDK region. The first
 * time a sound slot holds an M1 sound the file is opened, the buffer allocated, then read CHUNK
 * bytes per main-loop pass (the UI keeps running), checked against the build's sum and handed to
 * m1_core; until then notes are silent and the screen says why. The buffer stays until power-off.
 * No room in TE's sample memory (samples fill it): retried every ~2 s. */
enum { LD_IDLE, LD_READING, LD_READY, LD_NOFILE, LD_NOMEM, LD_BAD };
#define CHUNK       0x10000                     /* bytes per main-loop pass */
#define RETRY       250                         /* main-loop passes (~8 ms each) */
volatile uint8_t m1_load = LD_IDLE;             /* bench reads */
static uint8_t *ld_buf;
static int ld_fd = -1;
static uint32_t ld_pos;
static uint16_t ld_wait;

static int any_m1_sound(void)
{
    int s;

    for (s = 0; s < 8; s++)
        if (TE_SYNTH_TYPE[s] == TE_INSTR_DRWAVE && marked(TE_SYNTH_KNOBS + s * 8))
            return 1;
    return 0;
}

static uint32_t rom_sum(const uint8_t *d, uint32_t n)  /* as gen_m1.py rom_sum */
{
    const uint32_t *w = (const uint32_t *)(const void *)d;
    uint32_t h = 0x811c9dc5u, i;

    for (i = 0; i < n / 4; i++)
        h = ((h << 5) | (h >> 27)) ^ w[i];
    return h;
}

void m1_main(void)
{
    uint32_t n = m1_rom_file_bytes();
    int r;

    if (!m1_rom_file() || m1_load == LD_READY || m1_load == LD_NOFILE || m1_load == LD_BAD)
        return;
    if (m1_load != LD_READING) {
        if (m1_load == LD_NOMEM && ++ld_wait < RETRY)
            return;
        ld_wait = 0;
        if (!any_m1_sound())
            return;
        ld_fd = TE_FILE_OPEN(m1_rom_file(), TE_FILE_RDONLY, TE_FILE_MODE);
        if (ld_fd < 0) {
            m1_load = LD_NOFILE;
            return;
        }
        if (!ld_buf)
            ld_buf = TE_HEAP_MALLOC(TE_HEAP_SAMPLES, n);
        if (!ld_buf) {
            TE_FILE_CLOSE(ld_fd);
            m1_load = LD_NOMEM;
            return;
        }
        ld_pos = 0;
        m1_load = LD_READING;
    }
    r = TE_FILE_READ(ld_fd, ld_buf + ld_pos, n - ld_pos < CHUNK ? n - ld_pos : CHUNK);
    if (r > 0)
        ld_pos += (uint32_t)r;
    if (r > 0 && ld_pos < n)
        return;
    TE_FILE_CLOSE(ld_fd);
    TE_DCACHE_FLUSH(ld_buf, ld_buf + n, 1);     /* the reader may have used DMA */
    if (ld_pos != n || rom_sum(ld_buf, n) != m1_rom_file_sum()) {
        m1_load = LD_BAD;
        return;
    }
    m1_set_rom(ld_buf);
    m1_load = LD_READY;
}

/* the loader's state on the LCD's second line (0 = nothing to say) */
static const char *load_text(char *t)
{
    static const char *const msg[] = {
        "LOADING SAMPLES", 0, 0, "SAMPLE FILE MISSING", "NO SAMPLE MEMORY FREE", "SAMPLE FILE DAMAGED",
    };
    uint32_t pc;
    int i = 0;

    if (!m1_rom_file() || m1_load == LD_READY)
        return 0;
    if (m1_load != LD_READING)
        return msg[m1_load];
    pc = ld_pos / (m1_rom_file_bytes() / 100 + 1);
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

static int engine_screen_m1(void)
{
    int slot;

    return *TE_EDIT_MODE == 0 && *TE_SYNTH_SUBSCREEN == 0
           && TE_SYNTH_TYPE[*TE_SYNTH_SLOT & 7] == TE_INSTR_DRWAVE && marked(current(&slot));
}

/* on_key observe (KeyHandler thread, sees every key first): the keyboard's keys, held or released */
void m1_note_key(int key, int down)
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

int m1_key(int key, int down)
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
    if (*TE_SHIFT_HELD || !engine_screen_m1())
        return 0;
    m1_page = (uint8_t)(m1_page + 1 < PAGES ? m1_page + 1 : 0);
    touched = 0;
    consumed |= (uint8_t)(1 << b);
    return 1;
}

/* built-in bank: name, wave A, wave B (OFF), mix %, cutoff %, B pitch (semitones), detune, F.ENV %,
 * F.DECAY %, res %, key track %, A decay (amount: 0 = hold, 63 = fastest), B fade (0 = instant)
 * (6-bit steps). Waves are the multisound numbers of the sample set (0 PIANO .. 99 DWGS SINE), or
 * of the built-in waves. bank_rom holds the sound library's original multisounds,
 * octaves and layer levels. */
typedef struct {
    const char *name;
    int8_t a, b, mix, cut, bpitch, det, fenv, fdec, res, ktrk, adec, bfade;
} bank_entry;

#include "m1_bank.inc"

static const bank_entry bank_builtin[] = {
    { "SAW LEAD",     0, 0,   50, 55,  0,  30, 40, 45, 25, 40, 0,  0 },
    { "SQUARE BASS",  1, OFF, 0,  40,  0,  0,  60, 25, 35, 30, 0,  0 },
    { "PULSE",        2, OFF, 0,  70,  0,  0,  20, 40, 10, 40, 0,  0 },
    { "TRIANGLE",     3, 4,   30, 100, 12, 0,  0,  0,  0,  0,  0,  0 },
    { "SINE PAD",     4, 4,   50, 100, 7,  12, 0,  0,  0,  0,  0,  34 },
    { "ORGAN",        5, OFF, 0,  100, 0,  0,  0,  0,  0,  0,  0,  0 },
    { "E.PIANO",      6, 4,   20, 90,  12, 0,  0,  0,  0,  20, 28, 0 },
    { "VOICES",       7, 7,   50, 85,  0,  20, 0,  0,  0,  0,  0,  20 },
};

static const bank_entry *bank(int *n)
{
    if (m1_have_rom()) {
        *n = (int)(sizeof(bank_rom) / sizeof(bank_rom[0]));
        return bank_rom;
    }
    *n = (int)(sizeof(bank_builtin) / sizeof(bank_builtin[0]));
    return bank_builtin;
}

/* page-1 value at the centre of zone z of n (0..16383) */
static int16_t zone1(int z, int n) { return (int16_t)(((2 * z + 1) * 32768 / (2 * n)) >> 1); }

static void bank_load(volatile int16_t *k, int slot, int e)
{
    int n;
    const bank_entry *b = &bank(&n)[e];
    int nw = m1_nwaves();

    k[0] = zone1(b->a < nw ? b->a : 0, nw);
    k[1] = zone1(b->b == OFF || b->b >= nw ? 0 : b->b + 1, nw + 1);
    k[2] = (int16_t)(b->mix * EH_PAGE1_MAX / 100);
    k[3] = (int16_t)(b->cut * EH_PAGE1_MAX / 100);
    k[4] = pack(b->bpitch + 24, b->res * 63 / 100);
    k[5] = pack(b->det, b->ktrk * 63 / 100);
    k[6] = pack(b->fenv * 63 / 100, b->adec);
    k[7] = pack(b->fdec * 63 / 100, b->bfade);
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), (int16_t *)k);
    m1_bank = (int8_t)e;
    bank_slot = (int8_t)slot;
    bank_edited = 0;
}

int m1_knob(int knob, int16_t *diff)
{
    int slot, v, d = *diff;
    volatile int16_t *k;

    if ((unsigned int)knob > 3 || !engine_screen_m1())
        return 0;
    k = current(&slot);
    if (d)
        hint_done = 1;
    if (*TE_SHIFT_HELD) {                       /* drwave's shift + knob: not on an M1 sound */
        if (knob == 0 && d) {                   /* P-- (the sound as it was), P01 .. Pnn, P-- ... */
            int n, e = bank_slot == slot ? m1_bank : -1, s = d > 0 ? 1 : -1, i;

            bank(&n);
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
                m1_bank = -1;
                bank_slot = (int8_t)slot;
                bank_edited = 0;
            }
        }
        return 1;
    }
    if (m1_page == 3)
        return eh_fx_knob(slot, knob, (int16_t)d);
    touched = (uint8_t)knob;
    bank_edited = 1;
    if (m1_page == 0 && knob < 2) {             /* WAVE A / B: one multisound per detent */
        int n = m1_nwaves() + knob, z = (k[knob] < 0 ? 0 : k[knob]) * 2 * n >> 15;
        int step = (d < 0 ? -d : d) >> 10;

        step = step < 1 ? 1 : step;
        z += d > 0 ? step : -step;
        k[knob] = zone1(z < 0 ? 0 : z >= n ? n - 1 : z, n);
    } else if (m1_page == 0) {
        v = k[knob] + ((d * (STEP_SCALE >> 1)) >> 15);
        k[knob] = (int16_t)(v < 0 ? 0 : v > EH_PAGE1_MAX ? EH_PAGE1_MAX : v);
    } else {                                    /* 6-bit fields: one step per detent, more when fast */
        int hi = m1_page == 1, max = hi ? hi_max[knob] : 63;
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

/* Screen (spec: README.md "Screen"): the page LEDs, the 40 x 4 LCD (the program and the sounding
 * notes; the waves; the page's four knobs in four fields, name over value), the cursor under the
 * field of the knob turned last; below it the current page's four knobs as on every SDK engine. */

static const char *const labels[12] = {
    "WAVE A", "WAVE B", "MIX", "CUTOFF", "B PITCH", "DETUNE", "F.ENV", "F.DECAY",
    "RES", "KEY TRK", "A DECAY", "B FADE",
};
static const char *const page_names[PAGES] = { "WAVES", "PITCH+FILTER", "SHAPE", "FX" };
static const char *const page_leds[PAGES] = { "WAVES", "PITCH", "SHAPE", "FX" };

#define C_BLUE      GFX_RGB(0x6e, 0x8b, 0xff)
#define C_GREEN     GFX_RGB(0x3d, 0xdc, 0x97)
#define C_WHITE     GFX_RGB(0xe6, 0xe6, 0xe6)
#define C_ORANGE    GFX_RGB(0xff, 0x6a, 0x3d)
#define C_RED       GFX_RGB(0xff, 0x4f, 0x5e)
#define C_GREY      GFX_RGB(0x8a, 0x88, 0x98)
#define C_DIM       GFX_RGB(0x5a, 0x58, 0x66)
#define C_LINE      GFX_RGB(0x48, 0x46, 0x50)
#define C_TRACK     GFX_RGB(0x35, 0x32, 0x38)
#define C_LIME      GFX_RGB(0xc8, 0xe6, 0x3c)  /* the LCD: lit, unlit dots, its glass */
#define C_LIME_OFF  GFX_RGB(0x1a, 0x20, 0x0c)
#define C_LIME_BG   GFX_RGB(0x0c, 0x10, 0x06)

#define NOTES       3                           /* sounding notes named on the LCD */

static const eh_lcd lcd = { 20, 23, 40, 1, 1, 7, 11, C_LIME, C_LIME_OFF };

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

/* a name as the LCD shows it: capitals at the start of a word, small letters after (M1 Piano) */
static char *put_name(char *d, const char *s)
{
    int first = 1;

    for (; *s; s++) {
        char c = *s;

        *d++ = !first && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
        first = !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
    }
    *d = 0;
    return d;
}

/* the value of parameter q in a field of the LCD's last line (9 cells): waves by name */
static void lcd_value(char *t, int q, const int16_t *p)
{
    int v;

    if (q == M1_WAVE_A || q == M1_WAVE_B) {
        if (put_name(t, m1_wave_name(q == M1_WAVE_A ? m1_wave_of(p[q]) : m1_waveb_of(p[q]))) - t > 9)
            t[9] = 0;
    } else if (q == M1_BPITCH) {
        v = m1_semis_of(p[q]);
        put_dec(put(t, v > 0 ? "+" : ""), v);
    } else {
        put_dec(t, p[q] * 100 >> 15);
    }
}

/* the sounding notes (held keys with their voice's pitch, flashing voices), lowest first: A4 C#5 */
static void notes(char *t)
{
    static const char *const nn[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    uint32_t now = blocks;
    int key[HELD + EH_VOICES], n = 0, i, j, k;
    char *d = t;

    for (i = 0; i < HELD + EH_VOICES; i++) {
        k = i < HELD ? (held_id[i] ? held_key[i] : -1)
                     : (int32_t)(flash_until[i - HELD] - now) > 0 ? flash_key[i - HELD] : -1;
        if (k < 0 || k > 127)
            continue;
        for (j = 0; j < n && key[j] != k; j++)
            ;
        if (j == n)
            key[n++] = k;
    }
    for (i = 1; i < n; i++)                     /* sort */
        for (j = i; j > 0 && key[j - 1] > key[j]; j--) {
            k = key[j];
            key[j] = key[j - 1];
            key[j - 1] = k;
        }
    *d = 0;
    for (i = n > NOTES ? n - NOTES : 0; i < n; i++) {
        if (d != t)
            d = put(d, " ");
        d = put_dec(put(d, nn[key[i] % 12]), key[i] / 12 - 1);
    }
}

static int len(const char *s)
{
    int n = 0;

    while (s[n])
        n++;
    return n;
}

void m1_frame(uint16_t *buf)
{
    static const uint16_t col[4] = { C_BLUE, C_GREEN, C_WHITE, C_ORANGE };
    static const uint8_t page_param[3][4] = {
        { M1_WAVE_A, M1_WAVE_B, M1_MIX, M1_CUTOFF }, { M1_BPITCH, M1_DETUNE, M1_FENV, M1_FDECAY },
        { M1_RES, M1_KEYTRK, M1_ADECAY, M1_BFADE },
    };
    int16_t p[M1_PARAMS];
    volatile int16_t *k;
    int slot, i, x, pg = m1_page, n, own, cx;
    const bank_entry *b;
    const char *ld;
    char txt[48], *d;

    if (!engine_screen_m1())
        return;
    k = current(&slot);
    unpack(k, p);
    b = bank(&n);
    own = m1_bank >= 0 && m1_bank < n && bank_slot == slot;

    gfx_begin(buf);
    gfx_rect(0, 0, gfx_w, 140, GFX_BLACK);

    for (i = 0; i < PAGES; i++) {               /* the page LEDs (red = this page), PAGE - / + */
        x = 10 + i * 40;
        gfx_disc(x, 6, 2, i == pg ? C_RED : C_GREY);
        gfx_text(x + 6, 4, page_leds[i], i == pg ? C_WHITE : C_GREY, 1);
    }
    eh_frame(243, 1, 33, 11, 2, 1, C_WHITE, GFX_BLACK);
    gfx_text(249, 4, "PAGE -", C_WHITE, 1);
    eh_frame(279, 1, 33, 11, 2, 1, C_WHITE, GFX_BLACK);
    gfx_text(285, 4, "PAGE +", C_WHITE, 1);

    /* the LCD: PROG P02  M1 Piano* ... A4 C#5 / empty (how to browse until a knob turns; the
     * loader) / the page's four knobs, name over value */
    eh_frame(8, 15, 304, 55, 5, 2, C_BLUE, C_LIME_BG);
    eh_lcd_blank(&lcd, 4);
    d = put(txt, "PROG ");
    if (own) {
        d = put_dec(put(d, m1_bank < 9 ? "P0" : "P"), m1_bank + 1);
        d = put_name(put(d, "  "), b[(int)m1_bank].name);
        if (bank_edited)
            put(d, "*");
    } else {
        put_name(put(d, "P--  "), m1_wave_name(m1_wave_of(p[M1_WAVE_A])));
    }
    eh_lcd_text(&lcd, 0, 0, txt, C_LIME);
    notes(txt);
    eh_lcd_text(&lcd, 0, 40 - len(txt), txt, C_LIME);

    if ((ld = load_text(txt)) != 0) {           /* the sample data is not there (yet) */
        eh_lcd_text(&lcd, 1, 0, ld, m1_load == LD_READING || m1_load == LD_IDLE ? C_LIME : C_RED);
    } else if (!hint_done) {                    /* until the first knob turn; then the line stays dark */
        eh_lcd_text(&lcd, 1, 0, "SHIFT+BLUE: SOUNDS", C_LIME);
    }
    if (pg == 3) {
        eh_lcd_text(&lcd, 2, 0, "FX", C_LIME);
    } else {
        for (i = 0; i < 4; i++) {
            eh_lcd_text(&lcd, 2, i * 10, labels[page_param[pg][i]], C_LIME);
            lcd_value(txt, page_param[pg][i], p);
            eh_lcd_text(&lcd, 3, i * 10, txt, C_LIME);
        }
    }

    /* the cursor: a segment under each field, filled in its knob's colour under the knob turned
     * last; the page's name between two lines */
    for (i = 0; i < 4; i++) {
        cx = lcd.x + i * 10 * lcd.adv + (9 * lcd.adv - 2) / 2;
        if (pg < 3 && i == touched)
            eh_rrect(cx - 10, 74, 20, 4, 1, col[i]);
        else
            eh_frame(cx - 10, 74, 20, 4, 1, 1, C_GREY, GFX_BLACK);
    }
    n = len(page_names[pg]) * 4 - 1;
    gfx_rect(24, 86, 136 - n / 2, 1, C_LINE);
    gfx_text(160 - n / 2, 84, page_names[pg], C_GREY, 1);
    gfx_rect(166 + n / 2, 86, 130 - n / 2, 1, C_LINE);

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
