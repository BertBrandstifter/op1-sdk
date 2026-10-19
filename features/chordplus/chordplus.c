/* CHORD+ (spec: README.md): chord keyboard for the synth. White keys play the chords of
 * the chosen key and scale, held black keys modify them (cp_core.h).
 *
 * cp_kb_on / cp_kb_off  keyboard note-on/off call sites (filter, first in the chain): a white key
 *                       becomes its chord, a black key only sets a modifier (no sound). Each chord
 *                       note is passed on through the rest of the chain (compose_kb_note_on, e.g.
 *                       the quantizer) and played by TE if nobody takes it. With STRUM the notes go
 *                       to the Seq thread instead and play one after another (not quantized).
 * cp_tick               Seq thread, once per 64-frame block: clock, strummed notes that are due
 * cp_key                on_key (filter): shift + insert: off -> on + panel; on: panel; panel
 *                       open: off
 * cp_knob               on_knob (filter) while the panel is open: KEY SCALE VOICING STRUM
 * cp_frame              after TE's frame (drawn last): chord badge while on, panel while open
 * Synth playmode only; MIDI and the sequencers are untouched (they get the plain keys). Settings
 * live in RAM (C major, CLOSE, no strum after boot). */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "cp_core.h"

#define BARRIER()       __asm__ volatile("" ::: "memory")
#define PANEL_TIME      (TE_SAMPLE_RATE * 5)    /* panel closes 5 s after the last change */
#define STRUM_MAX       100                     /* ms */
#define RING            64                      /* power of two */
#define PENDING         32

/* composer's kb_note_on / kb_note_off chains (generated _compose.c): the rest of the chain */
int compose_kb_note_on(void *note, int extra);
int compose_kb_note_off(void *note);

volatile uint8_t cp_on;                         /* bench reads */
volatile cp_settings cp_set;                    /* key, scale, voicing (bench reads) */
volatile uint8_t cp_strum;                      /* ms between strummed notes */
static volatile uint32_t now;                   /* samples, advanced by cp_tick */
static volatile uint32_t panel_until;           /* panel open while now < this */
static volatile uint8_t panel;
static volatile uint8_t consumed;               /* insert down was eaten -> eat its up */
volatile uint8_t cp_pinned;                     /* panel pinned (insert while shown): no timeout */

static int panel_shown(void)
{
    return panel && (cp_pinned || (int32_t)(now - panel_until) < 0);
}
static volatile char chord_name[16];

/* KeyHandler thread */
static uint8_t forwarding;                      /* our own notes passing through the chain */
static uint8_t held_pc[12];                     /* black keys held, per pitch class */
static volatile uint16_t held_at;               /* black keys held, by place (on_key; bit 0..9) */

/* the black keys' ids (on_key), lower octave F# G# A# C# D# then the upper (RE 2026-10-17) */
static const uint8_t black_ids[10] = { 13, 14, 15, 32, 22, 23, 24, 25, 26, 27 };
static uint8_t count_sync[128];                 /* sounding chord notes (unstrummed) */
static int prev_center;
static struct {
    uint8_t notes[CP_MAX_NOTES], n, strum;
} chord_of[128];                                /* the chord each held white key started */

/* KeyHandler -> Seq thread: strummed notes */
typedef struct ev {
    uint8_t on, note, midi_out;
    int8_t vel;
    uint32_t delay;                             /* samples after it reaches the Seq thread */
} ev;
static ev ring[RING];
static volatile uint32_t head, tail;

/* Seq thread */
static struct {
    uint8_t note, midi_out;
    int8_t vel;
    uint32_t due;
} pending[PENDING];
static int npending;
static uint8_t count_strum[128];

/* the held black keys as modifiers: by their place on the keyboard (lower octave: quality, upper:
 * power / sus2 / voicings / more inversion); by pitch class (the lower octave's meaning) if no
 * on_key event told the place */
