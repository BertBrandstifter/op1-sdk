/* Input quantizer: TE glue around quant_core (spec: README.md).
 *
 * q_kb_on / q_kb_off      keyboard note-on/off call sites (filter): capture instead of play
 * q_midi_on / q_midi_off  MIDI-in note-on/off call sites (filter): same
 * q_tick                  Seq thread, after TE's tick, once per 64-frame block: clock, grid,
 *                         drain captured events into the core, play what is due
 * q_key                   on_key entry (filter): shift + insert cycles
 *                         off -> 1/16 -> 1/8 -> 1/4 -> 1/2 -> 1/32 -> 1/8T -> 1/16T -> 1/32T -> off
 * q_knob                  on_knob entry (filter): on the tempo screen, shift + white knob picks the
 *                         grid, shift + orange knob toggles SNAP / QUEUE
 * q_frame                 after TE renders a frame: blinking grid dot top right while on (green on
 *                         beats); grid name for 1.5 s after a change; "QUANT 1/16" on the tempo screen
 *
 * Producers (KeyHandler thread, MIDI thread) only write their own single-producer ring; the
 * core is touched by the Seq thread alone. TE's own key logic (sequencer offer, tape trigger,
 * s_note_ons) still runs immediately: only the sound is moved. Synth and drum playmodes
 * (drums: the press-time octave and shift state are restored when the note plays).
 * No init hook: all state starts zeroed in .bss (core empty, grid off). */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "../quantcore/quant_core.h"

#define BARRIER()       __asm__ volatile("" ::: "memory")
#define LATE_WINDOW     1102                    /* 25 ms at 44.1 kHz */
#define RING            32                      /* power of two */
#define SRC_KB          0
#define SRC_MIDI        1

/* grid sizes in quarter notes (num/den): off, 1/16, 1/8, 1/4, 1/2, 1/32, 1/8T, 1/16T, 1/32T */
static const uint8_t grid_num[] = { 0, 1, 1, 1, 2, 1, 1, 1, 1 };
static const uint8_t grid_den[] = { 0, 4, 2, 1, 1, 8, 3, 6, 12 };
static const char *const grid_name[] = { "", "1/16", "1/8", "1/4", "1/2", "1/32", "1/8T", "1/16T",
                                         "1/32T" };
#define GRIDS (sizeof(grid_den) / sizeof(grid_den[0]))

typedef struct ring {
    qz_ev ev[RING];
    uint32_t stamp[RING];
    volatile uint32_t head;                     /* written by the producer only */
    volatile uint32_t tail;                     /* written by the Seq thread only */
} ring;

static ring rings[2];
static qz_state core;
static volatile uint32_t now;                   /* samples, advanced by q_tick */
volatile int q_grid;                            /* index into grid_*; 0 = off (global: bench reads it) */
static volatile int busy;                       /* core still holds events */
static volatile int key_consumed;               /* insert down was eaten -> eat its up too */
static int last_grid, using_tape;
static int32_t last_ph;                         /* metronome phase seen on the previous tick */
static uint32_t beats;                          /* metronome beats counted (parity for 1/2) */
static int have_ph;
static volatile uint32_t grid_prev;             /* last grid point (samples) for the blink dot */
static volatile uint32_t grid_step;             /* grid step in whole samples */
static volatile int grid_on_beat;               /* last grid point was a beat */
static volatile uint32_t label_until;           /* show the grid name until this time */
#define LABEL_TIME  (TE_SAMPLE_RATE * 3 / 2)
#define CHORD_WINDOW 1764                       /* 40 ms from a chord's first note: same slot (chords);
                                                 * capped at half a grid step */
volatile int q_queue_mode = 1;                  /* 1 queue (default), 0 snap (global: bench reads it) */
volatile int q_anchor_mode = 1;                 /* phrase start: first note after a pause plays now
                                                 * and anchors the grid (global: bench turns it off) */
static int applied_queue = -1;

static int ring_pending(const ring *r)
{
    return r->head != r->tail;
}

/* producer side: returns 1 if the event was captured (TE must not play it now) */
static int capture(int src, int kind, const te_note *n, int extra)
{
    ring *r = &rings[src];
    uint32_t h = r->head;
    qz_ev *e;
    int pm;

    if (!(q_grid || busy || ring_pending(r)))
        return 0;                               /* quantizer idle: stock behaviour */
    pm = *TE_PLAYMODE;
    if ((pm != 0 && pm != 1) || n->extra != 0 || n->note < 0 || n->note > 127
        || (uint32_t)(h - r->tail) >= RING)
        return 0;                               /* not ours / ring full: play now */
    e = &r->ev[h & (RING - 1)];
    e->kind = (uint8_t)kind;
    e->key = (uint8_t)n->note;
    e->data[0] = (uint8_t)n->vel;               /* -1 survives as 0xff, sign-extended below */
    e->data[1] = n->midi_out;
    e->data[2] = (uint8_t)extra;
    e->data[3] = (uint8_t)(src | pm << 1);      /* bit 1: drum playmode */
    e->data[4] = (uint8_t)TE_DRUM_OCTAVE[*TE_DRUM_SLOT & 7];   /* drums: octave at press */
    e->data[5] = (uint8_t)*TE_DRUM_SHIFT_NOTE;  /* drums: shift state at press (-1 = none) */
    r->stamp[h & (RING - 1)] = now;
    BARRIER();
    r->head = h + 1;
    return 1;
}

