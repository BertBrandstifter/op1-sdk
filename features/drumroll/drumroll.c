/* DRUM ROLL (spec: README.md): step sequencer for the drum kit, FL-style channel rack drawn to
 * scale, per-step VEL TONE LEN NUDGE PROB RATCH, per-sample filter + level.
 *
 * dr_init         defaults (4 empty patterns, lanes on keys 1..8, filters off)
 * dr_tick         Seq thread, once per 64-frame block: clock (tape position while the tape runs,
 *                 own clock otherwise), the pattern's hits as drum note-ons, every note-off when it
 *                 is due, the preview hit; notes only in drum playmode (TE asserts otherwise)
 * dr_te_note_on   drum sampler voice note_on slot: TE's, then the voice's post-process state (the
 *                 sequencer's gain + gate for its own hits, the sample's level for played keys)
 * dr_te_render    drum sampler voice render slot: TE's, then the sample's filter, gain, gate
 * dr_key          on_key: shift + white press on the tape screen = play / stop (drum playmode, not
 *                 while the requantize screen is open);
 *                 shift + green press on the drum screen opens / closes the screen; while
 *                 open blue press = step on / off (any page), green press = next page, white press =
 *                 hear the step, shift + press = actions
 *                 (all knob presses ours, also shift + blue); drum keys select their lane (not consumed)
 * dr_knob         on_knob while open: the page's four functions; shift + blue lane, shift + green pattern
 * dr_frame        the screen, or a small badge while running with the screen closed
 * dr_main         main thread, every main-loop pass: loads the patterns + settings from TE's op1.db
 *                 once, then saves what changed ~3 s after the last edit (at once on close)
 * Page 6 CLEAR: a knob push asks "CLEAR PATTERN X?" (turn: no / yes, push: ok). */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "dr_core.h"
#include "dr_dsp.h"
#include "dr_store.h"

/* TE drum sampler voice (DrumSampler.cpp, RE 2026-10-14): vtable 0x0190fac4 (render slot 0x0190fad8,
 * note_on slot 0x0190fae0), 12 voices of 0x14 bytes from 0xffb00000 (ctor 0x01978df4). note_on gets
 * the key's 16-byte parameter block: 0x01aaadb4 + kit slot * 0x180 + key * 16 (Core 0x01967ccc). */
#define DR_TE_RENDER        ((void (*)(void *, int16_t *, const int16_t *))0x019789ec)
#define DR_TE_NOTE_ON       ((void (*)(void *, const int16_t *))0x01978926)
#define DR_TE_VOICE_BASE    0xffb00000u
#define DR_TE_VOICE_SIZE    0x14
#define DR_TE_VOICES        12
#define DR_TE_KEY_PARAMS    0x01aaadb4u
#define DR_TE_KIT_SIZE      0x180
#define DR_NOTE0            53                  /* drum key 0 (TE adds 12 * the kit's octave) */
/* drum screen edit mode: assumed 1 (synth 0, tape 2, mixer 3, tempo 4 are known) */
#define EDIT_TAPE           2
/* requantize (when in the image): its screen owns the tape screen's knob presses from PANEL (2) on */
extern volatile int rq_state __attribute__((weak));
#define RQ_PANEL            2
#define KEY_PUSH            59                  /* blue 59 green 60 white 61 orange 62 */
#define OFFS                64
#define BLOCK               TE_AUDIO_FRAMES
#define BARRIER()           __asm__ volatile("" ::: "memory")

/* ------------------------------------------------------------------ state */

dr_pattern dr_pat[DR_PATTERNS];                 /* bench reads */
volatile uint8_t dr_cur;                        /* pattern 0..3 */
volatile uint8_t dr_run, dr_open, dr_page;     /* bench reads; page 0..DR_PAGES-1 */
volatile uint8_t dr_roll[8];                    /* drum sound picked "drum roll" in the engine list */
static volatile uint8_t roll_hidden[8];         /* ... and its screen closed with shift + green */
volatile uint8_t dr_sel, dr_cursor, dr_param, dr_zoom;  /* dr_param: the graph's step parameter */
volatile uint8_t dr_swing;
volatile uint32_t dr_clock;                     /* last tick's clock (playheads, header) */
volatile uint8_t dr_tape;                       /* following the tape */
static volatile uint8_t inited;

typedef dr_filt filt;
static filt filters[DR_KEYS];
static dr_coef coefs[DR_KEYS];
static volatile int32_t key_gain[DR_KEYS];      /* played keys: the sample's lane level */

/* Seq thread -> voice note_on (same thread, synchronous) */
static volatile struct { int8_t key; uint8_t valid; int32_t gain, gate; } pend;
static dr_voice voices[DR_TE_VOICES];
static volatile uint8_t voice_on[DR_TE_VOICES];

/* KeyHandler -> Seq thread */
static volatile uint8_t preview_req;
static volatile uint8_t consumed;               /* knob pushes whose key-up we eat (bits 0..3) */

/* Seq thread */
static struct { uint8_t note; uint32_t due; } offs[OFFS];
static int noffs;
static uint32_t next_from;
static uint8_t was_running, was_tape;
static uint32_t seed = 0x2545f491u;

static void refresh_gains(void);

void dr_init(void)
{
    int i;

    for (i = 0; i < DR_PATTERNS; i++)
        dr_pattern_init(&dr_pat[i]);
    for (i = 0; i < DR_KEYS; i++) {
        filters[i].type = DR_F_OFF;
        filters[i].cut = 64;
        filters[i].res = 20;
        dr_coef_set(&coefs[i], DR_F_OFF, 64, 20);
    }
    dr_zoom = 0;
    refresh_gains();
    BARRIER();
    inited = 1;
}

static dr_pattern *pat(void)
{
    return &dr_pat[dr_cur & (DR_PATTERNS - 1)];
}

static dr_lane *lane(void)
{
    return &pat()->lane[dr_sel & (DR_LANES - 1)];
}

/* played keys get the level of the first lane on that key */
static void refresh_gains(void)
{
    const dr_pattern *p = pat();
    int k, l;

    for (k = 0; k < DR_KEYS; k++) {
        int32_t g = DR_UNITY;

        for (l = 0; l < DR_LANES; l++)
            if (p->lane[l].key == k) {
                g = p->lane[l].level * DR_UNITY / 100;
                break;
            }
        key_gain[k] = g;
    }
}

static void set_filter(int key)
{
    dr_coef c;

    dr_coef_set(&c, filters[key].type, filters[key].cut, filters[key].res);
    coefs[key].type = DR_F_OFF;                 /* the render never sees half an update */
    BARRIER();
    coefs[key].a1 = c.a1;
    coefs[key].a2 = c.a2;
    coefs[key].a3 = c.a3;
    coefs[key].kh = c.kh;
    BARRIER();
    coefs[key].type = c.type;
}

static int drum_screen(void);

/* is the DRUM ROLL screen up (on the drum engine page) */
static int shown(void)
{
    int s = *TE_DRUM_SLOT & 7;

    return drum_screen() && (dr_roll[s] ? !roll_hidden[s] : dr_open);
}

static void set_shown(int on)
{
    int s = *TE_DRUM_SLOT & 7;

    if (dr_roll[s])
        roll_hidden[s] = (uint8_t)!on;
    else
        dr_open = (uint8_t)on;
}