static int mods(void)
{
    int i, m = 0;

    if (held_at) {
        for (i = 0; i < 10; i++)
            if (held_at >> i & 1)
                m |= cp_black_mod_at(i);
        return m;
    }
    return (held_pc[1] ? CP_FLIP : 0) | (held_pc[3] ? CP_SEVEN : 0) | (held_pc[6] ? CP_SUS4 : 0)
           | (held_pc[8] ? CP_NINE : 0) | (held_pc[10] ? CP_INV1 : 0) | (held_pc[10] > 1 ? CP_INV2 : 0);
}

static void forward_on(const te_note *n, int extra, int note)
{
    te_note m = *n;
    int taken;

    m.note = note;
    forwarding = 1;
    taken = compose_kb_note_on(&m, extra);
    forwarding = 0;
    if (!taken)
        TE_VOICE_NOTE_ON(&m, extra);
}

static void forward_off(const te_note *n, int note)
{
    te_note m = *n;
    int taken;

    m.note = note;
    forwarding = 1;
    taken = compose_kb_note_off(&m);
    forwarding = 0;
    if (!taken)
        TE_VOICE_NOTE_OFF(&m);
}

static void push(int on, const te_note *n, int note, uint32_t delay)
{
    uint32_t h = head;
    ev *e;

    if (h - tail >= RING)
        return;                                 /* full: drop (never more than a few chords) */
    e = &ring[h & (RING - 1)];
    e->on = (uint8_t)on;
    e->note = (uint8_t)note;
    e->midi_out = n->midi_out;
    e->vel = (int8_t)n->vel;
    e->delay = delay;
    BARRIER();
    head = h + 1;
}

int cp_kb_on(te_note *n, int extra)
{
    cp_settings s;
    uint8_t *notes;
    int note = n->note, c, i;

    if (forwarding || !cp_on || *TE_PLAYMODE != 0 || n->extra != 0 || note < 0 || note > 127)
        return 0;
    if (cp_is_black(note)) {
        held_pc[note % 12]++;
        return 1;                               /* modifier: no sound */
    }
    if (chord_of[note].n)
        return 1;                               /* already sounding (key bounce) */
    s.key = cp_set.key;
    s.scale = cp_set.scale;
    s.voicing = cp_set.voicing;
    notes = chord_of[note].notes;
    c = cp_chord(&s, note, mods(), prev_center, notes);
    if (!c)
        return 0;
    prev_center = cp_center(notes, c);
    cp_name(&s, note, mods(), (char *)chord_name);
    i = cp_root_pc(&s, note);
    if (i >= 0 && notes[0] % 12 != i) {         /* inverted: the bass after a slash, "C/E" */
        const char *b = cp_key_names[notes[0] % 12];
        int k;

        for (k = 0; k < 11 && chord_name[k]; k++)
            ;
        if (k <= 12) {
            chord_name[k++] = '/';
            while (*b && k < 15)
                chord_name[k++] = *b++;
            chord_name[k] = 0;
        }
    }
    chord_of[note].n = (uint8_t)c;
    chord_of[note].strum = cp_strum != 0;
    for (i = 0; i < c; i++) {
        if (chord_of[note].strum)
            push(1, n, notes[i], (uint32_t)i * cp_strum * (TE_SAMPLE_RATE / 1000));
        else if (count_sync[notes[i]]++ == 0)
            forward_on(n, extra, notes[i]);
    }
    return 1;
}

int cp_kb_off(te_note *n)
{
    int note = n->note, i;

    if (forwarding || note < 0 || note > 127)
        return 0;
    if (cp_is_black(note)) {
        if (!held_pc[note % 12])
            return 0;                           /* pressed while CHORD+ was off: TE's */
        held_pc[note % 12]--;
        return 1;
    }
    if (!chord_of[note].n)
        return 0;
    for (i = 0; i < chord_of[note].n; i++) {
        int x = chord_of[note].notes[i];

        if (chord_of[note].strum)
            push(0, n, x, 0);
        else if (count_sync[x] && --count_sync[x] == 0)
            forward_off(n, x);
    }
    chord_of[note].n = 0;
    return 1;
}

