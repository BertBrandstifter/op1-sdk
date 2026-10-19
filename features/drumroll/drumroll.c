/* DRUM ROLL (spec: README.md): the finger sequencer's step editor. FL-style channel rack drawn to
 * scale over TE's finger screen; a finger pattern edited here plays with 8 lanes, per-step VEL TONE
 * LEN NUDGE PROB RATCH, per-sample filter + level.
 *
 * dr_init         defaults (14 empty patterns, lanes on keys 1..8, filters off)
 * dr_fs_get_note  call site in TE's drum finger player (one call per artist step and hand): records
 *                 the step events, returns -1 for DRUM ROLL's patterns (TE plays nothing of them)
 * dr_fs_set_note  the 4 call sites where TE's finger screen writes a note (step record, delete,
 *                 clear): TE's write, then DRUM ROLL follows it (import / merge)
 * dr_tick         Seq thread, once per 64-frame block after TE's player: a clock per playing DRUM
 *                 ROLL pattern (started and re-synced by the step events), its hits as drum
 *                 note-ons, every note-off when it is due, the preview hit; notes only in drum
 *                 playmode (TE asserts otherwise)
 * dr_te_note_on   drum sampler voice note_on slot: TE's, then the voice's post-process state (the
 *                 sequencer's gain + gate for its own hits, the sample's level for played keys)
 * dr_te_render    drum sampler voice render slot: TE's, then the sample's filter, gain, gate
 * dr_key          on_key: shift + green press on the finger screen (sequencer key, drum playmode,
 *                 finger picked) opens / closes the screen; while open blue press = step on / off
 *                 (any page), green press = next page, white press = hear the step, shift + press =
 *                 actions (all knob presses ours); the keyboard stays TE's
 * dr_knob         on_knob while open: the page's four functions; shift + blue lane, shift + green
 *                 finger pattern 1..14
 * dr_frame        the screen, or a small badge while a DRUM ROLL pattern plays with the screen closed
 * dr_main         main thread, every main-loop pass: loads the patterns + settings from TE's op1.db
 *                 once, then saves what changed ~3 s after the last edit (at once on close)
 * Page 6 CLEAR: a knob push asks "CLEAR PATTERN n?" (turn: no / yes, push: ok). */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "dr_core.h"
#include "dr_dsp.h"
#include "dr_store.h"
#include "dr_fs.h"
#include "dr_punch.h"

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

/* TE's finger sequencer ("ok", RE 2026-10-17; spec: README.md). Drum OkData: artist patterns, notes
 * [14][32][2] (53 + key, -1), lengths; loaded from table okseq on first use. Player: the selected
 * pattern (last started). set_note marks the data dirty (0x0193ef3c) for TE's saver. */
#define FS_DATA             ((void *)0x01dda710)
#define FS_LOADED           (*(volatile uint8_t *)0x01ddb564)
#define FS_ARTIST(a)        (*(volatile int32_t *)(0x01dda718 + 4 * (a)))
#define FS_NOTE(p, s, h)    (*(volatile int32_t *)(0x01dda720 + (p) * 0x100 + (s) * 8 + (h) * 4))
#define FS_LEN(p)           (*(volatile int32_t *)(0x01ddb520 + 4 * (p)))
#define FS_MARK_DIRTY       ((void (*)(void *))0x0193ef3c)
#define FS_SET_NOTE         ((void (*)(void *, int, int, int, int))0x019ecc5c)
#define FS_GET_NOTE         ((int (*)(void *, int, int, int))0x019eccf0)
#define FS_PLAYER_BUILT     (*(volatile int32_t *)0x01ddc5a4)
#define FS_SELECTED         (*(volatile int32_t *)0x01ddc5c4)   /* player 0x01ddc5a8 + 0x1c */
#define FS_STEP(a)          (*(volatile int32_t *)(0x01ddc5b4 + 4 * (a)))   /* player + 0x0c: artist's step */
#define KEY_PUSH            59                  /* blue 59 green 60 white 61 orange 62 */
#define OFFS                64
#define BLOCK               TE_AUDIO_FRAMES
#define BARRIER()           __asm__ volatile("" ::: "memory")

/* ------------------------------------------------------------------ state */

dr_pattern dr_pat[DR_PATTERNS];                 /* bench reads; pattern p = finger pattern p + 1 */
volatile uint8_t dr_cur;                        /* shown pattern 0..13 */
volatile uint8_t dr_open, dr_page;              /* bench reads; page 0..DR_PAGES-1 */
volatile uint16_t dr_owned;                     /* bit p: pattern p is DRUM ROLL's (else TE's) */
volatile uint8_t dr_sel, dr_cursor, dr_param, dr_zoom;  /* dr_param: the graph's step parameter */
volatile uint8_t dr_swing;
dr_fs_clock dr_play[DR_PATTERNS];               /* Seq thread: the playing DRUM ROLL patterns */
static volatile uint8_t inited;

typedef dr_filt filt;
static filt filters[DR_KEYS];
static dr_coef coefs[DR_KEYS];
static volatile int32_t key_gain[DR_KEYS];      /* played keys: the sample's lane level */

/* Seq thread -> voice note_on (same thread, synchronous) */
static volatile struct { int8_t key; uint8_t valid; int32_t gain, gate; } pend;
static dr_voice voices[DR_TE_VOICES];
static volatile uint8_t voice_on[DR_TE_VOICES];

/* punch-in effects (black keys on TE's drum finger screen): KeyHandler -> Seq thread -> voices */
volatile uint32_t dr_px_held;                   /* bit i: effect i held (bench reads) */
volatile uint8_t dr_px_bank;                    /* the black keys' bank 0..2 (A B C; green press) */
static volatile uint16_t px_consumed;           /* black keys whose release is ours */
static volatile uint32_t px_seq[DR_PX_KEYS], px_count;  /* press order: the last REPEAT wins */
static dr_px_ctl px_ctl;                        /* Seq thread writes, voice renders read */
static dr_px_voice px_voices[DR_TE_VOICES];

/* KeyHandler -> Seq thread */
static volatile uint8_t preview_req;
static volatile uint8_t consumed;               /* knob pushes whose key-up we eat (bits 0..3) */

/* Seq thread */
static struct { uint8_t note; uint32_t due; } offs[OFFS];
static int noffs;
static uint32_t now;                            /* samples since boot (note-off times) */
static uint32_t touch_at;                       /* `now` at the last knob / key event (view: cursor) */
#define TOUCH_HOLD  (2 * 44100)                 /* after it, the view follows the playhead */
static volatile uint16_t ev_mask;               /* step events of this block (get_note, hand 0) */
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
    return &dr_pat[dr_cur % DR_PATTERNS];
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

static int drum_ok(void)
{
    return *TE_PLAYMODE == 1 && *TE_PLAYMODE_TARGET == 1;
}

/* the finger sequencer is the drum sequencer */
static int finger(void)
{
    return *TE_DRUM_SEQ_TYPE == TE_SEQ_FINGER;
}

/* TE's finger screen: the sequencer screen in drum playmode with the finger sequencer picked */
static int finger_screen(void)
{
    return *TE_PLAYMODE == 1 && *TE_EDIT_MODE == TE_EDIT_MODE_SEQ && finger();
}

/* is the DRUM ROLL screen up (over the finger screen) */
static int shown(void)
{
    return dr_open && finger_screen();
}