/* TE's drum engine pick (0x0193f86c, entry, filter; msg: +4 drum sound, +8 = drum_types row id, or
 * -index of a kit preset). "drum roll" (our row) : TE does nothing (the sound keeps its kit, nothing
 * unknown is saved), DRUM ROLL takes the sound's engine page. A kit pick keeps DRUM ROLL on (the kits
 * are its sounds), dbox turns it off. */
#define DR_ROW              64                  /* content_sql: drum_types id of "drum roll" */

int dr_te_drum_pick(void *msg)
{
    int s = *(volatile int32_t *)((char *)msg + 4) & 7, id = *(volatile int32_t *)((char *)msg + 8);

    if (id == DR_ROW) {
        dr_roll[s] = 1;
        roll_hidden[s] = 0;
        return 1;
    }
    if (id > 0)
        dr_roll[s] = 0;
    return 0;
}

static int drum_ok(void)
{
    return *TE_PLAYMODE == 1 && *TE_PLAYMODE_TARGET == 1;
}

/* the drum sound's engine page (drum screen, T1): DRUM ROLL's place, as an engine's screen; the
 * envelope / FX / LFO pages (T2-T4) stay TE's (and the SDK effects') */
static int drum_screen(void)
{
    return *TE_PLAYMODE == 1 && *TE_EDIT_MODE == TE_EDIT_MODE_DRUM && *TE_DRUM_SUBSCREEN == 0;
}

static uint32_t bpm_x100(void)
{
    float b = *TE_BPM;

    return b >= 40.0f && b <= 220.0f ? (uint32_t)(b * 100.0f + 0.5f) : 0;
}

/* ------------------------------------------------------------------ voices (audio) */

static int voice_index(const void *self)
{
    uint32_t a = (uint32_t)self - DR_TE_VOICE_BASE, vi = a / DR_TE_VOICE_SIZE;

    return a % DR_TE_VOICE_SIZE == 0 && vi < DR_TE_VOICES ? (int)vi : -1;
}

void dr_te_note_on(void *self, const int16_t *params)
{
    uint32_t a = (uint32_t)params - DR_TE_KEY_PARAMS;
    int vi = voice_index(self), key;

    DR_TE_NOTE_ON(self, params);
    if (vi < 0)
        return;
    voice_on[vi] = 0;
    if (!inited || a >= 8 * DR_TE_KIT_SIZE || a % 16)
        return;
    key = (int)(a % DR_TE_KIT_SIZE) / 16;
    if (pend.valid && pend.key == key) {
        dr_voice_start(&voices[vi], key, pend.gain, pend.gate);
        pend.valid = 0;
    } else {
        dr_voice_start(&voices[vi], key, key_gain[key], -1);
    }
    voice_on[vi] = 1;
}

void dr_te_render(void *self, int16_t *out, const int16_t *params)
{
    int vi = voice_index(self);
    dr_voice *v;

    DR_TE_RENDER(self, out, params);
    if (vi < 0 || !voice_on[vi])
        return;
    v = &voices[vi];
    dr_process(v, out, TE_VOICE_SAMPLES, coefs[v->key].type != DR_F_OFF ? &coefs[v->key] : 0);
}

/* ------------------------------------------------------------------ Seq thread */

static void note(int on, int key, int tone, int32_t gain, int32_t gate)
{
    te_note n;
    int32_t shift_now;

    n.note = DR_NOTE0 + key;
    n.vel = -1;                                 /* 127; the step's VEL is our gain */
    n.midi_out = 0;
    n.pad[0] = n.pad[1] = n.pad[2] = 0;
    n.extra = 0;
    if (!on) {
        TE_VOICE_NOTE_OFF(&n);
        return;
    }
    pend.key = (int8_t)key;
    pend.gain = gain;
    pend.gate = gate;
    BARRIER();
    pend.valid = 1;
    shift_now = *TE_DRUM_SHIFT_NOTE;            /* shift + held drum key = chromatic mode: not ours */
    *TE_DRUM_SHIFT_NOTE = -1;
    TE_VOICE_NOTE_ON(&n, tone);
    *TE_DRUM_SHIFT_NOTE = shift_now;
    pend.valid = 0;
}

static void add_off(int key, uint32_t due)
{
    if (noffs < OFFS) {
        offs[noffs].note = (uint8_t)key;
        offs[noffs].due = due;
        noffs++;
    } else {
        note(0, key, 0, 0, 0);                  /* never lose a note-off (TE's list holds 128) */
    }
}

static void flush_offs(void)
{
    while (noffs)
        note(0, offs[--noffs].note, 0, 0, 0);
}

/* TE's drum note-on ignores a note that is still held (0x01a84ba0 searches its sounding list
 * first): release our held copy of that drum before hitting it again (back-to-back steps, ratchets) */
static void release_key(int key)
{
    int i;

    for (i = 0; i < noffs; )
        if (offs[i].note == key) {
            note(0, key, 0, 0, 0);
            offs[i] = offs[--noffs];
        } else {
            i++;
        }
}

static void hit(const dr_lane *ln, const dr_step *s, uint32_t at, uint32_t hold, int full)
{
    int32_t gain = s->p[DR_VEL] * ln->level * DR_UNITY / 10000;

    release_key(ln->key);
    note(1, ln->key, s->p[DR_TONE], gain, full ? -1 : (int32_t)hold * 2);   /* voices run at 2x */
    add_off(ln->key, at + hold);
}

static void emit(void *ctx, const dr_hit *h)
{
    (void)ctx;
    hit(&pat()->lane[h->lane], h->s, h->at, h->hold, h->full);
}

/* step lengths per lane, recomputed only when the tempo or a rate changes (64-bit divides) */
static uint64_t st_len[DR_LANES];
static uint8_t st_rate[DR_LANES];
static uint32_t st_bits, st_bpm;

/* GUI: the Seq thread's cached length (stale for at most a block), or 120 BPM before any tick */
static uint64_t lane_step(int l)
{
    uint64_t L = st_len[l & (DR_LANES - 1)];

    return st_bpm && L ? L : dr_step_q16(12000, pat()->lane[l & (DR_LANES - 1)].rate);
}

static const uint64_t *steps(void)
{
    const dr_pattern *p = pat();
    uint32_t bits = *(volatile uint32_t *)TE_BPM;
    int l, all = bits != st_bits || !st_bpm;

    if (all) {
        st_bits = bits;
        st_bpm = bpm_x100();
    }
    for (l = 0; l < DR_LANES; l++)
        if (all || st_rate[l] != p->lane[l].rate) {
            st_rate[l] = p->lane[l].rate;
            st_len[l] = dr_step_q16(st_bpm, st_rate[l]);
        }
    return st_len;
}