/* ------------------------------------------------------------------ Seq thread */

static void play(int on, int note, int midi_out, int vel)
{
    te_note m;

    m.note = note;
    m.vel = vel;
    m.midi_out = (uint8_t)midi_out;
    m.pad[0] = m.pad[1] = m.pad[2] = 0;
    m.extra = 0;
    if (on)
        TE_VOICE_NOTE_ON(&m, 0);
    else
        TE_VOICE_NOTE_OFF(&m);
}

void cp_tick(void)
{
    uint32_t t = now + TE_AUDIO_FRAMES;
    int i, j;

    now = t;
    while (tail != head) {
        ev e = ring[tail & (RING - 1)];

        BARRIER();
        tail++;
        if (e.on) {
            if (npending < PENDING) {
                pending[npending].note = e.note;
                pending[npending].midi_out = e.midi_out;
                pending[npending].vel = e.vel;
                pending[npending].due = t + e.delay;
                npending++;
            }
            continue;
        }
        /* off: a strummed note not played yet is dropped, else released */
        for (i = 0; i < npending && pending[i].note != e.note; i++)
            ;
        if (i < npending) {
            pending[i] = pending[--npending];
        } else if (count_strum[e.note] && --count_strum[e.note] == 0) {
            play(0, e.note, e.midi_out, e.vel);
        }
    }
    for (i = 0; i < npending; ) {
        if ((int32_t)(t - pending[i].due) >= 0) {
            if (count_strum[pending[i].note]++ == 0)
                play(1, pending[i].note, pending[i].midi_out, pending[i].vel);
            for (j = i; j < npending - 1; j++)
                pending[j] = pending[j + 1];   /* keep order: strum up */
            npending--;
        } else {
            i++;
        }
    }
}

/* ------------------------------------------------------------------ KeyHandler thread */

int cp_key(int key, int down)
{
    int i;

    for (i = 0; i < 10; i++)                    /* black keys: only watched (their notes follow) */
        if (key == black_ids[i]) {
            if (down)
                held_at |= (uint16_t)(1 << i);
            else
                held_at &= (uint16_t)~(1 << i);
            return 0;
        }
    if (key != TE_KEY_INSERT)
        return 0;
    if (!down) {
        if (consumed) {
            consumed = 0;
            return 1;
        }
        return 0;
    }
    if (!*TE_SHIFT_HELD) {                      /* insert while the panel shows: pin / unpin it */
        if (!cp_on || !panel_shown())
            return 0;                           /* else insert is TE's */
        cp_pinned = !cp_pinned;
        if (!cp_pinned)
            panel = 0;                          /* unpinned: closes */
        consumed = 1;
        return 1;
    }
    if (!cp_on) {
        cp_on = 1;
        panel = 1;
    } else if (!panel_shown()) {
        panel = 1;
    } else {
        cp_on = 0;
        panel = 0;
        cp_pinned = 0;
    }
    panel_until = now + PANEL_TIME;
    consumed = 1;
    return 1;
}

static int step(int v, int d, int n)
{
    return (v + (d > 0 ? 1 : -1) + n) % n;
}

int cp_knob(int knob, int16_t *diff)
{
    int d = *diff, v;

    if ((unsigned int)knob > 3 || !cp_on || !panel_shown() || !d)
        return 0;
    switch (knob) {
    case 0: cp_set.key = (uint8_t)step(cp_set.key, d, 12); break;
    case 1: cp_set.scale = (uint8_t)step(cp_set.scale, d, CP_SCALES); break;
    case 2: cp_set.voicing = (uint8_t)step(cp_set.voicing, d, CP_VOICINGS); break;
    default:
        v = cp_strum + (d > 0 ? 5 : -5);
        cp_strum = (uint8_t)(v < 0 ? 0 : v > STRUM_MAX ? STRUM_MAX : v);
    }
    panel_until = now + PANEL_TIME;
    return 1;
}