static int owned(int p)
{
    return (dr_owned >> p) & 1;
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
    dr_px_start(&px_voices[vi]);
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
    dr_px_process(&px_voices[vi], out, TE_VOICE_SAMPLES, &px_ctl);
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

static void hit(const dr_lane *ln, const dr_step *s, uint32_t due, uint32_t hold, int full)
{
    int32_t gain = s->p[DR_VEL] * ln->level * DR_UNITY / 10000;

    release_key(ln->key);
    note(1, ln->key, s->p[DR_TONE], gain, full ? -1 : (int32_t)hold * 2);   /* voices run at 2x */
    add_off(ln->key, due + hold);
}

/* the hits of pattern time [src, src + n) go out at block offset out + (at - src) x od / sn (the
 * time effects' speed: HALF TIME 1 / 2, DOUBLE TIME 2 / 1); tone: the repeat effects' TONE, and
 * the hit effects on top (see px_hit) */
typedef struct { const dr_pattern *p; uint32_t src, out, sn, sd; int tone; } emit_ctx;
static uint8_t px_mask = 0xff;                  /* lanes the SOLO / NO KICK / CUT effects let through */

static void px_hit(const dr_pattern *pt, int lane, const dr_step *s, uint32_t due, uint32_t hold, int full, int tone);

static void emit(void *ctx, const dr_hit *h)
{
    const emit_ctx *e = ctx;

    if (!(px_mask >> h->lane & 1))
        return;
    px_hit(e->p, h->lane, h->s, now + e->out + (h->at - e->src) * e->sd / e->sn, h->hold * e->sd / e->sn,
           h->full, e->tone);
}

/* step lengths per pattern and lane, recomputed only when the tempo or a rate changes (64-bit
 * divides); st16 = one finger step (1/16) */
static uint64_t st_len[DR_PATTERNS][DR_LANES], st16;
static uint8_t st_rate[DR_PATTERNS][DR_LANES];
static uint32_t st_bits, st_bpm;

/* GUI: the Seq thread's cached length (stale for at most a block), or 120 BPM before any tick */
static uint64_t lane_step(int l)
{
    uint64_t L = st_len[dr_cur % DR_PATTERNS][l & (DR_LANES - 1)];

    return st_bpm && L ? L : dr_step_q16(12000, pat()->lane[l & (DR_LANES - 1)].rate);
}

static void tempo(void)
{
    uint32_t bits = *(volatile uint32_t *)TE_BPM;

    if (bits != st_bits || !st_bpm) {
        st_bits = bits;
        st_bpm = bpm_x100();
        st16 = dr_step_q16(st_bpm, DR_RATE_16);
        for (bits = 0; bits < DR_PATTERNS * DR_LANES; bits++)
            st_rate[bits / DR_LANES][bits % DR_LANES] = 0xff;
    }
}

static const uint64_t *steps(int p)
{
    const dr_pattern *pt = &dr_pat[p];
    int l;

    for (l = 0; l < DR_LANES; l++)
        if (st_rate[p][l] != pt->lane[l].rate) {
            st_rate[p][l] = pt->lane[l].rate;
            st_len[p][l] = dr_step_q16(st_bpm, st_rate[p][l]);
        }
    return st_len[p];
}

/* TE's drum finger player asks for a note (call site 0x019ede2a): hand 0 = one artist's step event.
 * DRUM ROLL's patterns: nothing for TE to play (DRUM ROLL plays them from the seq tick). */
static volatile uint16_t px_take;               /* TE's patterns DRUM ROLL plays while a punch-in
                                                 * effect is held (TE: nothing of them meanwhile) */

int dr_fs_get_note(void *data, int p, int step, int hand)
{
    if (inited && data == FS_DATA && (unsigned int)p < DR_PATTERNS) {
        if (hand == 0)
            ev_mask |= (uint16_t)(1 << p);
        if (owned(p) || (px_take >> p & 1))
            return -1;
    }
    return FS_GET_NOTE(data, p, step, hand);
}

static void offs_due(uint32_t to)
{
    int i;

    for (i = 0; i < noffs; ) {
        if ((int32_t)(to - offs[i].due) >= 0) {
            note(0, offs[i].note, 0, 0, 0);
            offs[i] = offs[--noffs];
        } else {
            i++;
        }
    }
}

/* ---- punch-in effects, Seq thread (three banks, see dr_punch.h) */
static int8_t rep_key[DR_PATTERNS] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static uint32_t rep_t0[DR_PATTERNS], gl_seed[DR_PATTERNS];
static uint32_t px_rng = 0x1234567u, px_gate_clk, fade_t0;
static int32_t lp_q8 = 127 << 8, hp_q8;         /* sweep cutoffs (dr_coef_set index, Q8) */
static int16_t lp_set = -1, hp_set = -1;        /* the index the coefficients were made for */
static int16_t fade_q = 256;                    /* FADE: velocity x / 256 */

/* ECHO and COIN hits to come (later blocks) */
#define LATER 32
static struct { const dr_pattern *p; uint8_t lane; dr_step s; uint32_t due, hold; } later[LATER];
static int nlater;

static uint32_t rnd(void)
{
    px_rng = px_rng * 1664525u + 1013904223u;
    return px_rng >> 8;
}

static int held(int fx)
{
    return dr_px_held >> fx & 1;
}

static void hit_at(const dr_pattern *pt, int lane, const dr_step *s, uint32_t due, uint32_t hold)
{
    if ((int32_t)(due - (now + BLOCK)) >= 0) {  /* a later block: queued */
        if (nlater < LATER) {
            later[nlater].p = pt;
            later[nlater].lane = (uint8_t)lane;
            later[nlater].s = *s;
            later[nlater].due = due;
            later[nlater].hold = hold;
            nlater++;
        }
        return;
    }
    hit(&pt->lane[lane], s, due, hold, 0);
}

static void later_due(void)
{
    int i;

    for (i = 0; i < nlater; )
        if ((int32_t)(later[i].due - (now + BLOCK)) < 0) {
            hit(&later[i].p->lane[later[i].lane], &later[i].s, later[i].due, later[i].hold, 0);
            later[i] = later[--nlater];
        } else {
            i++;
        }
}

static int clamp_tone(int t)
{
    return t < -24 ? -24 : t > 24 ? 24 : t;
}

/* one hit with the hit effects: TONE (+ the time effect's) + OCT UP / DOWN + RND PITCH, FADE on the
 * velocity; COIN: an octave-up grace hit a quarter step later; ECHO: two softer repeats 3 and 6
 * steps later */
static void px_hit(const dr_pattern *pt, int lane, const dr_step *s, uint32_t due, uint32_t hold, int full, int tone)
{
    uint32_t ls = (uint32_t)(st16 >> 16);
    dr_step t = *s;
    int v;

    tone += (held(DR_PX_OCTUP) ? 12 : 0) - (held(DR_PX_OCTDN) ? 12 : 0);
    if (held(DR_PX_RNDPITCH))
        tone += (int)(rnd() % 25) - 12;
    t.p[DR_TONE] = (int8_t)clamp_tone(t.p[DR_TONE] + tone);
    v = t.p[DR_VEL] * fade_q / 256;
    if (v < 1)
        return;
    t.p[DR_VEL] = (int8_t)v;
    hit(&pt->lane[lane], &t, due, hold, full);
    if (held(DR_PX_COIN) && ls) {
        dr_step c = t;

        c.p[DR_TONE] = (int8_t)clamp_tone(c.p[DR_TONE] + 12);
        hit_at(pt, lane, &c, due + ls / 4, ls / 4 ? ls / 4 : 1);
    }
    if (held(DR_PX_ECHO) && ls) {
        dr_step e = t;

        e.p[DR_VEL] = (int8_t)(v * 6 / 10 > 0 ? v * 6 / 10 : 1);
        hit_at(pt, lane, &e, due + 3 * ls, hold);
        e.p[DR_VEL] = (int8_t)(v * 3 / 10 > 0 ? v * 3 / 10 : 1);
        hit_at(pt, lane, &e, due + 6 * ls, hold);
    }
}

static void sched(int p, uint32_t src, uint32_t n, uint32_t out, uint32_t sn, uint32_t sd, int tone)
{
    emit_ctx e = { &dr_pat[p], src, out, sn, sd, tone };

    if (n)
        dr_schedule(&dr_pat[p], src, n, steps(p), held(DR_PX_SHUFFLE) ? 66 : dr_swing,
                    seed ^ (uint32_t)p * 0x9e3779b9u, emit, &e);
}

/* the time effects: what they do with the pattern's clock (the last one pressed plays) */
enum { TM_REPEAT, TM_GLITCH, TM_SPEED, TM_REVERSE, TM_BUILD };
static const struct { uint8_t fx, kind, num, den; int8_t tone; } tfx[] = {
    { DR_PX_RETRIG,   TM_REPEAT, 1, 1, 0 },     /* the step from the nearest 1/16 over and over */
    { DR_PX_RETRIGF,  TM_REPEAT, 1, 2, 0 },     /* half a step */
    { DR_PX_RISE,     TM_REPEAT, 1, 2, 1 },     /* ... a semitone up each time */
    { DR_PX_FALL,     TM_REPEAT, 1, 2, -1 },    /* ... down */
    { DR_PX_LASER,    TM_REPEAT, 1, 4, -2 },    /* a 1/64 roll, 2 semitones down each: pew */
    { DR_PX_TRIPLET,  TM_REPEAT, 2, 3, 0 },     /* 1/16T */
    { DR_PX_LOOPBEAT, TM_REPEAT, 4, 1, 0 },     /* the beat */
    { DR_PX_GLITCH,   TM_GLITCH, 1, 1, 0 },     /* 1/16 pieces from random places of the bar */
    { DR_PX_HALF,     TM_SPEED,  1, 2, 0 },     /* half speed */
    { DR_PX_DOUBLE,   TM_SPEED,  2, 1, 0 },     /* double speed */
    { DR_PX_REVERSE,  TM_REVERSE, 1, 1, 0 },    /* the bar's steps backwards */
    { DR_PX_BUILD,    TM_BUILD,  1, 1, 0 },     /* a roll: 1/8, 1/16, 1/32, 1/64 beat by beat */
};
#define TFX ((int)(sizeof(tfx) / sizeof(tfx[0])))

static int held_time(void)                      /* index into tfx, -1 none */
{
    int i, r = -1;

    for (i = 0; i < TFX; i++)
        if (held(tfx[i].fx) && (r < 0 || px_seq[tfx[i].fx] > px_seq[tfx[r].fx]))
            r = i;
    return r;
}

/* FILL: on every 1/16 of the window, now and then an extra hit of one of the pattern's drums,
 * sometimes doubled */
static void fill(int p, uint32_t from, uint32_t end)
{
    static dr_step fs;
    const dr_pattern *pt = &dr_pat[p];
    uint32_t ls = (uint32_t)(st16 >> 16), k;
    int used[DR_LANES], nu = 0, l, s, j;

    for (l = 0; l < DR_LANES; l++)
        for (s = 0; s < pt->lane[l].len; s++)
            if (pt->lane[l].s[s].on && !pt->lane[l].mute && (px_mask >> l & 1)) {
                used[nu++] = l;
                break;
            }
    if (!nu || !ls)
        return;
    for (k = (from + ls - 1) / ls; k * ls < end; k++) {
        if (rnd() % 100 >= 45u)
            continue;
        l = used[rnd() % (uint32_t)nu];
        for (j = 0; j < DR_PARAMS; j++)
            fs.p[j] = dr_pdef[j];
        fs.on = 1;
        fs.p[DR_VEL] = (int8_t)(40 + rnd() % 60);
        px_hit(pt, l, &fs, now + (k * ls - from), ls / 2, 0, 0);
        if (rnd() % 100 < 20)                   /* a quick double now and then */
            px_hit(pt, l, &fs, now + (k * ls - from) + ls / 2, ls / 4, 0, 0);
    }
}

/* a TE pattern taken over: TE's notes now as DRUM ROLL's lanes (Seq thread; its own scratch) */
static void take_import(int p)
{
    static dr_fs_notes tn;
    int st, h;

    for (st = 0; st < DR_FS_STEPS; st++)
        for (h = 0; h < DR_FS_HANDS; h++)
            tn[st][h] = FS_NOTE(p, st, h);
    dr_fs_import(tn, FS_LEN(p), &dr_pat[p]);
}

/* the repeat effects loop the last step with a hit (lanes let through), from 1/16 step k0 back
 * up to a bar: pressed on an empty step, the step before plays on */
static int probe_n;

static void probe(void *ctx, const dr_hit *h)
{
    (void)ctx;
    probe_n += px_mask >> h->lane & 1;
}

static uint32_t last_hit_step(int p, uint32_t k0)
{
    uint32_t ls = (uint32_t)(st16 >> 16), i;

    for (i = 0; i < 16 && i <= k0; i++) {
        probe_n = 0;
        dr_schedule(&dr_pat[p], (k0 - i) * ls, ls, steps(p), 0, seed ^ (uint32_t)p * 0x9e3779b9u, probe, 0);
        if (probe_n)
            return k0 - i;
    }
    return k0;
}

/* one playing pattern's window [from, end): the time effect (its place captured at the nearest 1/16
 * of the first one pressed, for the repeats the last step with a hit from there; another one
 * meanwhile changes how, not where), else as it is; FILL on top. On release the pattern goes on
 * where its clock is (it never stopped). */
static void play_window(int p, uint32_t from, uint32_t end)
{
    uint32_t ls = (uint32_t)(st16 >> 16), n = end - from;
    int t = held_time(), i, k;
    dr_px_seg sg[3];

    if (t >= 0 && ls) {
        if (rep_key[p] < 0) {
            uint32_t k0 = (from + ls / 2) / ls;

            if (tfx[t].kind == TM_REPEAT && tfx[t].num * 4 / tfx[t].den < 8)
                k0 = last_hit_step(p, k0);      /* not on an empty step (LOOP BEAT: the beat) */
            else if (tfx[t].kind == TM_BUILD)
                k0 = last_hit_step(p, k0);
            rep_t0[p] = k0 * ls;
            gl_seed[p] = rnd();
        }
        rep_key[p] = (int8_t)t;
        switch (tfx[t].kind) {
        case TM_GLITCH:
            k = dr_px_glitch(from, n, ls, 16 * ls, gl_seed[p], sg);
            for (i = 0; i < k; i++)
                sched(p, sg[i].src, sg[i].n, sg[i].out, 1, 1, 0);
            break;
        case TM_REVERSE:
            k = dr_px_reverse(from, n, rep_t0[p], ls, 16, sg);
            for (i = 0; i < k; i++)
                sched(p, sg[i].src, sg[i].n, sg[i].out, 1, 1, 0);
            break;
        case TM_SPEED: {                        /* source = t0 + (t - t0) x num / den */
            uint32_t t0 = rep_t0[p], off = (int32_t)(t0 - from) > 0 ? t0 - from : 0, a, b;

            if (off >= n) {
                sched(p, from, n, 0, 1, 1, 0);
                break;
            }
            sched(p, from, off, 0, 1, 1, 0);
            a = t0 + (from + off - t0) * tfx[t].num / tfx[t].den;
            b = t0 + (end - t0) * tfx[t].num / tfx[t].den;
            sched(p, a, b - a, off, tfx[t].num, tfx[t].den, 0);
            break;
        }
        default: {                              /* REPEAT / BUILD: a slice over and over */
            uint32_t len = ls * tfx[t].num / tfx[t].den;

            if (tfx[t].kind == TM_BUILD) {
                uint32_t beats = (int32_t)(from - rep_t0[p]) > 0 ? (from - rep_t0[p]) / (4 * ls) : 0;

                len = (2 * ls) >> (beats > 3 ? 3 : beats);
            }
            k = dr_px_repeat(from, n, rep_t0[p], len ? len : 1, sg);
            for (i = 0; i < k; i++) {
                uint32_t at = from + sg[i].out;
                int r = (int32_t)(at - rep_t0[p]) >= 0 ? (int)((at - rep_t0[p]) / (len ? len : 1)) : 0;

                sched(p, sg[i].src, sg[i].n, sg[i].out, 1, 1, clamp_tone(r * tfx[t].tone));
            }
        }
        }
    } else {
        rep_key[p] = -1;
        sched(p, from, n, 0, 1, 1, 0);
    }
    if (held(DR_PX_FILL))
        fill(p, from, end);
}

static void set_coef(dr_coef *dst, int type, int cut, int res)
{
    dr_coef c;

    dr_coef_set(&c, type, cut, res);
    dst->type = DR_F_OFF;                       /* the render never sees half an update */
    BARRIER();
    dst->a1 = c.a1; dst->a2 = c.a2; dst->a3 = c.a3; dst->kh = c.kh;
    BARRIER();
    dst->type = (uint8_t)type;
}

/* the effects' control for this block: LP SWEEP closes over a bar, HP SWEEP opens over a bar;
 * released they come back within ~0.1 s; BLINDS: the gate on the first playing pattern's 1/16 grid;
 * 8-BIT; the lane mask (SOLOs, NO KICK, CUT); FADE over a bar from its press */
static void px_update(void)
{
    uint32_t ls = (uint32_t)(st16 >> 16), bar = ls * 16, blk;
    int32_t down = bar ? (int32_t)(((127 - 24) << 8) * (uint64_t)BLOCK / bar) : 0;
    int p, cut, lp_on = held(DR_PX_LPSWEEP), hp_on = held(DR_PX_HPSWEEP);
    uint8_t solo;

    for (p = 0; p < DR_PATTERNS && !dr_play[p].on; p++)
        ;
    if (lp_on)
        lp_q8 = lp_q8 - down > (24 << 8) ? lp_q8 - down : 24 << 8;
    else
        lp_q8 = lp_q8 + (8 << 8) < (127 << 8) ? lp_q8 + (8 << 8) : 127 << 8;
    if (hp_on)
        hp_q8 = hp_q8 + down * 92 / 103 < (92 << 8) ? hp_q8 + down * 92 / 103 : 92 << 8;
    else
        hp_q8 = hp_q8 - (8 << 8) > 0 ? hp_q8 - (8 << 8) : 0;
    cut = lp_q8 >> 8;
    if (cut >= 127 && !lp_on) {
        px_ctl.lp.type = DR_F_OFF;
        lp_set = -1;
    } else if (cut != lp_set) {
        set_coef(&px_ctl.lp, DR_F_LP, cut, 70);
        lp_set = (int16_t)cut;
    }
    cut = hp_q8 >> 8;
    if (cut <= 0 && !hp_on) {
        px_ctl.hp.type = DR_F_OFF;
        hp_set = -1;
    } else if (cut != hp_set) {
        set_coef(&px_ctl.hp, DR_F_HP, cut, 50);
        hp_set = (int16_t)cut;
    }
    if (held(DR_PX_BLINDS) && ls) {
        blk = p < DR_PATTERNS ? dr_play[p].next - BLOCK : px_gate_clk;    /* the block's clock */
        px_ctl.gper = 2 * ls;                   /* voices run at 2x */
        px_ctl.gpos = 2 * (blk % ls);
        px_ctl.gate = 1;
        px_gate_clk += BLOCK;
    } else {
        px_ctl.gate = 0;
        px_gate_clk = 0;
    }
    px_ctl.crush = (uint8_t)held(DR_PX_BIT8);
    solo = (uint8_t)((held(DR_PX_SOLO1) ? 0x01 : 0) | (held(DR_PX_SOLO12) ? 0x03 : 0)
                     | (held(DR_PX_SOLO14) ? 0x0f : 0) | (held(DR_PX_SOLO58) ? 0xf0 : 0));
    px_mask = (uint8_t)((solo ? solo : 0xff) & (held(DR_PX_NOKICK) ? 0xfe : 0xff));
    if (held(DR_PX_CUT))
        px_mask = 0;
    if (held(DR_PX_FADE) && bar) {
        uint32_t el;

        if (fade_q == 256 && !fade_t0)
            fade_t0 = now ? now : 1;
        el = now - fade_t0;
        fade_q = (int16_t)(el >= bar ? 0 : 256 - (int32_t)((uint64_t)el * 256 / bar));
    } else {
        fade_q = 256;
        fade_t0 = 0;
    }
}

void dr_tick(void)
{
    int ok = drum_ok(), run = ok && finger(), p;
    uint16_t ev = ev_mask;
    uint32_t to = now + BLOCK;

    ev_mask = 0;
    if (!inited)
        return;
    if (ok)                                     /* due note-offs first: a new hit may follow */
        offs_due(to);
    tempo();
    if (ok)
        later_due();                            /* ECHO / COIN hits that are due */
    else
        nlater = 0;
    /* the patterns an artist holds: a clock each, started and kept in time by TE's step events;
     * DRUM ROLL's are played, TE's too while a punch-in effect on the notes is held (taken over:
     * TE's notes read at the press, TE plays nothing of them meanwhile; released: TE's again) */
    {
        uint32_t note_fx = dr_px_held & ~(1u << DR_PX_LPSWEEP | 1u << DR_PX_HPSWEEP | 1u << DR_PX_BLINDS
                                          | 1u << DR_PX_BIT8);
        uint16_t take = 0;

        for (p = 0; p < DR_PATTERNS; p++) {
            dr_fs_clock *c = &dr_play[p];
            uint32_t from, end;

            if (!run || !st16 || (FS_ARTIST(0) != p && FS_ARTIST(1) != p)) {
                c->on = 0;
                continue;
            }
            if (!owned(p) && note_fx && FS_LOADED) {
                take |= (uint16_t)(1 << p);
                if (!(px_take >> p & 1))
                    take_import(p);
            }
            if (dr_fs_window(c, (ev >> p) & 1, st16, BLOCK, &from, &end) && end > from) {
                if (owned(p) || (take >> p & 1))
                    play_window(p, from, end);
                else
                    rep_key[p] = -1;
            }
        }
        px_take = take;
    }
    px_update();
    if (ok && preview_req) {
        const dr_lane *ln = lane();
        const dr_step *s = &ln->s[dr_cursor % DR_STEPS];
        uint64_t L = st_bpm ? steps(dr_cur % DR_PATTERNS)[dr_sel & (DR_LANES - 1)] : dr_step_q16(12000, ln->rate);
        int li = s->p[DR_LEN];
        uint32_t hold = (uint32_t)((L * dr_len8[li < 0 || li >= DR_LENS ? DR_LEN_FULL : li] / 8) >> 16);

        preview_req = 0;
        hit(ln, s, now, hold ? hold : 1, li == DR_LEN_FULL);
    }
    if (ok)                                     /* note-offs wait for drum playmode (never dropped) */
        offs_due(to);
    now = to;
}

/* ------------------------------------------------------------------ KeyHandler thread */

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
    F_SAMPLE, F_LENGTH, F_RATE, F_MUTE, F_FILTER, F_CUTOFF, F_RESO, F_LEVEL, F_PLEN
};
#define DR_PAGES    6
#define PG_PAT      5
static const char *const page_name[DR_PAGES] = { "STEP", "NOTE", "TIME", "LANE", "SOUND", "PAT" };
static const uint8_t page_fn[DR_PAGES][4] = {
    { F_STEP, F_ON, F_VEL, F_PROB },            /* 1 STEP: which step, hit or not, how loud, how sure */
    { F_STEP, F_TONE, F_LEN, F_RATCH },         /* 2 NOTE: pitch, length, repeats */
    { F_STEP, F_NUDGE, F_SWING, F_ZOOM },       /* 3 TIME: early / late, swing (all), view */
    { F_SAMPLE, F_LENGTH, F_RATE, F_MUTE },     /* 4 LANE: which drum, its own beat */
    { F_FILTER, F_CUTOFF, F_RESO, F_LEVEL },    /* 5 SOUND: the drum's filter and level */
    { F_PLEN, F_PLEN, F_PLEN, F_PLEN },         /* 6 PAT: the pattern's length (blue only; push: clear) */
};
static const char *const fn_name[] = {
    "STEP", "ON", "VEL", "PROB", "TONE", "LEN", "RATCH", "NUDGE", "SWING", "ZOOM",
    "SAMPLE", "LENGTH", "RATE", "MUTE", "FILTER", "CUTOFF", "RESO", "LEVEL", "LENGTH",
};
static const int8_t fn_param[] = {             /* step parameter behind a function, -1 none */
    -1, -1, DR_VEL, DR_PROB, DR_TONE, DR_LEN, DR_RATCH, DR_NUDGE,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
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

/* ---- the finger patterns (KeyHandler thread: TE's finger screen writes its notes here too) */

static dr_fs_notes te_notes;                    /* one pattern of TE's notes (scratch) */
static int8_t last_sel = -1;                    /* TE's selected pattern when last looked at */

/* TE's pattern p as DRUM ROLL shows it (TE's own patterns only; nothing before TE loaded okseq) */
static void import(int p)
{
    int st, h;

    if (owned(p) || !FS_LOADED)
        return;
    for (st = 0; st < DR_FS_STEPS; st++)
        for (h = 0; h < DR_FS_HANDS; h++)
            te_notes[st][h] = FS_NOTE(p, st, h);
    dr_fs_import(te_notes, FS_LEN(p), &dr_pat[p]);
}

/* TE's copy of pattern p = notes (written where they differ; TE's saver keeps them in okseq) */
static void te_write(int p, const dr_fs_notes notes)
{
    int st, h, changed = 0;

    if (!FS_LOADED)
        return;
    for (st = 0; st < DR_FS_STEPS; st++)
        for (h = 0; h < DR_FS_HANDS; h++)
            if (FS_NOTE(p, st, h) != notes[st][h]) {
                FS_NOTE(p, st, h) = notes[st][h];
                changed = 1;
            }
    if (changed)
        FS_MARK_DIRTY(FS_DATA);
}

/* a DRUM ROLL pattern's summary in TE's copy (TE's compressed view, stock firmware) */
static void summary(int p)
{
    if (!owned(p))
        return;
    dr_fs_summary(&dr_pat[p], te_notes);
    te_write(p, te_notes);
}

/* TE's length of pattern p = DRUM ROLL's (1/16 steps, TE keeps at most 32; TE's saver keeps it) */
static void te_length(int p)
{
    int n = dr_fs_length(&dr_pat[p]);

    n = n > DR_FS_STEPS ? DR_FS_STEPS : n;
    if (FS_LOADED && FS_LEN(p) != n) {
        FS_LEN(p) = n;
        FS_MARK_DIRTY(FS_DATA);
    }
}

/* TE's length of a pattern changed (finger screen; main thread, polled): a DRUM ROLL pattern's lanes
 * all follow (dr_fs_set_length), a TE pattern on screen is imported again. Race-free with an edit on
 * the KeyHandler thread: that one sets the lanes before TE's length, and a length that already
 * matches is left alone. */
static int8_t te_len_seen[DR_PATTERNS];        /* 0: not looked at yet */

static void follow_te_length(void)
{
    int p, n, m;

    if (!FS_LOADED)
        return;
    for (p = 0; p < DR_PATTERNS; p++) {
        n = FS_LEN(p);
        if (n < 1 || n > DR_FS_STEPS || n == te_len_seen[p])
            continue;
        m = te_len_seen[p];
        te_len_seen[p] = (int8_t)n;
        if (!m)
            continue;                           /* first look (boot): TE's length as it is */
        if (owned(p)) {
            m = dr_fs_length(&dr_pat[p]);
            if ((m > DR_FS_STEPS ? DR_FS_STEPS : m) != n && dr_fs_set_length(&dr_pat[p], n)) {
                summary(p);                     /* steps kept past the old length may show again */
                dirty = 1;
            }
        } else if (p == (dr_cur % DR_PATTERNS)) {
            import(p);
        }
        if (p == (dr_cur % DR_PATTERNS))
            select_lane(dr_sel);
    }
}

static void show_pattern(int p)
{
    dr_cur = (uint8_t)p;
    import(p);
    select_lane(dr_sel);
    refresh_gains();
}

/* the view follows TE's selected pattern (when the finger sequencer starts another one) */
static void follow(void)
{
    int s = FS_PLAYER_BUILT ? FS_SELECTED : -1;

    if (s < 0 || s >= DR_PATTERNS || s == last_sel)
        return;
    last_sel = (int8_t)s;
    show_pattern(s);
}

/* TE's finger screen writes a note (call sites 0x019ec534 delete, 0x019ec802 / 0x019ec840 step
 * record, 0x019ecade clear): TE's write, then a DRUM ROLL pattern takes the edit, a TE pattern on
 * screen is imported again */
/* ---- undo / redo (KeyHandler thread): shift + BACK = undo, shift + FORWARD = redo, several steps,
 * in the editor and on TE's drum finger screen with it closed. A step is the state of one pattern
 * before a change: DRUM ROLL's pattern, TE's copy of it (notes + length) and whose it is, so TE's
 * own finger screen edits (step record, delete, clear: its set_note calls) go back too. Knob turns
 * of one kind, the STEP REC keys of one step and TE's edits in a row within a second are one step;
 * every press another (src >= 0x400: never merged). The history
 * swaps: undo puts the step's state back and keeps the state it replaced for redo. */
#define HIST        16
#define COALESCE    44100                       /* samples: the same kind of edit within 1 s */

typedef struct dr_snap {
    int8_t p;
    uint8_t owned, te_len, pad;
    dr_fs_notes te;
    dr_pattern pat;
} dr_snap;

static dr_snap hist[HIST], h_pend, swap_tmp;
static uint8_t h_start, h_len, h_pos;           /* ring: h_pos steps to undo, h_len - h_pos to redo */
static int8_t pend_p = -1, last_p = -1;
static uint32_t last_src, last_at;
volatile uint8_t dr_undo_n, dr_redo_n;          /* bench reads */
static const char *volatile msg;                /* "UNDO" / "REDO" / "NOTHING TO UNDO" a moment */
static volatile uint32_t msg_until;

static void snap_get(int p, dr_snap *sn)
{
    int st, h;

    sn->p = (int8_t)p;
    sn->owned = (uint8_t)owned(p);
    sn->pat = dr_pat[p];
    sn->te_len = (uint8_t)(FS_LOADED ? FS_LEN(p) : 16);
    for (st = 0; st < DR_FS_STEPS; st++)
        for (h = 0; h < DR_FS_HANDS; h++)
            sn->te[st][h] = FS_LOADED ? FS_NOTE(p, st, h) : -1;
}

static void snap_set(const dr_snap *sn)
{
    int p = sn->p;

    dr_pat[p] = sn->pat;
    dr_owned = (uint16_t)(sn->owned ? dr_owned | 1 << p : dr_owned & ~(1 << p));
    te_write(p, sn->te);
    if (FS_LOADED && FS_LEN(p) != sn->te_len) {
        FS_LEN(p) = sn->te_len;
        FS_MARK_DIRTY(FS_DATA);
    }
    dirty = 1;
}

static int snap_differs(const dr_snap *sn)
{
    int p = sn->p, st, h;

    if (sn->owned != owned(p) || dr_hash(&sn->pat, (int)sizeof(dr_pattern)) != dr_hash(&dr_pat[p], (int)sizeof(dr_pattern)))
        return 1;
    if (FS_LOADED && FS_LEN(p) != sn->te_len)
        return 1;
    for (st = 0; st < DR_FS_STEPS; st++)
        for (h = 0; h < DR_FS_HANDS; h++)
            if (FS_LOADED && FS_NOTE(p, st, h) != sn->te[st][h])
                return 1;
    return 0;
}

static void hist_counts(void)
{
    dr_undo_n = h_pos;
    dr_redo_n = (uint8_t)(h_len - h_pos);
}

static void hist_before(int p)                  /* before an event that may change pattern p */
{
    snap_get(p, &h_pend);
    pend_p = (int8_t)p;
}

static void hist_after(uint32_t src)            /* after it: a step if it changed something */
{
    int p = pend_p;

    pend_p = -1;
    if (p < 0 || !snap_differs(&h_pend))
        return;
    if (h_pos && src < 0x400 && src == last_src && p == last_p && now - last_at < COALESCE) {
        last_at = now;                          /* the same kind of edit going on: one step */
        h_len = h_pos;                          /* (what could be redone is gone) */
    } else {
        h_len = h_pos;
        if (h_len == HIST) {                    /* full: the oldest step goes */
            h_start = (uint8_t)((h_start + 1) % HIST);
            h_len--;
            h_pos--;
        }
        hist[(h_start + h_pos) % HIST] = h_pend;
        h_len = ++h_pos;
        last_src = src;
        last_p = (int8_t)p;
        last_at = now;
    }
    hist_counts();
}

static void hist_step(int redo)
{
    dr_snap *e;

    if (redo ? h_pos >= h_len : !h_pos) {
        msg = redo ? "NOTHING TO REDO" : "NOTHING TO UNDO";
        msg_until = now + COALESCE;
        return;
    }
    if (!redo)
        h_pos--;
    e = &hist[(h_start + h_pos) % HIST];
    snap_get(e->p, &swap_tmp);                  /* the state it replaces: kept for the way back */
    snap_set(e);
    *e = swap_tmp;
    if (redo)
        h_pos++;
    last_src = 0;                               /* the next edit is a step of its own */
    hist_counts();
    if (dr_open)
        show_pattern(e->p);
    msg = redo ? "REDO" : "UNDO";
    msg_until = now + COALESCE;
}

void dr_fs_set_note(void *data, int p, int step, int hand, int n)
{
    int32_t was;

    if (!inited || data != FS_DATA || (unsigned int)p >= DR_PATTERNS || (unsigned int)step >= DR_FS_STEPS
        || (unsigned int)hand >= DR_FS_HANDS) {
        FS_SET_NOTE(data, p, step, hand, n);
        return;
    }
    was = FS_NOTE(p, step, hand);
    hist_before(p);                             /* TE's edit: undoable */
    FS_SET_NOTE(data, p, step, hand, n);
    if (owned(p)) {                             /* TE's copy stays as TE wrote it (summary: next edit) */
        if (dr_fs_te_edit(&dr_pat[p], step, was, n, FS_LEN(p)))
            dirty = 1;
        if (n < 0 && FS_NOTE(p, step, 0) < 0 && FS_NOTE(p, step, 1) < 0 && dr_fs_te_clear_step(&dr_pat[p], step))
            dirty = 1;                          /* TE emptied the step: every lane's (delete, clear) */
    } else if (p == (dr_cur % DR_PATTERNS)) {
        import(p);
    }
    if (p == (dr_cur % DR_PATTERNS)) {
        select_lane(dr_sel);
        refresh_gains();
    }
    hist_after(0x300);                          /* TE's edits within a second: one step */
}

/* page 6: the pattern empty on both sides, back to TE */
static void clear_pattern(void)
{
    int p = dr_cur % DR_PATTERNS, st;

    dr_owned &= (uint16_t)~(1 << p);
    for (st = 0; st < DR_FS_STEPS; st++)
        te_notes[st][0] = te_notes[st][1] = -1;
    te_write(p, te_notes);
    dr_pattern_init(pat());                     /* as new: lanes on keys 1..8, empty */
    import(p);                                  /* ... with TE's length */
    if (undo_of >= 0 && undo_of / DR_LANES == p)
        undo_of = -1;
    dr_cursor = 0;
    select_lane(dr_sel);
}

/* zoom: index (kept in op1.db; 0 1 2 as before) -> half bars shown; the knob steps 1 2 3 3.5 4
 * bars (3.5: 56 steps, a 1/32 lane of 64 steps + room) */
#define ZOOMS 5
static const uint8_t zoom_half[ZOOMS] = { 2, 4, 8, 6, 7 };
static const uint8_t zoom_order[ZOOMS] = { 0, 1, 3, 4, 2 };    /* indices by size */

static int zoom_pos(int z)
{
    int i;

    for (i = 0; i < ZOOMS && zoom_order[i] != z; i++)
        ;
    return i < ZOOMS ? i : 0;
}

static int zoom_step(int z, int d)
{
    return zoom_order[clampi(zoom_pos(z) + d, 0, ZOOMS - 1)];
}

static int zoom_text(int x, int y, int z, uint16_t c, int scale)    /* "3.5" */
{
    int h = zoom_half[z % ZOOMS];

    x = gfx_dec(x, y, h / 2, c, scale);
    return h & 1 ? gfx_text(x, y, ".5", c, scale) : x;
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
    case F_ZOOM:   dr_zoom = (uint8_t)zoom_step(dr_zoom, d); break;
    case F_SAMPLE: l->key = (uint8_t)wrap(l->key + d, DR_KEYS); break;
    case F_LENGTH:
        l->len = (uint8_t)clampi(l->len + d, 1, DR_STEPS);
        if (dr_cursor >= l->len)
            dr_cursor = (uint8_t)(l->len - 1);
        break;
    case F_RATE:   l->rate = (uint8_t)clampi(l->rate + d, 0, DR_RATES - 1); break;
    case F_PLEN:                                /* every lane, then TE's length (in this order) */
        {
            int was = dr_fs_length(pat()), n = was + d;

            while (n > 1 && n < DR_STEPS && (dr_fs_set_length(pat(), n), dr_fs_length(pat()) == was))
                n += d;                         /* coarse lanes only (1/4, 1/8, T): to the next length */
            dr_fs_set_length(pat(), n);
        }
        te_length(dr_cur % DR_PATTERNS);
        dr_cursor = (uint8_t)(dr_cursor % l->len);
        break;
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
    /* not playing: hear the step while shaping it */
    if (!dr_play[dr_cur % DR_PATTERNS].on && (f == F_VEL || f == F_TONE || f == F_LEN || f == F_RATCH || (f == F_ON && d > 0)))
        preview_req = 1;
}

/* shift + press: blue (or green) close, orange clear the lane (again: undo); white nothing */
volatile uint8_t dr_rec;                        /* STEP REC mode (bench reads) */
static uint8_t rec_touched;                     /* the cursor step was played into since arriving */

static void action(int k)
{
    int at = (dr_cur % DR_PATTERNS) * DR_LANES + (dr_sel & (DR_LANES - 1));

    if (k == 0 || k == 1) {
        dr_open = 0;
        dr_rec = 0;
        save_now = 1;                           /* closing: keep it now, not ~3 s later */
    } else if (k == 2) {
        dr_rec = !dr_rec;                       /* shift + white: STEP REC on / off */
        rec_touched = 0;
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

/* after a key / knob event on pattern p (hash: p before it): an edit makes a TE pattern DRUM
 * ROLL's; a DRUM ROLL pattern's summary follows every edit */
static void after_event(int p, uint32_t before, int cleared)
{
    if (!cleared && dr_hash(&dr_pat[p], (int)sizeof(dr_pattern)) != before) {
        dr_owned |= (uint16_t)(1 << p);
        summary(p);
    }
    refresh_gains();
    dirty = 1;
}

/* the black keys, low to high (F# G# A# C# D# F# G# A# C# D#: key ids, RE 2026-10-17): punch-in
 * effects on TE's drum finger screen, where a plain black press does nothing in TE (its player
 * eats it): the 10 effects of the bank picked (green press on the finger screen, editor closed:
 * A -> B -> C); shift + black stays TE's step record */
static const uint8_t px_keys[10] = { 13, 14, 15, 32, 22, 23, 24, 25, 26, 27 };
static int8_t px_of[10] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };  /* the effect a key holds */

static int px_key(int key, int down)
{
    int i, fx;

    for (i = 0; i < 10 && px_keys[i] != key; i++)
        ;
    if (i == 10)
        return -1;
    if (down) {
        if (*TE_SHIFT_HELD || !finger_screen())
            return 0;
        fx = (dr_px_bank % DR_PX_BANKS) * DR_PX_BANK + i;
        px_seq[fx] = ++px_count;
        dr_px_held |= 1u << fx;
        px_of[i] = (int8_t)fx;
        px_consumed |= (uint16_t)(1 << i);
        return 1;
    }
    if (!(px_consumed >> i & 1))
        return 0;                               /* its press was TE's: so is the release */
    px_consumed &= (uint16_t)~(1 << i);
    if (px_of[i] >= 0)
        dr_px_held &= ~(1u << px_of[i]);
    px_of[i] = -1;
    return 1;
}

/* STEP REC (shift + white press while open): the keyboard plays drums into the cursor step (the
 * step index of every lane: the first key after arriving clears the step in every lane, more keys
 * add to it); a key goes to the first lane on its drum, else to the first empty lane (it gets the
 * drum), else nowhere. BACK / FORWARD go a step back / on (a step passed by stays as it is). The
 * 24 keys (ids by place, RE 2026-10-17) and the arrows are ours, press and release. */
static const uint8_t kb_ids[24] = {
    39, 13, 40, 14, 41, 15, 42, 28, 32, 29, 22, 30, 31, 23, 48, 24, 49, 25, 50, 51, 26, 52, 27, 53,
};
static uint32_t rec_consumed;                   /* bits 0..23 keys, 24 / 25 BACK / FORWARD */
static uint8_t undo_consumed;                   /* shift + BACK / FORWARD presses whose release is ours */

static int rec_key(int key, int down)
{
    dr_pattern *pt = pat();
    int i, l, s, p, bit;
    uint32_t before;

    if ((key == TE_KEY_BACK || key == TE_KEY_FORWARD) && (!down || !*TE_SHIFT_HELD)) {
        int d = key == TE_KEY_FORWARD ? 1 : -1, len = lane()->len < 1 ? 1 : lane()->len;

        bit = 24 + (d > 0);
        if (!down) {
            if (!(rec_consumed >> bit & 1))
                return 0;
            rec_consumed &= ~(1u << bit);
            return 1;
        }
        dr_cursor = (uint8_t)(((dr_cursor + d) % len + len) % len);
        rec_touched = 0;
        rec_consumed |= 1u << bit;
        touch_at = now;
        return 1;
    }
    for (i = 0; i < 24 && kb_ids[i] != key; i++)
        ;
    if (i == 24)
        return -1;
    if (!down) {
        if (!(rec_consumed >> i & 1))
            return 0;
        rec_consumed &= ~(1u << i);
        return 1;
    }
    p = dr_cur % DR_PATTERNS;
    before = dr_hash(&dr_pat[p], (int)sizeof(dr_pattern));
    hist_before(p);
    s = dr_cursor % DR_STEPS;
    if (!rec_touched) {                         /* re-recorded: the step off in every lane */
        for (l = 0; l < DR_LANES; l++)
            if (s < pt->lane[l].len)
                pt->lane[l].s[s].on = 0;
        rec_touched = 1;
    }
    for (l = 0; l < DR_LANES && pt->lane[l].key != i; l++)
        ;
    if (l == DR_LANES) {                        /* no lane on that drum: the first empty one */
        for (l = 0; l < DR_LANES; l++) {
            int k, any = 0;

            for (k = 0; k < pt->lane[l].len; k++)
                any |= pt->lane[l].s[k].on;
            if (!any)
                break;
        }
        if (l < DR_LANES)
            pt->lane[l].key = (uint8_t)i;
    }
    if (l < DR_LANES && s < pt->lane[l].len) {
        dr_step *st = &pt->lane[l].s[s];
        int j;

        for (j = 0; j < DR_PARAMS; j++)
            st->p[j] = dr_pdef[j];
        st->on = 1;
        dr_sel = (uint8_t)l;                    /* hear it (the cursor step of that lane) */
        preview_req = 1;
    }
    after_event(p, before, 0);
    hist_after(0x200 + (uint32_t)s);            /* the keys of one step: one step */
    rec_consumed |= 1u << i;
    touch_at = now;
    return 1;
}

int dr_key(int key, int down)
{
    int k = key - KEY_PUSH, p, cleared = 0;
    uint32_t before;

    if (!inited)
        return 0;
    if (key == TE_KEY_BACK || key == TE_KEY_FORWARD) {    /* shift + BACK / FORWARD: undo / redo */
        int bit = key == TE_KEY_FORWARD;

        if (down && *TE_SHIFT_HELD && finger_screen()) {
            hist_step(bit);
            undo_consumed |= (uint8_t)(1 << bit);
            return 1;
        }
        if (!down && (undo_consumed >> bit & 1)) {
            undo_consumed &= (uint8_t)~(1 << bit);
            return 1;
        }
    }
    if (dr_rec && (shown() || !down) && (p = rec_key(key, down)) >= 0)
        return p;                               /* STEP REC: the keyboard and the arrows */
    if ((p = px_key(key, down)) >= 0)
        return p;
    if (k < 0 || k > 3) {                       /* the keyboard stays TE's (pattern keys, step record) */
        if (!down && shown())
            follow();                           /* a pattern key let go: TE has started it by now */
        return 0;
    }
    if (!down) {
        if (consumed & (1 << k)) {
            consumed &= (uint8_t)~(1 << k);
            return 1;
        }
        return 0;
    }
    if (!finger_screen())
        return 0;
    if (!dr_open) {
        dr_ask = 0;
        if (k == 1 && !*TE_SHIFT_HELD) {        /* green press: the punch-in bank A -> B -> C */
            dr_px_bank = (uint8_t)((dr_px_bank + 1) % DR_PX_BANKS);
            msg = "FX BANK";
            msg_until = now + COALESCE;
            consumed |= 1 << k;
            return 1;
        }
        if (k != 0 || !*TE_SHIFT_HELD)
            return 0;                           /* requantize leaves shift + blue to TE in drum playmode */
        dr_open = 1;                            /* shift + blue press: open on TE's selected pattern */
        last_sel = -1;
        show_pattern(dr_cur % DR_PATTERNS);
        follow();
        for (p = 0; p < DR_PATTERNS; p++)
            summary(p);                         /* TE's copies up to date (after a restart too) */
        consumed |= 1 << k;
        return 1;
    }
    /* open: every knob press is ours */
    touch_at = now;
    follow();
    p = dr_cur % DR_PATTERNS;
    before = dr_hash(&dr_pat[p], (int)sizeof(dr_pattern));
    hist_before(p);
    if (dr_ask) {                               /* the dialog: any push = ok on the picked answer */
        if (dr_ask_yes) {
            clear_pattern();
            cleared = 1;
        }
        dr_ask = 0;
    } else if (*TE_SHIFT_HELD) {
        action(k);
    } else if (dr_page == PG_PAT && k != 1) {
        dr_ask = 1;                             /* blue / white / orange push: ask, NO picked */
        dr_ask_yes = 0;
    } else if (k == 0) {
        edit(F_ON, lane()->s[dr_cursor % DR_STEPS].on ? -1 : 1);   /* blue: step on / off */
    } else if (k == 1) {
        dr_page = (uint8_t)(dr_page + 1 < DR_PAGES ? dr_page + 1 : 0);     /* green: next page */
    } else if (k == 2) {
        preview_req = 1;                        /* white: hear the cursor step */
    }
    after_event(p, before, cleared);
    hist_after(0x400 + (uint32_t)k + (*TE_SHIFT_HELD ? 8u : 0u) + 16u * dr_page);
    consumed |= 1 << k;
    return 1;
}

int dr_knob(int knob, int16_t *diff)
{
    int d = *diff > 0 ? 1 : *diff < 0 ? -1 : 0, p;
    uint32_t before;

    if (!inited || !shown() || (unsigned int)knob > 3)
        return 0;
    if (!d)
        return 1;
    touch_at = now;
    if (dr_ask) {
        dr_ask_yes = d > 0;                     /* any knob: left NO, right YES */
        return 1;
    }
    follow();
    p = dr_cur % DR_PATTERNS;
    before = dr_hash(&dr_pat[p], (int)sizeof(dr_pattern));
    hist_before(p);
    if (*TE_SHIFT_HELD) {
        if (knob == 0)
            select_lane(dr_sel + d);            /* browse lanes */
        else if (knob == 1)
            show_pattern(clampi(dr_cur + d, 0, DR_PATTERNS - 1));      /* finger pattern 1..14 */
    } else if (dr_page != PG_PAT || knob == 0) {
        edit(page_fn[dr_page % DR_PAGES][knob], d);
    }
    after_event(p, before, 0);
    hist_after(0x100 + (uint32_t)knob + 4u * dr_page + (*TE_SHIFT_HELD ? 64u : 0u) + 128u * dr_sel);
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

/* the shown pattern plays as DRUM ROLL's (its clock: the Seq thread's, a block stale at most) */
static int playing(void)
{
    return dr_play[dr_cur % DR_PATTERNS].on;
}

static uint32_t play_clock(void)
{
    uint32_t t = dr_play[dr_cur % DR_PATTERNS].next;

    return t > BLOCK ? t - BLOCK : 0;
}

/* TE plays the shown TE pattern (an artist holds it) */
static int te_playing(void)
{
    int p = dr_cur % DR_PATTERNS;

    return !owned(p) && FS_LOADED && (FS_ARTIST(0) == p || FS_ARTIST(1) == p);
}

/* the playhead of lane l (step index), -1 if the shown pattern is not playing: DRUM ROLL's clock,
 * or for TE's own pattern (imported: 1/16 lanes) the step of the artist that holds it */
static int lane_playhead(const dr_lane *ln, uint64_t L)
{
    int p = dr_cur % DR_PATTERNS, a, st, len = ln->len < 1 ? 1 : ln->len;

    if (playing())
        return dr_playhead(ln, play_clock(), L);
    if (!te_playing())
        return -1;
    a = FS_ARTIST(0) == p ? 0 : 1;
    st = FS_STEP(a);
    return st < 0 || st >= DR_FS_STEPS ? -1 : st % len;
}

volatile int16_t dr_ph_shown = -1;              /* the selected lane's playhead as drawn (bench) */

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
    {
        int ph = lane_playhead(ln, L);

        if (l == dr_sel)
            dr_ph_shown = (int16_t)ph;
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
    gfx_rect(2, 1, 19, 11, col[3]);             /* finger pattern 1..14 */
    gfx_dec(dr_cur % DR_PATTERNS + 1 < 10 ? 8 : 4, 2, dr_cur % DR_PATTERNS + 1, GFX_BLACK, 2);
    x = gfx_text(25, 2, owned(dr_cur % DR_PATTERNS) ? "DR" : "TE", owned(dr_cur % DR_PATTERNS) ? col[0] : c_dim, 2);
    if (dr_rec) {                               /* STEP REC */
        gfx_rect(x + 6, 1, 30, 11, c_red);
        x = gfx_text(x + 9, 2, "REC", GFX_BLACK, 2) + 3;
    }
    if (playing() || te_playing())
        x = gfx_text(x + 8, 2, "PLAY", col[1], 2);
    else
        x = gfx_text(x + 8, 2, "STOP", c_dim, 2);
    if (playing() && beat) {
        uint32_t b = (uint32_t)(((uint64_t)play_clock() << 16) / beat);

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
    case F_ZOOM:   x = zoom_text(x, y, dr_zoom, c, scale); return gfx_text(x, y, " BAR", c, 1);
    case F_SAMPLE: return key_text(x, y, ln->key, c, scale);
    case F_LENGTH: return gfx_dec(x, y, ln->len, c, scale);
    case F_PLEN:   return gfx_dec(x, y, dr_fs_length(pat()), c, scale);
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
    case F_ZOOM:   return zoom_pos(dr_zoom) * 256 / (ZOOMS - 1);
    case F_SAMPLE: return ln->key * 256 / (DR_KEYS - 1);
    case F_LENGTH: return (ln->len - 1) * 256 / (DR_STEPS - 1);
    case F_PLEN:   return (dr_fs_length(pat()) - 1) * 256 / (DR_STEPS - 1);
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

/* page 6 PAT: the pattern's length on blue (one column), clearing on the presses */
static void draw_pat_page(void)
{
    int n = dr_fs_length(pat()), x = 2 + COL_W + 6, y = PANEL_Y + 2;

    x = gfx_text(2, PANEL_Y + 27, "X 1/16 =", c_dim, 1);   /* "16 X 1/16 = 1 BAR + 3" */
    x = gfx_dec(x + 4, PANEL_Y + 27, n / 16, GFX_WHITE, 1);
    x = gfx_text(x + 2, PANEL_Y + 27, n / 16 == 1 ? "BAR" : "BARS", c_dim, 1);
    if (n % 16) {
        x = gfx_text(x + 3, PANEL_Y + 27, "+", c_dim, 1);
        gfx_dec(x + 3, PANEL_Y + 27, n % 16, GFX_WHITE, 1);
    }
    x = 2 + COL_W + 6;
    gfx_rect(x - 4, y, 1, 38, c_beat);
    x = gfx_text(x, y, "CLEAR PATTERN", GFX_WHITE, 2);
    gfx_dec(x + 8, y, dr_cur % DR_PATTERNS + 1, col[3], 2);
    x = 2 + COL_W + 6;
    x = gfx_text(x, y + 14, "TURN", c_dim, 1) + 4;
    x = gfx_text(x, y + 14, "BLUE", col[0], 1) + 4;
    gfx_text(x, y + 14, "= LENGTH OF ALL 8 LANES + TE", c_dim, 1);
    x = gfx_text(2 + COL_W + 6, y + 22, "PUSH", c_dim, 1) + 4;
    x = gfx_text(x, y + 22, "BLUE", col[0], 1) + 4;
    x = gfx_text(x, y + 22, "WHITE", col[2], 1) + 4;
    x = gfx_text(x, y + 22, "ORANGE", col[3], 1) + 4;
    gfx_text(x, y + 22, "= CLEAR - ASKS FIRST", c_dim, 1);
    gfx_text(2 + COL_W + 6, y + 30, "CLEAR EMPTIES IT HERE AND IN TE", c_dim, 1);
}

/* "CLEAR PATTERN X?" over the lanes: turn picks NO / YES, push = ok */
#define ASK_W   184
#define ASK_H   62

static void draw_ask(void)
{
    int x0 = (gfx_w - ASK_W) / 2, y0 = LANE_Y + 10, i;

    gfx_rect(x0, y0, ASK_W, ASK_H, GFX_BLACK);
    frame_rect(x0, y0, ASK_W, ASK_H, GFX_WHITE);
    i = gfx_text(x0 + 12, y0 + 8, "CLEAR PATTERN", GFX_WHITE, 2);
    i = gfx_dec(i + 8, y0 + 8, dr_cur % DR_PATTERNS + 1, col[3], 2);
    gfx_text(i, y0 + 8, "?", col[3], 2);
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

    for (i = 0; i < (pg == PG_PAT ? 1 : 4); i++) {
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
    if (pg == PG_PAT) {
        draw_pat_page();
    } else if (pg <= 2) {
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
        gfx_text(2, PANEL_Y + 38, owned(dr_cur % DR_PATTERNS) ? "DRUM ROLL PLAYS THIS FINGER PATTERN: 8 LANES"
                                                            : "TE FINGER PATTERN: AN EDIT HERE MAKES IT A DRUM ROLL ONE", c_dim, 1);
    }
}

/* the held punch-in effects, "FX REPEAT 1/16 + LOW"; returns x */
static int px_text(int x, int y, int scale)
{
    int i, n = 0;

    for (i = 0; i < DR_PX_KEYS; i++)
        if (dr_px_held >> i & 1) {
            x = gfx_text(x, y, n ? " + " : "FX ", n ? c_dim : col[3], scale);
            n++;
            x = gfx_text(x, y, dr_px_name[i], col[3], scale);
        }
    return x;
}

static void draw_hints(void)
{
    int x = 2, y = gfx_h - 7;

    if (dr_px_held) {
        px_text(2, y, 1);
        return;
    }
    if (dr_rec) {
        x = gfx_text(x, y, "STEP REC: PLAY KEYS - BACK / FWD: STEP -", c_red, 1) + 4;
        x = gfx_shift(x, y, c_red, 1);
        gfx_text(x, y, "WHITE: END", c_red, 1);
        return;
    }
    x = gfx_text(x, y, "PUSH", c_dim, 1) + 4;
    x = gfx_text(x, y, dr_page == PG_PAT ? "CLEAR" : "ON", col[0], 1) + 4;
    x = gfx_text(x, y, "PAGE", col[1], 1) + 4;
    x = gfx_text(x, y, dr_page == PG_PAT ? "CLEAR" : "HEAR", col[2], 1) + 8;
    x = gfx_shift(x, y, c_dim, 1);
    x = gfx_text(x, y, "TURN", c_dim, 1) + 4;
    x = gfx_text(x, y, "LANE", col[0], 1) + 4;
    x = gfx_text(x, y, "PATTERN", col[1], 1) + 8;
    x = gfx_shift(x, y, c_dim, 1);
    x = gfx_text(x, y, "PUSH", c_dim, 1) + 4;
    x = gfx_text(x, y, "CLOSE", col[0], 1) + 4;
    gfx_text(x, y, "CLEAR", col[3], 1);
}

/* while punch-in effects are held: a window as CHORD+'s panel, the last pressed effect's
 * pictogram and name, the key it sits on (shift arrow + key), the other held ones below */
static void draw_px_window(void)
{
    static const char *const key_name[10] = { "F#", "G#", "A#", "C#", "D#", "F#", "G#", "A#", "C#", "D#" };
    static const uint16_t frame_col = GFX_RGB(0x35, 0x32, 0x38);
    int i, last = -1, x, y, x0 = 34, y0 = 22, w = 252, h = 108, r, c;
    char bank[2] = { 0, 0 };

    for (i = 0; i < DR_PX_KEYS; i++)
        if ((dr_px_held >> i & 1) && (last < 0 || px_seq[i] > px_seq[last]))
            last = i;
    if (last < 0)
        return;
    gfx_rect(x0, y0, w, h, frame_col);
    gfx_rect(x0 + 2, y0 + 2, w - 4, h - 4, GFX_BLACK);
    x = gfx_text(x0 + 10, y0 + 8, "PUNCH-IN", col[3], 2);
    x = gfx_text(x + 10, y0 + 12, "BANK", c_dim, 1) + 4;
    bank[0] = (char)('A' + last / DR_PX_BANK);
    x = gfx_text(x, y0 + 12, bank, col[1], 1) + 6;
    x = gfx_text(x, y0 + 12, "KEY", c_dim, 1) + 4;
    gfx_text(x, y0 + 12, key_name[last % DR_PX_BANK], GFX_WHITE, 1);
    for (r = 0; r < 8; r++)                     /* the pictogram, 8 x 8 cells of 6 px */
        for (c = 0; c < 8; c++)
            if (dr_px_icon[last][r] & (0x80 >> c))
                gfx_rect(x0 + 12 + c * 6, y0 + 30 + r * 6, 5, 5, col[3]);
    gfx_text(x0 + 74, y0 + 40, dr_px_name[last], GFX_WHITE, 3);
    for (i = 0, x = x0 + 74, y = y0 + 64; i < DR_PX_KEYS; i++)
        if (i != last && (dr_px_held >> i & 1)) {
            x = gfx_text(x, y, "+ ", c_dim, 1);
            x = gfx_text(x, y, dr_px_name[i], col[1], 1) + 6;
        }
    x = gfx_text(x0 + 10, y0 + h - 12, "HOLD = ON   EDITOR CLOSED:", c_dim, 1) + 4;
    x = gfx_text(x, y0 + h - 12, "GREEN", col[1], 1) + 4;
    gfx_text(x, y0 + h - 12, "= NEXT BANK", c_dim, 1);
}

/* "UNDO  3 LEFT" for a second after shift + BACK / FORWARD */
static void draw_msg(void)
{
    int x;

    if (!msg || (int32_t)(now - msg_until) >= 0)
        return;
    gfx_rect(96, 40, 128, 26, GFX_BLACK);
    frame_rect(96, 40, 128, 26, col[3]);
    if (msg[0] == 'F') {                        /* "FX BANK B" */
        char b[2] = { (char)('A' + dr_px_bank % DR_PX_BANKS), 0 };

        x = gfx_text(108, 49, msg, GFX_WHITE, 2);
        gfx_text(x + 8, 47, b, col[1], 3);
        return;
    }
    x = gfx_shift(104, 49, col[3], 2) + 4;
    x = gfx_text(x, 49, msg, GFX_WHITE, msg[0] == 'N' ? 1 : 2);
    if (msg[0] != 'N') {
        x = gfx_dec(x + 8, 52, msg[0] == 'U' ? dr_undo_n : dr_redo_n, col[3], 1);
        gfx_text(x + 3, 52, "LEFT", c_dim, 1);
    }
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
        if (finger_screen()) {                  /* TE's finger screen: how to open the editor, at the */
            int x;                              /* size of the engines' "SHIFT+BLUE: PRESETS" */

            char b[2] = { (char)('A' + dr_px_bank % DR_PX_BANKS), 0 };

            int y = gfx_h - 16;                 /* the lower left corner, clear of the case edge */

            gfx_rect(2, y - 2, 4 + 12 + 2 + 12 * 8 + 48, 14, GFX_BLACK);
            x = gfx_shift(4, y, c_dim, 2) + 2;  /* the shift arrow */
            x = gfx_text(x, y, "BLUE", col[0], 2);
            x = gfx_text(x, y, ": EDITOR", c_dim, 2);
            x = gfx_text(x + 12, y, "FX", c_dim, 2);           /* the punch-in bank */
            gfx_text(x + 4, y, b, col[1], 2);   /* green: the knob that changes it */
            draw_px_window();
            draw_msg();
        }
        return;
    }
    bpm = st_bpm;
    beat = dr_step_q16(bpm ? bpm : 12000, 0);
    L = lane_step(dr_sel);
    vlen = (uint32_t)((beat * 2 * zoom_half[dr_zoom % ZOOMS]) >> 16);   /* 1 / 2 / 3 / 3.5 / 4 bars */
    {
        int ph = lane_playhead(lane(), L);      /* playing and not touched lately: the playhead's bar */
        uint32_t at = ph >= 0 && now - touch_at > TOUCH_HOLD ? (uint32_t)ph : dr_cursor;

        v0 = vlen ? (uint32_t)(((uint64_t)at * L) >> 16) / vlen * vlen : 0;
    }
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
    draw_px_window();
    draw_msg();
}

/* ------------------------------------------------------------------ main thread: op1.db
 * Rows in TE's own database, table sdk_drumroll (id 0..13 patterns 1-14, 14 settings), the way TE keeps
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

static void put_id(char **o, int id)            /* 0..99 */
{
    if (id >= 10)
        *(*o)++ = (char)('0' + id / 10);
    *(*o)++ = (char)('0' + id % 10);
}

/* row id's data -> row (0-ended); 0 if there is none (or it cannot be read) */
static int db_read(void *db, int id)
{
    te_db_query q = { db, 0 };
    char *o = sql;
    int rc = 0, n = 0, i;
    void *sem = *TE_DB_LOCK;

    put(&o, "SELECT data FROM sdk_drumroll WHERE id=");
    put_id(&o, id);
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

            while (t && t[n] && n < (int)DR_ROW_TEXT_MAX - 1) {
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
    put_id(&o, id);
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
    s->owned[0] = (uint8_t)dr_owned;
    s->owned[1] = (uint8_t)(dr_owned >> 8);
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
        dr_owned = (uint16_t)(s.owned[0] | s.owned[1] << 8);
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

        if (h == saved_hash[id] || (id < DR_PATTERNS && !owned(id)))
            continue;                           /* TE's patterns are TE's to keep (okseq) */
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
    follow_te_length();
    if (shown())
        follow();                               /* a held pattern key: TE started it, the view goes there */
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