void dr_tick(void)
{
    int tape = *TE_TAPE_STATE == 1 || *TE_TAPE_STATE == 2, ok = drum_ok(), i;
    uint32_t from, to;
    const uint64_t *st;

    if (!inited)
        return;
    if (tape) {
        uint32_t p = (uint32_t)(*TE_TAPE_POS >> 6);

        /* continuous when the tape moved about one block on; else it jumped (seek, rewind) */
        if (was_tape && was_running && p + 4 * BLOCK > next_from && p < next_from + 4 * BLOCK) {
            from = next_from;
            to = p + BLOCK > from ? p + BLOCK : from;
        } else {
            if (ok)
                flush_offs();
            from = p;
            to = p + BLOCK;
        }
    } else if (dr_run && !was_running) {
        if (ok)
            flush_offs();
        from = 0;                               /* play from bar 1 */
        to = BLOCK;
    } else {
        from = next_from;
        to = from + BLOCK;
    }
    if (tape != was_tape && ok)
        flush_offs();                           /* the clock changes: offs refer to the old one */
    was_tape = (uint8_t)tape;
    was_running = dr_run;
    next_from = to;
    dr_clock = from;
    dr_tape = (uint8_t)tape;

    if (ok)                                     /* due note-offs first: a new hit may follow */
        for (i = 0; i < noffs; ) {
            if ((int32_t)(to - offs[i].due) >= 0) {
                note(0, offs[i].note, 0, 0, 0);
                offs[i] = offs[--noffs];
            } else {
                i++;
            }
        }
    st = steps();
    if (ok && dr_run && st_bpm)
        dr_schedule(pat(), from, to - from, st, dr_swing, seed, emit, 0);
    if (ok && preview_req) {
        const dr_lane *ln = lane();
        const dr_step *s = &ln->s[dr_cursor % DR_STEPS];
        uint64_t L = st_bpm ? st[dr_sel & (DR_LANES - 1)] : dr_step_q16(12000, ln->rate);
        int li = s->p[DR_LEN];
        uint32_t hold = (uint32_t)((L * dr_len8[li < 0 || li >= DR_LENS ? DR_LEN_FULL : li] / 8) >> 16);

        preview_req = 0;
        hit(ln, s, from, hold ? hold : 1, li == DR_LEN_FULL);
    }
    if (!ok)
        return;                                 /* note-offs wait for drum playmode (never dropped) */
    for (i = 0; i < noffs; ) {
        if ((int32_t)(to - offs[i].due) >= 0) {
            note(0, offs[i].note, 0, 0, 0);
            offs[i] = offs[--noffs];
        } else {
            i++;
        }
    }
}

/* ------------------------------------------------------------------ KeyHandler thread */

static const uint8_t note_keys[24] = TE_NOTE_KEY_IDS;

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int wrap(int v, int n)
{
    return ((v % n) + n) % n;
}

/* Controls as on the SDK synth engines: a knob turn edits the value under that knob, green press goes
 * to the next page (blue press: step on / off), shift + blue turn browses (the lanes; the engines' bank). Every page puts four
 * functions on the four knobs. White press plays the cursor step. */