int q_kb_on(te_note *n, int extra)    { return capture(SRC_KB, QZ_ON, n, extra); }
int q_kb_off(te_note *n)              { return capture(SRC_KB, QZ_OFF, n, 0); }
int q_midi_on(te_note *n, int extra)  { return capture(SRC_MIDI, QZ_ON, n, extra); }
int q_midi_off(te_note *n)            { return capture(SRC_MIDI, QZ_OFF, n, 0); }

/* Seq thread: play one event through TE's voice code */
static void play(const qz_ev *e, void *ctx)
{
    te_note n;
    int drum = (e->data[3] >> 1) & 1, extra = (int8_t)e->data[2];
    int32_t shift_now;

    (void)ctx;
    n.note = e->key;
    n.vel = (int8_t)e->data[0];
    n.midi_out = e->data[1];
    n.pad[0] = n.pad[1] = n.pad[2] = 0;
    n.extra = 0;
    if (e->kind != QZ_ON) {
        TE_VOICE_NOTE_OFF(&n);                  /* always: never leave a note hanging */
        return;
    }
    if (drum != (*TE_PLAYMODE == 1))
        return;                                 /* played in the other instrument: drop it */
    if (!drum) {
        TE_VOICE_NOTE_ON(&n, extra);            /* synth: octave already in the note */
        return;
    }
    /* drums: note-on adds 12 * the octave *now* + extra -> correct to the octave at press, and
     * give it the shift state it had at press */
    extra += 12 * ((int8_t)e->data[4] - TE_DRUM_OCTAVE[*TE_DRUM_SLOT & 7]);
    shift_now = *TE_DRUM_SHIFT_NOTE;
    *TE_DRUM_SHIFT_NOTE = (int8_t)e->data[5];
    TE_VOICE_NOTE_ON(&n, extra);
    *TE_DRUM_SHIFT_NOTE = shift_now;
}

/* samples from now to the next grid point of grid g, aligned to the tape or the metronome */
static uint32_t to_next_grid(uint64_t step_q16, int g)
{
    int32_t st = *TE_TAPE_STATE;
    uint64_t pos, rem;
    int32_t ph, beat, sub;

    using_tape = (st == 1 || st == 2) && *TE_TAPE_MOVING;
    if (using_tape) {
        pos = (uint64_t)(uint32_t)(*TE_TAPE_POS >> 6) << 16;
        rem = pos % step_q16;
        return rem ? (uint32_t)((step_q16 - rem) >> 16) : 0;
    }
    ph = *TE_METRO_TO_BEAT;
    beat = *TE_METRO_BEAT_LEN;
    if (ph <= 0 || beat <= 0)
        return 0;
    if (grid_num[g] > grid_den[g]) {
        /* longer than a beat (1/2): the metronome knows the next beat, not the bar, so put
         * grid points on every k-th beat of our own beat count (may be one beat off the bar) */
        uint32_t k = grid_num[g] / grid_den[g], wait = (k - (beats + 1) % k) % k;
        return (uint32_t)(((int64_t)ph + (int64_t)beat * wait) >> 14);
    }
    sub = (int32_t)((int64_t)beat * grid_num[g] / grid_den[g]);
    if (sub <= 0)
        return 0;
    return (uint32_t)((ph % sub) >> 14);
}

static int bpm_x100(void)
{
    float b = *TE_BPM;

    if (!(b >= 40.0f && b <= 220.0f))
        return 0;
    return (int)(b * 100.0f + 0.5f);
}

static void drain(int g)
{
    int src;
    ring *r;
    uint32_t t;
    qz_ev e;

    for (src = 0; src < 2; src++) {
        r = &rings[src];
        while ((t = r->tail) != r->head) {
            BARRIER();
            e = r->ev[t & (RING - 1)];
            if (!g || qz_input(&core, r->stamp[t & (RING - 1)], &e) == QZ_NOW)
                play(&e, 0);
            r->tail = t + 1;
        }
    }
}