/* ------------------------------------------------------------------ GUI thread */

void cp_frame(uint16_t *buf)
{
    static const char *const short_scale[CP_SCALES] = {     /* 8 chars max at size 2 */
        "MAJOR", "MINOR", "DORIAN", "MIXO", "HARM MIN", "PHRYGIAN", "LYDIAN",
    };
    static const uint16_t col[4] = {
        GFX_RGB(0x69, 0x8e, 0xff), GFX_RGB(0x00, 0xed, 0x95),   /* blue, green */
        GFX_RGB(0xe6, 0xe6, 0xe6), GFX_RGB(0xff, 0x6a, 0x3d),   /* white, orange */
    };
    static const uint16_t dim = GFX_RGB(0x5a, 0x58, 0x66), frame_col = GFX_RGB(0x35, 0x32, 0x38);
    char name[16];
    int i, x;

    if (!cp_on)
        return;
    gfx_begin(buf);
    for (i = 0; i < 15 && chord_name[i]; i++)
        name[i] = chord_name[i];
    name[i] = 0;
    gfx_rect(196, 0, 76, 9, GFX_BLACK);         /* badge: between page titles and P1 / grid dot */
    x = gfx_text(198, 2, "CHORD", col[1], 1);
    gfx_text(x + 4, 2, name, GFX_WHITE, 1);

    if (!panel_shown())
        return;
    /* nearly full screen, size 2-3 text (4 px per char and scale): four columns of 78 px */
    gfx_rect(2, 2, 316, 152, frame_col);
    gfx_rect(4, 4, 312, 148, GFX_BLACK);
    gfx_text(10, 10, "CHORD+", col[1], 3);
    if (cp_pinned) {                            /* pinned: stays until insert again */
        gfx_text(178, 8, "PINNED", col[3], 1);
        gfx_text(178, 18, "INSERT: UNPIN", dim, 1);
    } else {
        gfx_text(178, 8, "INSERT: PIN", dim, 1);
        gfx_text(178, 18, "SHIFT+INSERT: OFF", dim, 1);
    }
    for (i = 0; i < 4; i++) {
        static const char *const labels[4] = { "KEY", "SCALE", "VOICING", "STRUM" };

        gfx_rect(10 + i * 78, 34, 70, 2, col[i]);
        gfx_text(10 + i * 78, 40, labels[i], col[i], 2);
    }
    gfx_text(10, 56, cp_key_names[cp_set.key % 12], GFX_WHITE, 3);
    gfx_text(88, 58, short_scale[cp_set.scale % CP_SCALES], GFX_WHITE, 2);
    gfx_text(166, 58, cp_voicing_names[cp_set.voicing % CP_VOICINGS], GFX_WHITE, 2);
    if (cp_strum) {
        x = gfx_dec(244, 58, cp_strum, GFX_WHITE, 2);
        gfx_text(x, 58, "MS", GFX_WHITE, 2);
    } else {
        gfx_text(244, 58, "OFF", GFX_WHITE, 2);
    }
    gfx_text(10, 80, "HOLD BLACK KEYS - LOWER OCTAVE", dim, 1);
    gfx_text(166, 80, "UPPER OCTAVE", dim, 1);
    for (i = 0; i < 5; i++) {
        static const char *const lower[5] = { "C# MIN/MAJ", "D# 7TH", "F# SUS4", "G# 9TH", "A# INV 1" };
        static const char *const upper[5] = { "C# POWER", "D# SUS2", "F# OPEN", "G# WIDE", "A# INV 2" };
        static const uint8_t bit[5] = { 3, 4, 0, 1, 2 };     /* places: F# G# A# C# D# */
        int y = 90 + i * 12;

        gfx_text(10, y, lower[i], held_at >> bit[i] & 1 ? col[1] : GFX_WHITE, 2);
        gfx_text(166, y, upper[i], held_at >> (bit[i] + 5) & 1 ? col[1] : GFX_WHITE, 2);
    }
}