enum {
    F_STEP, F_ON, F_VEL, F_PROB, F_TONE, F_LEN, F_RATCH, F_NUDGE, F_SWING, F_ZOOM,
    F_SAMPLE, F_LENGTH, F_RATE, F_MUTE, F_FILTER, F_CUTOFF, F_RESO, F_LEVEL
};
#define DR_PAGES    6
#define PG_CLEAR    5
static const char *const page_name[DR_PAGES] = { "STEP", "NOTE", "TIME", "LANE", "SOUND", "CLEAR" };
static const uint8_t page_fn[DR_PAGES][4] = {
    { F_STEP, F_ON, F_VEL, F_PROB },            /* 1 STEP: which step, hit or not, how loud, how sure */
    { F_STEP, F_TONE, F_LEN, F_RATCH },         /* 2 NOTE: pitch, length, repeats */
    { F_STEP, F_NUDGE, F_SWING, F_ZOOM },       /* 3 TIME: early / late, swing (all), view */
    { F_SAMPLE, F_LENGTH, F_RATE, F_MUTE },     /* 4 LANE: which drum, its own beat */
    { F_FILTER, F_CUTOFF, F_RESO, F_LEVEL },    /* 5 SOUND: the drum's filter and level */
    { F_STEP, F_STEP, F_STEP, F_STEP },         /* 6 CLEAR: no knob functions (push: the dialog) */
};
static const char *const fn_name[] = {
    "STEP", "ON", "VEL", "PROB", "TONE", "LEN", "RATCH", "NUDGE", "SWING", "ZOOM",
    "SAMPLE", "LENGTH", "RATE", "MUTE", "FILTER", "CUTOFF", "RESO", "LEVEL",
};
static const int8_t fn_param[] = {             /* step parameter behind a function, -1 none */
    -1, -1, DR_VEL, DR_PROB, DR_TONE, DR_LEN, DR_RATCH, DR_NUDGE,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

static dr_lane undo_lane;                       /* the lane before shift + orange press cleared it */
static volatile int8_t undo_of = -1;            /* its index (pattern * 8 + lane), -1 none */
volatile uint8_t dr_ask, dr_ask_yes;            /* "CLEAR PATTERN X?" up / YES picked (bench reads) */
static volatile uint8_t dirty, save_now;        /* an edit happened / save at the next main pass */

static void lane_clear(dr_lane *l)
{
    int key = l->key, len = l->len, rate = l->rate, level = l->level;

    dr_lane_init(l, key);
    l->len = (uint8_t)len;
    l->rate = (uint8_t)rate;
    l->level = (uint8_t)level;
}

static void select_lane(int l)
{
    dr_sel = (uint8_t)clampi(l, 0, DR_LANES - 1);
    dr_cursor = (uint8_t)(dr_cursor % lane()->len);
}

static void clear_pattern(void)
{
    dr_pattern_init(pat());                     /* as new: lanes on keys 1..8, 16 steps, empty */
    if (undo_of >= 0 && undo_of / DR_LANES == (dr_cur & (DR_PATTERNS - 1)))
        undo_of = -1;
    dr_cursor = 0;
    select_lane(dr_sel);
}

static void edit(int f, int d)
{
    dr_lane *l = lane();
    dr_step *s = &l->s[dr_cursor % DR_STEPS];
    filt *fl = &filters[l->key % DR_KEYS];
    int p = fn_param[f];

    switch (f) {
    case F_STEP:   dr_cursor = (uint8_t)wrap(dr_cursor + d, l->len); break;
    case F_ON:     s->on = d > 0; break;    /* right = hit, left = rest */
    case F_SWING:  dr_swing = (uint8_t)clampi(dr_swing + d * 5, 0, 100); break;
    case F_ZOOM:   dr_zoom = (uint8_t)clampi(dr_zoom + d, 0, 2); break;
    case F_SAMPLE: l->key = (uint8_t)wrap(l->key + d, DR_KEYS); break;
    case F_LENGTH:
        l->len = (uint8_t)clampi(l->len + d, 1, DR_STEPS);
        if (dr_cursor >= l->len)
            dr_cursor = (uint8_t)(l->len - 1);
        break;
    case F_RATE:   l->rate = (uint8_t)clampi(l->rate + d, 0, DR_RATES - 1); break;
    case F_MUTE:   l->mute = d > 0; break;
    case F_FILTER: fl->type = (uint8_t)clampi(fl->type + d, 0, DR_FILTERS - 1); break;
    case F_CUTOFF: fl->cut = (uint8_t)clampi(fl->cut + d * 2, 0, 127); break;
    case F_RESO:   fl->res = (uint8_t)clampi(fl->res + d * 4, 0, 127); break;
    case F_LEVEL:  l->level = (uint8_t)clampi(l->level + d * 5, 0, 100); break;
    default:
        if (p >= 0) {
            int st = p == DR_VEL || p == DR_PROB || p == DR_NUDGE ? 5 : 1;

            s->p[p] = (int8_t)dr_param_clamp(p, s->p[p] + d * st);
            dr_param = (uint8_t)p;              /* the graph follows the last edited one */
        }
    }
    if (f >= F_FILTER && f <= F_RESO)
        set_filter(l->key % DR_KEYS);
    /* stopped: hear the step while shaping it */
    if (!dr_run && (f == F_VEL || f == F_TONE || f == F_LEN || f == F_RATCH || (f == F_ON && d > 0)))
        preview_req = 1;
}

/* shift + press: green close, white play / stop, orange clear the lane (again: undo) */
static void action(int k)
{
    int at = (dr_cur & (DR_PATTERNS - 1)) * DR_LANES + (dr_sel & (DR_LANES - 1));

    if (k == 1) {
        set_shown(0);
        save_now = 1;                           /* closing: keep it now, not ~3 s later */
    } else if (k == 2) {
        dr_run ^= 1;
    } else if (k == 3) {
        dr_lane *l = lane();

        if (undo_of == at) {
            *l = undo_lane;
            undo_of = -1;
        } else {
            undo_lane = *l;
            undo_of = (int8_t)at;
            lane_clear(l);
        }
    }
}

int dr_key(int key, int down)
{
    int k = key - KEY_PUSH, i;

    if (!inited)
        return 0;
    if (k >= 0 && k <= 3) {
        if (!down) {
            if (consumed & (1 << k)) {
                consumed &= (uint8_t)~(1 << k);
                return 1;
            }
            return 0;
        }
        /* tape screen, drum playmode: shift + white press = play / stop, as inside DRUM ROLL, so the
         * pattern can be started there and recorded (it follows the tape while the tape runs) */
        if (k == 2 && *TE_SHIFT_HELD && *TE_PLAYMODE == 1 && *TE_EDIT_MODE == EDIT_TAPE
            && !(&rq_state && rq_state >= RQ_PANEL)) {
            dr_run ^= 1;
            consumed |= 1 << k;
            return 1;
        }
        if (!drum_screen())
            return 0;
        if (!shown()) {
            dr_ask = 0;
            if (k != 1 || !*TE_SHIFT_HELD)
                return 0;
            set_shown(1);                       /* shift + green press: open */
            consumed |= 1 << k;
            return 1;
        }
        /* open: every knob press is ours (shift + blue press too: no requantize take from here) */
        if (dr_ask) {                           /* the dialog: any push = ok on the picked answer */
            if (dr_ask_yes)
                clear_pattern();
            dr_ask = 0;
        } else if (*TE_SHIFT_HELD) {
            action(k);
        } else if (dr_page == PG_CLEAR && k != 1) {
            dr_ask = 1;                         /* blue / white / orange push: ask, NO picked */
            dr_ask_yes = 0;
        } else if (k == 0) {
            edit(F_ON, lane()->s[dr_cursor % DR_STEPS].on ? -1 : 1);   /* blue: step on / off */
        } else if (k == 1) {
            dr_page = (uint8_t)(dr_page + 1 < DR_PAGES ? dr_page + 1 : 0);     /* green: next page */
        } else if (k == 2) {
            preview_req = 1;                    /* white: hear the cursor step */
        }
        refresh_gains();
        dirty = 1;
        consumed |= 1 << k;
        return 1;
    }
    /* a drum key selects its lane (TE still plays it) */
    if (down && shown())
        for (i = 0; i < 24; i++)
            if (note_keys[i] == key) {
                int l;

                for (l = 0; l < DR_LANES; l++)
                    if (pat()->lane[l].key == i) {
                        select_lane(l);
                        break;
                    }
                break;
            }
    return 0;
}

int dr_knob(int knob, int16_t *diff)
{
    int d = *diff > 0 ? 1 : *diff < 0 ? -1 : 0;

    if (!inited || !shown() || (unsigned int)knob > 3)
        return 0;
    if (!d)
        return 1;
    if (dr_ask) {
        dr_ask_yes = d > 0;                     /* any knob: left NO, right YES */
        return 1;
    }
    if (*TE_SHIFT_HELD) {
        if (knob == 0)
            select_lane(dr_sel + d);            /* browse lanes */
        else if (knob == 1) {
            dr_cur = (uint8_t)clampi(dr_cur + d, 0, DR_PATTERNS - 1);
            select_lane(dr_sel);
        }
    } else if (dr_page != PG_CLEAR) {
        edit(page_fn[dr_page % DR_PAGES][knob], d);
    }
    refresh_gains();
    dirty = 1;
    return 1;                                   /* all four knobs, with and without shift */
}

/* ------------------------------------------------------------------ GUI thread */

#define HDR_H       13
#define LANE_Y      15
#define LANE_H      11
#define GRID_X      50
#define GRID_W      268
#define PANEL_Y     (LANE_Y + DR_LANES * LANE_H + 2)

static const uint16_t col[4] = {
    GFX_RGB(0x69, 0x8e, 0xff), GFX_RGB(0x00, 0xed, 0x95),           /* blue, green */
    GFX_RGB(0xe6, 0xe6, 0xe6), GFX_RGB(0xff, 0x6a, 0x3d),           /* white, orange */
};
static const uint16_t c_bg = GFX_RGB(0x0c, 0x0c, 0x10), c_sel = GFX_RGB(0x26, 0x26, 0x36),
                      c_bar = GFX_RGB(0x5a, 0x58, 0x66), c_beat = GFX_RGB(0x2c, 0x2b, 0x34),
                      c_off = GFX_RGB(0x3a, 0x39, 0x44), c_dim = GFX_RGB(0x7c, 0x79, 0x90),
                      c_red = GFX_RGB(0xff, 0x3a, 0x5d);
static const char *const rate_short[DR_RATES] = { "/4", "/8", "/8T", "/16", "/16T", "/32" };

/* c at pct % brightness (pct 0..100) */
static uint16_t shade(uint16_t c, int pct)
{
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31, m = pct * 1311 >> 10;     /* 0..128 */

    return (uint16_t)(((r * m >> 7) << 11) | ((g * m >> 7) << 5) | (b * m >> 7));
}

/* view in whole samples (32-bit maths: the frame runs ~30 times a second) */
static uint32_t v0, vlen;                       /* first sample, samples shown */
static uint32_t vscale;                         /* pixels per sample, Q20 */

static int xs(uint32_t t)                       /* x of clock sample t */
{
    int32_t d = (int32_t)(t - v0), lim = (int32_t)vlen * 2;

    d = d < -lim ? -lim : d > lim ? lim : d;    /* |d * vscale| < 2^31 */
    return GRID_X + (int)((d * (int32_t)vscale) >> 20);
}

static int xof(uint64_t t)                      /* x of a 16.16 time */
{
    return xs((uint32_t)(t >> 16));
}

static void frame_rect(int x, int y, int w, int h, uint16_t c)
{
    gfx_rect(x, y, w, 1, c);
    gfx_rect(x, y + h - 1, w, 1, c);
    gfx_rect(x, y, 1, h, c);
    gfx_rect(x + w - 1, y, 1, h, c);
}

static int clipx(int *x0, int *x1)
{
    if (*x0 < GRID_X)
        *x0 = GRID_X;
    if (*x1 > GRID_X + GRID_W)
        *x1 = GRID_X + GRID_W;
    return *x1 > *x0;
}

static int key_text(int x, int y, int key, uint16_t c, int scale)    /* "K01" .. "K24" */
{
    x = gfx_text(x, y, key + 1 < 10 ? "K0" : "K", c, scale);
    return gfx_dec(x, y, key + 1, c, scale);
}

static int param_text(int x, int y, int p, int v, uint16_t c, int scale)
{
    if (p == DR_LEN)
        return gfx_text(x, y, dr_len_name[clampi(v, 0, DR_LENS - 1)], c, scale);
    if (p == DR_RATCH)
        x = gfx_text(x, y, "X", c, scale);
    else if ((p == DR_TONE || p == DR_NUDGE) && v > 0)
        x = gfx_text(x, y, "+", c, scale);
    return gfx_dec(x, y, v, c, scale);
}

static void draw_lane(int l, uint64_t beat_q16)
{
    const dr_lane *ln = &pat()->lane[l];
    uint16_t lc = ln->mute ? c_off : col[l & 3];
    uint64_t L = lane_step(l);
    int y = LANE_Y + l * LANE_H, len = ln->len < 1 ? 1 : ln->len;
    uint32_t k, k0, k1, si;

    (void)beat_q16;
    if (l == dr_sel)
        gfx_rect(0, y, GRID_X - 2, LANE_H - 1, c_sel);
    gfx_rect(0, y + 1, 2, LANE_H - 3, ln->mute ? c_red : lc);
    key_text(4, y + 3, ln->key, GFX_WHITE, 1);
    gfx_dec(len < 10 ? 24 : 20, y + 3, len, c_dim, 1);
    gfx_text(29, y + 3, rate_short[ln->rate % DR_RATES], c_dim, 1);
    if (!(L >> 16))
        return;
    k0 = v0 / ((uint32_t)(L >> 16) + 1);        /* never past the first visible step */
    k1 = (v0 + vlen) / (uint32_t)(L >> 16) + 1;
    for (k = k0, si = k0 % (uint32_t)len; k < k1; k++, si = si + 1 == (uint32_t)len ? 0 : si + 1) {
        const dr_step *s = &ln->s[si];
        int x0 = xof(k * L) + 1, x1 = xof((k + 1) * L), ghost = k >= (uint32_t)len, w;
        int r = s->p[DR_RATCH] < 1 ? 1 : s->p[DR_RATCH], h, j;

        if (!clipx(&x0, &x1))
            continue;
        w = x1 - x0;
        if (!s->on) {
            gfx_rect(x0, y + LANE_H - 3, w, 1, ghost ? c_beat : c_off);
            continue;
        }
        {
            uint16_t c = shade(lc, (ghost ? 15 : 35) + (ghost ? 25 : 65) * s->p[DR_VEL] / 100);

            for (h = 0; h < r; h++) {           /* a ratchet step: its hits */
                int a = x0 + w * h / r, b = x0 + w * (h + 1) / r - (r > 1 && h < r - 1 ? 1 : 0);

                if (s->p[DR_PROB] < 100) {      /* probability: dotted */
                    for (j = a; j < b; j += 2)
                        gfx_rect(j, y + 1, 1, LANE_H - 3, c);
                } else if (b > a) {
                    gfx_rect(a, y + 1, b - a, LANE_H - 3, c);
                }
            }
            if (s->p[DR_TONE] && w > 2)
                gfx_rect(x1 - 2, s->p[DR_TONE] > 0 ? y + 1 : y + LANE_H - 4, 2, 2, GFX_WHITE);
        }
    }
    /* loop line where the lane starts over */
    for (k = (uint32_t)len; (uint32_t)(((uint64_t)k * L) >> 16) < v0 + vlen; k += (uint32_t)len) {
        int xl = xof(k * L);

        if (xl >= GRID_X && xl < GRID_X + GRID_W)
            gfx_rect(xl, y, 1, LANE_H - 1, lc);
    }
    /* cursor (first pass of the lane) and playhead */
    if (l == dr_sel) {
        int x0 = xof((uint64_t)dr_cursor * L), x1 = xof((uint64_t)(dr_cursor + 1) * L) + 1;

        if (clipx(&x0, &x1))
            frame_rect(x0, y, x1 - x0, LANE_H - 1, GFX_WHITE);
    }
    if (dr_run) {
        int ph = dr_playhead(ln, dr_clock, L);

        if (ph >= 0) {
            int x0 = xof((uint64_t)ph * L) + 1, x1 = xof((uint64_t)(ph + 1) * L);

            if (clipx(&x0, &x1))
                gfx_rect(x0, y + LANE_H - 2, x1 - x0, 1, GFX_WHITE);
        }
    }
}

static void draw_grid_lines(uint64_t beat)
{
    uint32_t k;
    int bottom = LANE_Y + DR_LANES * LANE_H;

    if (!(beat >> 16))
        return;
    for (k = v0 / (uint32_t)(beat >> 16); (uint32_t)(((uint64_t)k * beat) >> 16) <= v0 + vlen; k++) {
        int x = xof(k * beat);

        if (x >= GRID_X && x < GRID_X + GRID_W)
            gfx_rect(x, LANE_Y, 1, bottom - LANE_Y, k % 4 ? c_beat : c_bar);
    }
}

static void draw_header(void)
{
    uint32_t bpm = st_bpm;
    uint64_t beat = dr_step_q16(bpm ? bpm : 12000, 0);
    int x;

    gfx_rect(0, 0, gfx_w, HDR_H, GFX_RGB(0x18, 0x18, 0x22));
    gfx_rect(2, 1, 11, 11, col[3]);
    {
        char p[2] = { (char)('A' + (dr_cur & 3)), 0 };

        gfx_text(4, 2, p, GFX_BLACK, 2);
    }
    if (dr_run)
        x = gfx_text(18, 2, dr_tape ? "TAPE" : "PLAY", dr_tape ? c_red : col[1], 2);
    else
        x = gfx_text(18, 2, "STOP", c_dim, 2);
    if (dr_run && beat) {
        uint32_t b = (uint32_t)(((uint64_t)dr_clock << 16) / beat);

        x = gfx_dec(x + 8, 2, (int32_t)(b / 4 + 1), GFX_WHITE, 2);
        x = gfx_text(x, 2, ".", GFX_WHITE, 2);
        x = gfx_dec(x, 2, (int32_t)(b % 4 + 1), GFX_WHITE, 2);
    }
    x = gfx_dec(x + 10, 2, (int32_t)((bpm + 50) / 100), GFX_WHITE, 2);
    x = gfx_text(x + 2, 6, "BPM", c_dim, 1);
    if (dr_swing) {
        x = gfx_text(x + 8, 2, "SW", c_dim, 2);
        gfx_dec(x + 2, 2, dr_swing, GFX_WHITE, 2);
    }
    {
        int pg = dr_page % DR_PAGES, i;         /* "P1 STEP" + page dots, as the engines' "P1" */
        char n[3] = { 'P', (char)('1' + pg), 0 };

        x = gfx_w - 2 - 8 * 8 - DR_PAGES * 4 - 4;
        for (i = 0; i < DR_PAGES; i++)
            gfx_rect(x + i * 4, 5, 2, 3, i == pg ? GFX_WHITE : c_off);
        x = gfx_text(x + DR_PAGES * 4 + 2, 2, n, GFX_WHITE, 2);
        gfx_text(x + 6, 2, page_name[pg], col[1], 2);
    }
}

/* graph of the cursor parameter over the lane's first pass (STEP focus) */
static void draw_graph(void)
{
    const dr_lane *ln = lane();
    int p = dr_param % DR_PARAMS, top = PANEL_Y + 26, h = 14, mid = top + h / 2, len = ln->len;
    int lo = dr_pmin[p], hi = dr_pmax[p], bip = lo < 0, k;
    uint64_t L = lane_step(dr_sel);

    gfx_text(2, top + 1, dr_pname[p], c_dim, 1);
    gfx_text(2, top + 8, "PER STEP", c_dim, 1);
    if (bip)
        gfx_rect(GRID_X, mid, GRID_W, 1, c_beat);
    else
        gfx_rect(GRID_X, top + h, GRID_W, 1, c_beat);
    for (k = 0; k < len && L; k++) {
        const dr_step *s = &ln->s[k];
        int x0 = xof((uint64_t)k * L) + 1, x1 = xof((uint64_t)(k + 1) * L) - 1, v = s->p[p], y0, y1;
        uint16_t c = k == dr_cursor ? GFX_WHITE : s->on ? col[dr_sel & 3] : c_off;

        if (!clipx(&x0, &x1))
            continue;
        if (x1 - x0 > 6) {
            int m = (x0 + x1) / 2;              /* thin bars, as TE draws levels */

            x0 = m - 2;
            x1 = m + 2;
        }
        if (!s->on && k != dr_cursor) {
            gfx_rect(x0, bip ? mid : top + h - 1, x1 - x0, 1, c_off);      /* off: just a mark */
            continue;
        }
        if (bip) {
            int d = v * (h / 2) / hi;

            y0 = d > 0 ? mid - d : mid;
            y1 = d > 0 ? mid : mid - d;
            if (y1 == y0)
                y1++;
        } else {
            y1 = top + h;
            y0 = y1 - (v - lo + 1) * h / (hi - lo + 1);
        }
        gfx_rect(x0, y0, x1 - x0, y1 - y0, c);
    }
}

/* filter response sketch (dB, -24 .. +12) in w x h at (x, y) */
static int ilog2_q8(uint64_t v)                 /* log2(v) * 256, v >= 1 */
{
    int e = 63;

    while (e && !(v >> e))
        e--;
    return e * 256 + (int)(((v << (63 - e)) >> 55) & 0xff);
}

static void draw_curve(int x, int y, int w, int h, const filt *f)
{
    dr_coef c;
    int i, prev = -1;

    dr_coef_set(&c, f->type, f->cut, f->res);
    gfx_rect(x, y + h / 3, w, 1, c_beat);       /* 0 dB */
    for (i = 0; i < w; i++) {
        int u = i * 127 / (w - 1), d = u - f->cut;
        /* r = f / fc = 1000^(d / 127) = 2^e, e = d * log2(1000) / 127 (Q8: 9.966 * 256 = 2551) */
        int32_t e_q8 = d * 2551 / 127;
        uint64_t r2, kq, num, den, h2;
        int db, yy;

        {
            int ei = e_q8 >> 8, ef = e_q8 & 255;
            uint64_t r = (uint64_t)(65536 + ef * 256);      /* 2^frac ~ 1 + frac: a sketch (Q16) */

            r = ei >= 0 ? r << (ei > 20 ? 20 : ei) : r >> (-ei > 40 ? 40 : -ei);
            r2 = (r * r) >> 16;                 /* (f / fc)^2, Q16 */
        }
        if (r2 > ((uint64_t)1 << 40))
            r2 = (uint64_t)1 << 40;
        kq = ((uint64_t)c.kh * 2) >> 15;        /* damping, Q16 */
        {
            int64_t a = 65536 - (int64_t)r2;    /* 1 - r^2 */
            uint64_t a2 = (uint64_t)(a < 0 ? -a : a);

            den = ((a2 >> 8) * (a2 >> 8)) + (((kq * kq) >> 16) * r2 >> 16);   /* Q16 */
        }
        if (f->type == DR_F_LP)
            num = (uint64_t)1 << 16;
        else if (f->type == DR_F_HP)
            num = (r2 >> 8) * (r2 >> 8);
        else if (f->type == DR_F_BP)
            num = ((kq * kq) >> 16) * r2 >> 16;
        else
            num = den;
        h2 = den ? (num << 16) / den : 65536;   /* |H|^2, Q16 */
        db = h2 ? (ilog2_q8(h2) - 16 * 256) * 3 / 256 : -99;     /* 10 log10 = 3.01 log2 */
        yy = y + h / 3 - db * h / 36;
        yy = yy < y ? y : yy > y + h - 1 ? y + h - 1 : yy;
        if (prev >= 0) {
            int a = prev < yy ? prev : yy, b = prev < yy ? yy : prev;

            gfx_rect(x + i, a, 1, b - a + 1, col[2]);
        }
        prev = yy;
    }
}

/* value of a function as text */
static int fn_text(int f, int x, int y, uint16_t c, int scale)
{
    const dr_lane *ln = lane();
    const dr_step *s = &ln->s[dr_cursor % DR_STEPS];
    const filt *fl = &filters[ln->key % DR_KEYS];

    switch (f) {
    case F_STEP:   x = gfx_dec(x, y, dr_cursor + 1, c, scale); x = gfx_text(x, y, "/", c_dim, scale);
                   return gfx_dec(x, y, ln->len, c_dim, scale);
    case F_ON:     return gfx_text(x, y, s->on ? "ON" : "OFF", s->on ? c : c_dim, scale);
    case F_SWING:  return gfx_dec(x, y, dr_swing, c, scale);
    case F_ZOOM:   x = gfx_dec(x, y, 1 << (dr_zoom % 3), c, scale); return gfx_text(x, y, " BAR", c, 1);
    case F_SAMPLE: return key_text(x, y, ln->key, c, scale);
    case F_LENGTH: return gfx_dec(x, y, ln->len, c, scale);
    case F_RATE:   return gfx_text(x, y, dr_rate_name[ln->rate % DR_RATES], c, scale);
    case F_MUTE:   return gfx_text(x, y, ln->mute ? "MUTE" : "OFF", ln->mute ? c_red : c_dim, scale);
    case F_FILTER: return gfx_text(x, y, dr_filter_name[fl->type % DR_FILTERS], c, scale);
    case F_CUTOFF: {
        uint32_t hz = 20, i;                    /* 20 Hz * 1000^(cut / 127) */

        for (i = 0; i < fl->cut; i++)
            hz = hz * 1056 / 1000 + (i & 1);    /* 1000^(1/127) = 1.0559 */
        if (hz < 1000)
            return gfx_dec(x, y, (int32_t)hz, c, scale);
        x = gfx_dec(x, y, (int32_t)(hz / 1000), c, scale);
        x = gfx_text(x, y, ".", c, scale);
        x = gfx_dec(x, y, (int32_t)(hz / 100 % 10), c, scale);
        return gfx_text(x, y, "K", c, scale);
    }
    case F_RESO:   return gfx_dec(x, y, fl->res, c, scale);
    case F_LEVEL:  return gfx_dec(x, y, ln->level, c, scale);
    default:
        return param_text(x, y, fn_param[f], s->p[fn_param[f]], c, scale);
    }
}

/* 0..256 for the value bar; bipolar functions are drawn from the middle */
static int fn_frac(int f, int *bip)
{
    const dr_lane *ln = lane();
    const dr_step *s = &ln->s[dr_cursor % DR_STEPS];
    const filt *fl = &filters[ln->key % DR_KEYS];
    int p = fn_param[f];

    *bip = f == F_TONE || f == F_NUDGE;
    switch (f) {
    case F_STEP:   return ln->len > 1 ? dr_cursor * 256 / (ln->len - 1) : 0;
    case F_ON:     return s->on ? 256 : 0;
    case F_SWING:  return dr_swing * 256 / 100;
    case F_ZOOM:   return (dr_zoom % 3) * 128;
    case F_SAMPLE: return ln->key * 256 / (DR_KEYS - 1);
    case F_LENGTH: return (ln->len - 1) * 256 / (DR_STEPS - 1);
    case F_RATE:   return ln->rate * 256 / (DR_RATES - 1);
    case F_MUTE:   return ln->mute ? 256 : 0;
    case F_FILTER: return fl->type * 256 / (DR_FILTERS - 1);
    case F_CUTOFF: return fl->cut * 256 / 127;
    case F_RESO:   return fl->res * 256 / 127;
    case F_LEVEL:  return ln->level * 256 / 100;
    default:
        return (s->p[p] - dr_pmin[p]) * 256 / (dr_pmax[p] - dr_pmin[p]);
    }
}

#define COL_W   78

static void draw_clear_page(void)
{
    char p[2] = { (char)('A' + (dr_cur & 3)), 0 };
    int x = gfx_text(2, PANEL_Y + 4, "CLEAR PATTERN", GFX_WHITE, 2);

    gfx_text(x + 8, PANEL_Y + 4, p, col[3], 2);
    gfx_text(2, PANEL_Y + 18, "ALL 8 LANES START NEW: EMPTY - 16 STEPS - KEYS 1-8", c_dim, 1);
    gfx_text(2, PANEL_Y + 26, "OTHER PATTERNS AND THE FILTERS STAY", c_dim, 1);
    x = gfx_text(2, PANEL_Y + 34, "PUSH", c_dim, 1) + 4;
    x = gfx_text(x, PANEL_Y + 34, "BLUE", col[0], 1) + 4;
    x = gfx_text(x, PANEL_Y + 34, "WHITE", col[2], 1) + 4;
    x = gfx_text(x, PANEL_Y + 34, "OR", c_dim, 1) + 4;
    x = gfx_text(x, PANEL_Y + 34, "ORANGE", col[3], 1) + 4;
    gfx_text(x, PANEL_Y + 34, "TO CLEAR - ASKS FIRST", c_dim, 1);
}

/* "CLEAR PATTERN X?" over the lanes: turn picks NO / YES, push = ok */
#define ASK_W   184
#define ASK_H   62

static void draw_ask(void)
{
    int x0 = (gfx_w - ASK_W) / 2, y0 = LANE_Y + 10, i;
    char p[3] = { (char)('A' + (dr_cur & 3)), '?', 0 };

    gfx_rect(x0, y0, ASK_W, ASK_H, GFX_BLACK);
    frame_rect(x0, y0, ASK_W, ASK_H, GFX_WHITE);
    i = gfx_text(x0 + 12, y0 + 8, "CLEAR PATTERN", GFX_WHITE, 2);
    gfx_text(i + 8, y0 + 8, p, col[3], 2);
    for (i = 0; i < 2; i++) {                   /* NO left, YES right */
        int on = i == (dr_ask_yes != 0), bx = x0 + 22 + i * 76, by = y0 + 26;
        uint16_t c = i ? c_red : GFX_WHITE;

        if (on)
            gfx_rect(bx, by, 64, 18, c);
        else
            frame_rect(bx, by, 64, 18, c_off);
        gfx_text(bx + (i ? 20 : 24), by + 4, i ? "YES" : "NO", on ? GFX_BLACK : c_dim, 2);
    }
    gfx_text(x0 + 12, y0 + ASK_H - 10, "TURN: NO / YES   PUSH: OK", c_dim, 1);
}

static void draw_panel(void)
{
    int pg = dr_page % DR_PAGES, i, y = PANEL_Y + 2;

    if (pg == PG_CLEAR) {
        draw_clear_page();
        return;
    }
    for (i = 0; i < 4; i++) {
        int f = page_fn[pg][i], x = 2 + i * COL_W, bip, v = fn_frac(f, &bip), bw = COL_W - 8;

        gfx_text(x, y, fn_name[f], col[i], 1);
        fn_text(f, x, y + 7, GFX_WHITE, 2);
        gfx_rect(x, y + 19, bw, 2, c_beat);
        if (bip) {
            int m = x + bw / 2, e = x + v * bw / 256;

            gfx_rect(e < m ? e : m, y + 19, (e < m ? m - e : e - m) + 1, 2, col[i]);
        } else {
            gfx_rect(x, y + 19, v * bw / 256, 2, col[i]);
        }
    }
    if (pg <= 2) {
        draw_graph();                           /* step pages: the lane's steps */
    } else if (pg == 4) {
        const filt *f = &filters[lane()->key % DR_KEYS];

        if (f->type != DR_F_OFF)
            draw_curve(GRID_X, PANEL_Y + 26, GRID_W, 18, f);
        else
            gfx_text(GRID_X, PANEL_Y + 32, "FILTER OFF: TURN BLUE FOR LP BP HP", c_dim, 1);
    } else {
        int x = gfx_text(2, PANEL_Y + 29, "LANE", c_dim, 1);

        x = gfx_dec(x + 4, PANEL_Y + 29, dr_sel + 1, GFX_WHITE, 1);
        x = gfx_text(x + 8, PANEL_Y + 29, "PLAYS", c_dim, 1);
        x = key_text(x + 4, PANEL_Y + 29, lane()->key, GFX_WHITE, 1);
        x = gfx_text(x + 8, PANEL_Y + 29, "LOOP", c_dim, 1);
        x = gfx_dec(x + 4, PANEL_Y + 29, lane()->len, GFX_WHITE, 1);
        x = gfx_text(x + 4, PANEL_Y + 29, "X", c_dim, 1);
        gfx_text(x + 4, PANEL_Y + 29, dr_rate_name[lane()->rate % DR_RATES], GFX_WHITE, 1);
        gfx_text(2, PANEL_Y + 38, "DRUM KEYS PICK THEIR LANE", c_dim, 1);
    }
}

static void draw_hints(void)
{
    int x = 2, y = gfx_h - 7;

    x = gfx_text(x, y, "PUSH", c_dim, 1) + 4;
    x = gfx_text(x, y, dr_page == PG_CLEAR ? "CLEAR" : "ON", col[0], 1) + 4;
    x = gfx_text(x, y, "PAGE", col[1], 1) + 4;
    x = gfx_text(x, y, dr_page == PG_CLEAR ? "CLEAR" : "HEAR", col[2], 1) + 8;
    x = gfx_text(x, y, "SHIFT+TURN", c_dim, 1) + 4;
    x = gfx_text(x, y, "LANE", col[0], 1) + 4;
    x = gfx_text(x, y, "PATTERN", col[1], 1) + 8;
    x = gfx_text(x, y, "SHIFT+PUSH", c_dim, 1) + 4;
    x = gfx_text(x, y, "CLOSE", col[1], 1) + 4;
    x = gfx_text(x, y, dr_run ? "STOP" : "PLAY", col[2], 1) + 4;
    gfx_text(x, y, "CLEAR", col[3], 1);
}

void dr_frame(uint16_t *buf)
{
    uint32_t bpm;
    uint64_t beat;
    uint64_t L;
    int l;

    if (!inited || *TE_PLAYMODE != 1)
        return;
    gfx_begin(buf);
    if (!shown()) {
        dr_ask = 0;                             /* left the screen: the question is gone */
        if (dr_run) {                           /* badge: running in the background */
            int x;

            gfx_rect(236, 0, 82, 9, GFX_BLACK);
            x = gfx_text(238, 2, "DRUM ROLL", col[1], 1);
            {
                char p[2] = { (char)('A' + (dr_cur & 3)), 0 };

                gfx_text(x + 4, 2, p, GFX_WHITE, 1);
            }
        }
        return;
    }
    bpm = st_bpm;
    beat = dr_step_q16(bpm ? bpm : 12000, 0);
    L = lane_step(dr_sel);
    vlen = (uint32_t)((beat * 4) >> 16) << (dr_zoom % 3);    /* 1 / 2 / 4 bars */
    v0 = vlen ? (uint32_t)(((uint64_t)dr_cursor * L) >> 16) / vlen * vlen : 0;
    vscale = vlen ? (uint32_t)(((uint64_t)GRID_W << 20) / vlen) : 0;

    gfx_rect(0, 0, gfx_w, gfx_h, c_bg);
    draw_header();
    draw_grid_lines(beat);
    for (l = 0; l < DR_LANES; l++)
        draw_lane(l, beat);
    gfx_rect(0, PANEL_Y - 1, gfx_w, 1, c_beat);
    draw_panel();
    draw_hints();
    if (dr_ask)
        draw_ask();
}

/* ------------------------------------------------------------------ main thread: op1.db
 * Rows in TE's own database, table sdk_drumroll (id 0..3 patterns A-D, 4 settings), the way TE keeps
 * its sequencers (easseq, endless). TE's db helpers assert on any SQLite error, so: the table is
 * created first, every statement is ours and fixed apart from base64 (no quote, no '%'), < 8 KB.
 * Reads use the same lock and calls as TE's own query, minus its JSON step (a row that is not
 * ours is skipped, never parsed). */
#define SAVE_WAIT   375                         /* main-loop passes (~8 ms) without an edit: ~3 s */
#define STEP_TRIES  100

static char row[DR_ROW_TEXT_MAX];               /* one row's text (load: a copy of the column) */
static char sql[DR_ROW_TEXT_MAX + 96];
static uint8_t loaded, pending;
static uint16_t quiet;
static uint32_t saved_hash[DR_ROWS];

static void put(char **o, const char *s)
{
    while (*s)
        *(*o)++ = *s++;
}

/* row id's data -> row (0-ended); 0 if there is none (or it cannot be read) */
static int db_read(void *db, int id)
{
    te_db_query q = { db, 0 };
    char *o = sql;
    int rc = 0, n = 0, i;
    void *sem = *TE_DB_LOCK;

    put(&o, "SELECT data FROM sdk_drumroll WHERE id=");
    *o++ = (char)('0' + id);
    *o = 0;
    row[0] = 0;
    TE_SEM_PEND(sem, 0);
    TE_DB_PREPARE(&q, sql);
    if (q.stmt) {
        for (i = 0; i < STEP_TRIES; i++) {
            rc = TE_SQLITE_STEP(q.stmt);
            if (rc == 100 || rc == 101)
                break;
        }
        if (rc == 100) {
            const char *t = TE_SQLITE_COLUMN_TEXT(q.stmt, 0);

            while (t && t[n] && n < DR_ROW_TEXT_MAX - 1) {
                row[n] = t[n];
                n++;
            }
            if (t && t[n])
                n = 0;                          /* longer than any row of ours: not ours */
            row[n] = 0;
        }
        TE_SQLITE_FINALIZE(q.stmt);
    }
    TE_SEM_POST(sem);
    return n;
}

static void db_write(void *db, int id, const char *text)
{
    char *o = sql;

    put(&o, "INSERT OR REPLACE INTO sdk_drumroll (id, data) VALUES (");
    *o++ = (char)('0' + id);
    put(&o, ",'");
    put(&o, text);
    put(&o, "')");
    *o = 0;
    TE_DB_EXEC(db, sql);
}

static void settings_get(dr_settings *s)
{
    int i;

    s->cur = dr_cur;
    s->swing = dr_swing;
    s->zoom = dr_zoom;
    s->pad = 0;
    for (i = 0; i < DR_KEYS; i++)
        s->filt[i] = filters[i];
}

static uint32_t row_hash(int id)
{
    dr_settings s;

    if (id < DR_PATTERNS)
        return dr_hash(&dr_pat[id], (int)sizeof(dr_pattern));
    settings_get(&s);
    return dr_hash(&s, (int)sizeof(s));
}

static void load_all(void *db)
{
    static dr_pattern p;
    dr_settings s;
    int id, i;

    TE_DB_EXEC(db, "CREATE TABLE IF NOT EXISTS sdk_drumroll (id INTEGER PRIMARY KEY, data TEXT NOT NULL)");
    for (id = 0; id < DR_PATTERNS; id++)
        if (db_read(db, id) && dr_load_pattern(id, row, &p) == 0)
            dr_pat[id] = p;
    if (db_read(db, DR_ROW_SETTINGS) && dr_load_settings(row, DR_FILTERS, &s) == 0) {
        dr_cur = s.cur;
        dr_swing = s.swing;
        dr_zoom = s.zoom;
        for (i = 0; i < DR_KEYS; i++) {
            filters[i] = s.filt[i];
            set_filter(i);
        }
    }
    select_lane(dr_sel);
    refresh_gains();
    for (id = 0; id < DR_ROWS; id++)
        saved_hash[id] = row_hash(id);          /* as stored (or defaults): nothing to write yet */
}

static void save_changed(void *db)
{
    int id;

    for (id = 0; id < DR_ROWS; id++) {
        uint32_t h = row_hash(id);

        if (h == saved_hash[id])
            continue;
        if (id < DR_PATTERNS) {
            dr_store_pattern(id, &dr_pat[id], row);
        } else {
            dr_settings s;

            settings_get(&s);
            dr_store_settings(&s, row);
        }
        db_write(db, id, row);
        saved_hash[id] = h;
    }
}

void dr_main(void)
{
    void *db;

    if (!inited)
        return;
    db = TE_DB_OP1();
    if (!db || !*TE_DB_LOCK)                    /* TE has not opened / used op1.db yet */
        return;
    if (!loaded) {
        load_all(db);
        loaded = 1;
        return;
    }
    if (dirty) {                                /* an edit: wait until it is quiet again */
        dirty = 0;
        pending = 1;
        quiet = 0;
    }
    if (save_now) {
        save_now = 0;
        quiet = SAVE_WAIT;
    }
    if (!pending || ++quiet < SAVE_WAIT)
        return;
    pending = 0;
    save_changed(db);                           /* a later edit sets dirty again: saved next round */
}