void q_tick(void)
{
    int g = q_grid, bpm;
    uint64_t step;
    uint32_t t;

    now += 64;
    t = now;
    /* count metronome beats: the countdown jumps up by about one beat when a beat passes
     * (a tempo change rescales it by less than that) */
    {
        int32_t ph = *TE_METRO_TO_BEAT;

        if (have_ph && ph > last_ph + (*TE_METRO_BEAT_LEN >> 1))
            beats++;
        last_ph = ph;
        have_ph = 1;
    }
    bpm = bpm_x100();
    /* off, playmode change in progress, or no sane tempo: release everything in order */
    if (!g || (*TE_PLAYMODE != 0 && *TE_PLAYMODE != 1) || *TE_PLAYMODE_TARGET != *TE_PLAYMODE || !bpm) {
        qz_flush(&core, play, 0);
        drain(0);
        busy = 0;
        last_grid = 0;                          /* re-init the core when quantizing resumes */
        return;
    }
    if (g != last_grid || q_queue_mode != applied_queue) {
        if (!last_grid)
            qz_init(&core);                     /* off -> on: forget note-ons from before */
        qz_set_window(&core, LATE_WINDOW);
        applied_queue = q_queue_mode;
        qz_set_queue(&core, applied_queue, CHORD_WINDOW);
        last_grid = g;
    }
    step = qz_step_q16((uint32_t)bpm, TE_SAMPLE_RATE, grid_num[g], grid_den[g]);
    qz_set_grid(&core, step, t + to_next_grid(step, g));
    {
        /* a pause of one beat (or one grid step, if longer) starts a new phrase */
        uint32_t beat = (uint32_t)(*TE_METRO_BEAT_LEN >> 14), st = (uint32_t)(step >> 16);

        qz_set_idle(&core, q_anchor_mode ? (beat > st ? beat : st) : 0);
        qz_set_chord(&core, CHORD_WINDOW < st / 2 ? CHORD_WINDOW : st / 2);
    }
    {
        uint32_t prev, next;

        qz_grid_around(&core, t, &prev, &next);
        if (prev != grid_prev) {
            /* is this grid point a beat? anchored phrase: counted from the anchor; else the tape
             * grid (running) or the metronome (stopped) says so */
            uint32_t beat = (uint32_t)(*TE_METRO_BEAT_LEN >> 14);

            if (grid_num[g] >= grid_den[g])
                grid_on_beat = 1;
            else if (!beat)
                grid_on_beat = 0;
            else if (core.anchored)
                grid_on_beat = (uint32_t)(prev - core.anchor) % beat < 64;
            else if (using_tape)
                grid_on_beat = ((uint32_t)(*TE_TAPE_POS >> 6) - (t - prev)) % beat < 64;
            else
                grid_on_beat = (uint32_t)(*TE_METRO_TO_BEAT >> 14) + (t - prev) + 64 >= beat;
        }
        grid_prev = prev;
        grid_step = (uint32_t)(step >> 16);
    }
    busy = 1;
    drain(g);
    qz_poll(&core, t, play, 0);
    busy = core.count != 0;
}

/* on_key entry: shift + insert cycles the grid; both its down and up are consumed */
int q_key(int key, int down)
{
    if (key != TE_KEY_INSERT)
        return 0;
    if (down && *TE_SHIFT_HELD) {
        q_grid = (q_grid + 1) % (int)GRIDS;
        label_until = now + LABEL_TIME;
        key_consumed = 1;
        return 1;
    }
    if (!down && key_consumed) {
        key_consumed = 0;
        return 1;
    }
    return 0;
}

/* on_knob entry, tempo screen + shift: white knob steps through the grids (no wrap), orange
 * knob: right = QUEUE, left = SNAP */
int q_knob(int knob, int16_t *diff)
{
    int g = q_grid;

    if (*TE_EDIT_MODE != TE_EDIT_MODE_TEMPO || !*TE_SHIFT_HELD || (knob != 2 && knob != 3))
        return 0;
    if (knob == 3) {
        if (*diff)
            q_queue_mode = *diff > 0;
        label_until = now + LABEL_TIME;
        return 1;
    }
    if (*diff > 0 && g < (int)GRIDS - 1)
        g++;
    else if (*diff < 0 && g > 0)
        g--;
    q_grid = g;
    label_until = now + LABEL_TIME;
    return 1;
}

/* blink length: 60 ms, at most half a grid step */
#define BLINK   2646

void q_frame(uint16_t *buf)
{
    int g = q_grid, x;
    uint32_t len = grid_step / 2 < BLINK ? grid_step / 2 : BLINK;

    gfx_begin(buf);
    if (*TE_EDIT_MODE == TE_EDIT_MODE_TEMPO) {
        /* bottom right of the tempo screen, scale 2 (over the lower edge of the boxes) */
        gfx_rect(120, 128, 200, 28, GFX_BLACK);
        x = gfx_text(124, 130, "QUANT ", GFX_WHITE, 2);
        x = gfx_text(x, 130, g ? grid_name[g] : "OFF", GFX_WHITE, 2);
        gfx_text(x + 8, 130, q_queue_mode ? "QUEUE" : "SNAP", GFX_RGB(255, 140, 0), 2);
        gfx_text(124, 143, "SHIFT+WHITE/ORANGE", GFX_RGB(128, 128, 128), 2);
    } else if ((int32_t)(label_until - now) > 0) {
        gfx_rect(gfx_w - 80, 0, 66, 14, GFX_BLACK);
        x = gfx_text(gfx_w - 78, 2, q_queue_mode ? "Q> " : "Q ", GFX_WHITE, 2);
        gfx_text(x, 2, g ? grid_name[g] : "OFF", GFX_WHITE, 2);
    }
    if (g && (uint32_t)(now - grid_prev) < len)
        gfx_disc(gfx_w - 7, 7, 4, grid_on_beat ? GFX_GREEN : GFX_WHITE);
}
