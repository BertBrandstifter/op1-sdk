/* NOTE ROLL (spec: README.md): the synth finger sequencer's step editor, DRUM ROLL's twin for the
 * synth. A finger pattern edited here plays with chords (up to 6 notes a step), per note VEL and
 * LEN (ties over steps), per step NUDGE, PROB, RATCH, a length up to 64 steps.
 *
 * nr_init         defaults (14 empty patterns)
 * nr_fs_get_note  call site in TE's synth finger player (one call per artist step and hand): records
 *                 the step events, returns -1 for NOTE ROLL's patterns (TE plays nothing of them)
 * nr_tick         Seq thread, once per 64-frame block after TE's player: a clock per playing NOTE
 *                 ROLL pattern (started and re-synced by the step events), its notes as synth
 *                 note-ons, every note-off when due, the preview; notes only in synth playmode
 * nr_key          on_key: shift + blue press on the finger screen (sequencer key, synth playmode,
 *                 finger picked) opens / closes the screen; while open blue press = note on / off,
 *                 green press = next page, white press = hear the step (all knob presses ours)
 * nr_knob         on_knob while open: the page's four functions; shift + blue voice, shift + green
 *                 finger pattern 1..14
 * nr_frame        the screen; on the finger screen while closed: the "(shift arrow) BLUE: EDITOR" note
 * nr_main         main thread: TE's copy of the patterns (its edits in, NOTE ROLL's summaries out,
 *                 lengths both ways), op1.db (load once, save ~3 s after the last edit) */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "nr_core.h"
#include "nr_store.h"
#include "nr_fs.h"
#include "nr_punch.h"

#define KEY_PUSH            59                  /* blue 59 green 60 white 61 orange 62 */
#define OFFS                96
#define BLOCK               TE_AUDIO_FRAMES
#define BARRIER()           __asm__ volatile("" ::: "memory")

/* TE's synth finger sequencer (RE 2026-10-17, R246; README "TE's finger sequencer"): the synth
 * OkData 0x01dd98bc (drum: 0x01dda710, same layout), the shared player 0x01ddc5a8 */
#define FS_DATA             ((void *)0x01dd98bc)
#define FS_LOADED           (*(volatile uint8_t *)0x01ddb564)
#define FS_ARTIST(a)        (*(volatile int32_t *)(0x01dd98c4 + 4 * (a)))
#define FS_NOTE(p, s, h)    (*(volatile int32_t *)(0x01dd98cc + (p) * 0x100 + (s) * 8 + (h) * 4))
#define FS_LEN(p)           (*(volatile int32_t *)(0x01dda6cc + 4 * (p)))
#define FS_MARK_DIRTY       ((void (*)(void *))0x0193ef3c)
#define FS_GET_NOTE         ((int (*)(void *, int, int, int))0x019eccf0)
#define FS_PLAYER_BUILT     (*(volatile int32_t *)0x01ddc5a4)
#define FS_SELECTED         (*(volatile int32_t *)0x01ddc5c4)   /* player + 0x1c (both playmodes) */
#define FS_STEP(a)          (*(volatile int32_t *)(0x01ddc5b4 + 4 * (a)))

/* ------------------------------------------------------------------ state */

nr_pattern nr_pat[NR_PATTERNS];                 /* bench reads; pattern p = finger pattern p + 1 */
volatile uint8_t nr_cur, nr_open, nr_page;      /* shown pattern 0..13, page 0..NR_PAGES-1 */
volatile uint16_t nr_owned;                     /* bit p: pattern p is NOTE ROLL's (else TE's) */
volatile uint8_t nr_cursor, nr_voice, nr_zoom, nr_swing;   /* cursor step, cursor voice (the note at
                                                 * nr_cpitch, or the step's free slot) */
volatile uint8_t nr_cpitch = 60;                /* the cursor's pitch row (a stored note) */
volatile uint8_t nr_rec;                        /* STEP REC mode (bench reads) */
static volatile int16_t preview_note = -1;      /* KeyHandler -> Seq thread: one note to hear */
#define FS_OCTAVE           (*(volatile int32_t *)0x01ddc59c)   /* TE's finger screen octave -4..4 */
volatile uint8_t nr_ask, nr_ask_yes;            /* "CLEAR PATTERN X?" up / YES picked */
volatile int16_t nr_ph_shown = -1;              /* the playhead as drawn (bench) */
nr_fs_clock nr_play[NR_PATTERNS];               /* Seq thread: the playing NOTE ROLL patterns */
static volatile uint8_t inited;

/* KeyHandler -> Seq thread / main thread */
static volatile uint8_t preview_req, consumed, dirty, save_now;
static volatile uint16_t want_summary;          /* bit p: TE's copy of p to rewrite (main thread) */

/* Seq thread */
static struct { uint8_t note; uint32_t due; } offs[OFFS];
static int noffs;
static uint32_t now, touch_at;
#define TOUCH_HOLD  (2 * 44100)                 /* after a knob / key event the view shows the cursor */
static volatile uint16_t ev_mask;               /* step events of this block (get_note, hand 0) */
static uint32_t seed = 0x6d2b79f5u;

/* punch-in effects (black keys on TE's synth finger screen): KeyHandler -> Seq thread */
volatile uint32_t nr_px_held;                   /* bit i: effect i held (bench reads) */
volatile uint8_t nr_px_bank;                    /* the black keys' bank 0..2 (A B C; green press) */
static volatile uint16_t px_consumed;           /* black keys whose release is ours */
static volatile uint32_t px_seq[NR_PX_KEYS], px_count;  /* press order: the last REPEAT wins */
#define LATER       32
static struct { uint8_t key, vel; uint32_t due, hold; } later[LATER];  /* ARP's note-ons to come */
static int nlater;

void nr_init(void)
{
    int i;

    for (i = 0; i < NR_PATTERNS; i++)
        nr_pattern_init(&nr_pat[i]);
    BARRIER();
    inited = 1;
}

static nr_pattern *pat(void)
{
    return &nr_pat[nr_cur % NR_PATTERNS];
}

static nr_step *cstep(void)
{
    return &pat()->s[nr_cursor % NR_STEPS];
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int owned(int p)
{
    return (nr_owned >> p) & 1;
}

static int synth_ok(void)
{
    return *TE_PLAYMODE == 0 && *TE_PLAYMODE_TARGET == 0;
}

static int finger(void)
{
    return *TE_SYNTH_SEQ_TYPE == TE_SEQ_FINGER;
}

static int finger_screen(void)
{
    return *TE_PLAYMODE == 0 && *TE_EDIT_MODE == TE_EDIT_MODE_SEQ && finger();
}

static int shown(void)
{
    return nr_open && finger_screen();
}

static uint32_t bpm_x100(void)
{
    float b = *TE_BPM;

    return b >= 40.0f && b <= 220.0f ? (uint32_t)(b * 100.0f + 0.5f) : 0;
}

/* ------------------------------------------------------------------ Seq thread */

/* a stored note as TE's finger player plays it: + 12 x the synth sound's octave, MIDI out echo */
static void note(int on, int played, int vel)
{
    te_note n;

    n.note = played;
    n.vel = on ? (vel * 127 + 50) / 100 : 0;
    n.midi_out = 1;
    n.pad[0] = n.pad[1] = n.pad[2] = 0;
    n.extra = 0;
    if (on)
        TE_VOICE_NOTE_ON(&n, 0);
    else
        TE_VOICE_NOTE_OFF(&n);
}

static int played_note(int key)
{
    int k = key + 12 * TE_SYNTH_OCTAVE[*TE_SYNTH_SLOT & 7];

    return k < 0 || k > 127 ? -1 : k;
}

static void add_off(int played, uint32_t due)
{
    if (noffs < OFFS) {
        offs[noffs].note = (uint8_t)played;
        offs[noffs].due = due;
        noffs++;
    } else {
        note(0, played, 0);                     /* never lose a note-off */
    }
}

/* a held copy of the same note is released first (TE does the same before a retrigger) */
static void release(int played)
{
    int i;

    for (i = 0; i < noffs; )
        if (offs[i].note == played) {
            note(0, played, 0);
            offs[i] = offs[--noffs];
        } else {
            i++;
        }
}

/* a stored note at due (this block or later: then queued) */
static void hit_key(int key, int vel, uint32_t due, uint32_t hold)
{
    int k;

    if ((int32_t)(due - (now + BLOCK)) >= 0) {
        if (nlater < LATER) {
            later[nlater].key = (uint8_t)key;
            later[nlater].vel = (uint8_t)vel;
            later[nlater].due = due;
            later[nlater].hold = hold;
            nlater++;
        }
        return;
    }
    if ((k = played_note(key)) < 0)
        return;
    release(k);
    note(1, k, vel);
    add_off(k, due + hold);
}

static void hit(const nr_note *n, uint32_t due, uint32_t hold)
{
    hit_key(n->key, n->vel, due, hold);
}

static void later_due(void)
{
    int i;

    for (i = 0; i < nlater; )
        if ((int32_t)(later[i].due - (now + BLOCK)) < 0) {
            uint32_t due = later[i].due;
            int key = later[i].key, vel = later[i].vel;
            uint32_t hold = later[i].hold;

            later[i] = later[--nlater];
            hit_key(key, vel, due, hold);
        } else {
            i++;
        }
}

static uint32_t px_rng = 0x2468ace1u;

static uint32_t rnd(void)
{
    px_rng = px_rng * 1664525u + 1013904223u;
    return px_rng >> 8;
}

/* the hits of pattern time [src, src + n) at block offset out + (at - src) x sd / sn (the time
 * effects' speed), the time effect's semitones on top, each note through the held effects
 * (nr_px_notes: SOLO / FADE may leave it out, the arps / TRILL / BLINDS / COIN / ECHO add notes) */
typedef struct { const nr_pattern *p; uint32_t src, out, sn, sd; int tone; } emit_ctx;

static uint64_t st16;
static int16_t fade_q = 256;                    /* FADE: velocity x / 256 */

static void emit(void *ctx, const nr_hit *h)
{
    const emit_ctx *e = ctx;
    const nr_step *st = &e->p->s[h->step];
    uint32_t span = (uint32_t)(st16 >> 16) * e->sd / e->sn / (st->ratch < 1 ? 1 : st->ratch);   /* a hit */
    uint32_t at = now + e->out + (h->at - e->src) * e->sd / e->sn;
    nr_px_pulse o[16];
    int key = h->n->key + e->tone, k, n;

    key = key < 0 ? 0 : key > 127 ? 127 : key;
    n = nr_px_notes(nr_px_held, key, h->n->vel, h->voice, nr_voices(st), span, h->hold * e->sd / e->sn, rnd(),
                    fade_q, o, 16);
    for (k = 0; k < n; k++)
        hit_key(o[k].key, o[k].vel, at + o[k].delay, o[k].hold);
}

static uint32_t st_bits;

static void tempo(void)
{
    uint32_t bits = *(volatile uint32_t *)TE_BPM;

    if (bits != st_bits || !st16) {
        st_bits = bits;
        st16 = nr_step_q16(bpm_x100());
    }
}

/* TE's synth finger player asks for a note (call site 0x019edc4e): hand 0 = one artist's step
 * event. NOTE ROLL's patterns: nothing for TE to play (NOTE ROLL plays them from the seq tick). */
static volatile uint16_t px_take;               /* TE's patterns NOTE ROLL plays while a punch-in
                                                 * effect is held (TE: nothing of them meanwhile) */

int nr_fs_get_note(void *data, int p, int step, int hand)
{
    if (inited && data == FS_DATA && (unsigned int)p < NR_PATTERNS) {
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

    for (i = 0; i < noffs; )
        if ((int32_t)(to - offs[i].due) >= 0) {
            note(0, offs[i].note, 0);
            offs[i] = offs[--noffs];
        } else {
            i++;
        }
}

/* ---- punch-in effects, Seq thread (three banks, see nr_punch.h; as DRUM ROLL's) */
static int8_t rep_key[NR_PATTERNS] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static uint32_t rep_t0[NR_PATTERNS], gl_seed[NR_PATTERNS], fade_t0;

static int held(int fx)
{
    return nr_px_held >> fx & 1;
}

static void sched(int p, uint32_t src, uint32_t n, uint32_t out, uint32_t sn, uint32_t sd, int tone)
{
    emit_ctx e = { &nr_pat[p], src, out, sn, sd, tone };

    if (n)
        nr_schedule(&nr_pat[p], src, n, st16, nr_swing, seed ^ (uint32_t)p * 0x9e3779b9u, emit, &e);
}

/* the time effects: what they do with the pattern's clock (the last one pressed plays) */
enum { TM_REPEAT, TM_GLITCH, TM_SPEED, TM_REVERSE, TM_BUILD };
static const struct { uint8_t fx, kind, num, den; int8_t tone; } tfx[] = {
    { NR_PX_RETRIG,   TM_REPEAT, 1, 1, 0 },     /* the step from the nearest 1/16 over and over */
    { NR_PX_RETRIGF,  TM_REPEAT, 1, 2, 0 },     /* half a step */
    { NR_PX_POWERUP,  TM_REPEAT, 1, 2, 1 },     /* ... a semitone up each time */
    { NR_PX_POWERDN,  TM_REPEAT, 1, 2, -1 },    /* ... down */
    { NR_PX_LASER,    TM_REPEAT, 1, 4, -2 },    /* a 1/64 roll, 2 semitones down each */
    { NR_PX_TRIPLET,  TM_REPEAT, 2, 3, 0 },     /* 1/16T */
    { NR_PX_LOOPBEAT, TM_REPEAT, 4, 1, 0 },     /* the beat */
    { NR_PX_GLITCH,   TM_GLITCH, 1, 1, 0 },     /* 1/16 pieces from random places of the bar */
    { NR_PX_HALF,     TM_SPEED,  1, 2, 0 },     /* half speed */
    { NR_PX_DOUBLE,   TM_SPEED,  2, 1, 0 },     /* double speed */
    { NR_PX_REVERSE,  TM_REVERSE, 1, 1, 0 },    /* the bar's steps backwards */
    { NR_PX_BUILD,    TM_BUILD,  1, 1, 0 },     /* a roll: 1/8, 1/16, 1/32, 1/64 beat by beat */
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

/* FILL: on every 1/16 now and then an extra note, one of the pattern's */
static void fill(int p, uint32_t from, uint32_t end)
{
    const nr_pattern *pt = &nr_pat[p];
    uint32_t ls = (uint32_t)(st16 >> 16), k;
    uint8_t keys[24];
    int nk = 0, st, v, i;

    for (st = 0; st < pt->len && nk < 24; st++)
        for (v = 0; v < NR_VOICES && pt->s[st].n[v].key != NR_NONE && nk < 24; v++) {
            for (i = 0; i < nk && keys[i] != pt->s[st].n[v].key; i++)
                ;
            if (i == nk)
                keys[nk++] = pt->s[st].n[v].key;
        }
    if (!nk || !ls)
        return;
    for (k = (from + ls - 1) / ls; k * ls < end; k++)
        if (rnd() % 100 < 40u)
            hit_key(keys[rnd() % (uint32_t)nk], (40 + (int)(rnd() % 50)) * fade_q / 256 + 1, now + (k * ls - from), ls / 2);
}

static int clamp_tone(int t)
{
    return t < -24 ? -24 : t > 24 ? 24 : t;
}

/* a TE pattern taken over: TE's notes now as NOTE ROLL's (Seq thread; its own scratch) */
static void take_import(int p)
{
    static nr_fs_notes tn;
    int st, h;

    for (st = 0; st < NR_FS_STEPS; st++)
        for (h = 0; h < NR_FS_HANDS; h++)
            tn[st][h] = FS_NOTE(p, st, h);
    nr_fs_import(tn, FS_LEN(p), &nr_pat[p]);
}

/* the repeat effects loop the last step with a note, from 1/16 step k0 back up to a bar: pressed on
 * an empty step, the step before plays on */
static int probe_n;

static void probe(void *ctx, const nr_hit *h)
{
    (void)ctx;
    (void)h;
    probe_n++;
}

static uint32_t last_hit_step(int p, uint32_t k0)
{
    uint32_t ls = (uint32_t)(st16 >> 16), i;

    for (i = 0; i < 16 && i <= k0; i++) {
        probe_n = 0;
        nr_schedule(&nr_pat[p], (k0 - i) * ls, ls, st16, 0, seed ^ (uint32_t)p * 0x9e3779b9u, probe, 0);
        if (probe_n)
            return k0 - i;
    }
    return k0;
}

/* one playing pattern's window [from, end): the time effect (its place captured at the nearest 1/16
 * of the first one pressed, for the repeats the last step with a note from there), else as it is;
 * FILL on top; the clock never stops */
static void play_window(int p, uint32_t from, uint32_t end)
{
    uint32_t ls = (uint32_t)(st16 >> 16), n = end - from;
    int t = held_time(), i, k;
    nr_px_seg sg[3];

    if (t >= 0 && ls) {
        if (rep_key[p] < 0) {
            uint32_t k0 = (from + ls / 2) / ls;

            if ((tfx[t].kind == TM_REPEAT && tfx[t].num * 4 / tfx[t].den < 8) || tfx[t].kind == TM_BUILD)
                k0 = last_hit_step(p, k0);      /* not on an empty step (LOOP BEAT: the beat) */
            rep_t0[p] = k0 * ls;
            gl_seed[p] = rnd();
        }
        rep_key[p] = (int8_t)t;
        switch (tfx[t].kind) {
        case TM_GLITCH:
            k = nr_px_glitch(from, n, ls, 16 * ls, gl_seed[p], sg);
            for (i = 0; i < k; i++)
                sched(p, sg[i].src, sg[i].n, sg[i].out, 1, 1, 0);
            break;
        case TM_REVERSE:
            k = nr_px_reverse(from, n, rep_t0[p], ls, 16, sg);
            for (i = 0; i < k; i++)
                sched(p, sg[i].src, sg[i].n, sg[i].out, 1, 1, 0);
            break;
        case TM_SPEED: {
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
        default: {
            uint32_t len = ls * tfx[t].num / tfx[t].den;

            if (tfx[t].kind == TM_BUILD) {
                uint32_t beats = (int32_t)(from - rep_t0[p]) > 0 ? (from - rep_t0[p]) / (4 * ls) : 0;

                len = (2 * ls) >> (beats > 3 ? 3 : beats);
            }
            k = nr_px_repeat(from, n, rep_t0[p], len ? len : 1, sg);
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
    if (held(NR_PX_FILL))
        fill(p, from, end);
}

static void fade_update(void)                   /* FADE: down to nothing over a bar from its press */
{
    uint32_t bar = (uint32_t)(st16 >> 16) * 16, el;

    if (!held(NR_PX_FADE) || !bar) {
        fade_q = 256;
        fade_t0 = 0;
        return;
    }
    if (!fade_t0)
        fade_t0 = now ? now : 1;
    el = now - fade_t0;
    fade_q = (int16_t)(el >= bar ? 0 : 256 - (int32_t)((uint64_t)el * 256 / bar));
}

void nr_tick(void)
{
    int ok = synth_ok(), run = ok && finger(), p;
    uint16_t ev = ev_mask;
    uint32_t to = now + BLOCK;

    ev_mask = 0;
    if (!inited)
        return;
    if (ok)                                     /* due note-offs first: a new note may follow */
        offs_due(to);
    tempo();
    fade_update();
    if (ok)
        later_due();
    else
        nlater = 0;                             /* ARP notes to come: not into the drum */
    /* the patterns an artist holds: a clock each (TE's step events); NOTE ROLL's are played, TE's
     * too while a punch-in effect is held (taken over: TE's notes read at the press, TE plays nothing
     * of them meanwhile; released: TE's again) */
    {
        uint16_t take = 0;

        for (p = 0; p < NR_PATTERNS; p++) {
            nr_fs_clock *c = &nr_play[p];
            uint32_t from, end;

            if (!run || !st16 || (FS_ARTIST(0) != p && FS_ARTIST(1) != p)) {
                c->on = 0;
                continue;
            }
            if (!owned(p) && nr_px_held && FS_LOADED) {
                take |= (uint16_t)(1 << p);
                if (!(px_take >> p & 1))
                    take_import(p);
            }
            if (nr_fs_window(c, (ev >> p) & 1, st16, BLOCK, &from, &end) && end > from) {
                if (owned(p) || (take >> p & 1))
                    play_window(p, from, end);
                else
                    rep_key[p] = -1;
            }
        }
        px_take = take;
    }
    if (ok && preview_note >= 0) {              /* STEP REC: the key just played, a step long */
        hit_key(preview_note, 100, now, (uint32_t)(st16 >> 16));
        preview_note = -1;
    }
    if (ok && preview_req) {                    /* the cursor step's notes, at most one step */
        const nr_step *s = cstep();
        uint32_t ls = (uint32_t)(st16 >> 16), hold;
        int v;

        preview_req = 0;
        for (v = 0; v < NR_VOICES && s->n[v].key != NR_NONE; v++) {
            hold = ls * nr_len8[s->n[v].len < NR_LENS - 1 ? s->n[v].len : NR_LEN_DEF] / 8;
            hit(&s->n[v], now, hold < 64 ? 64 : hold > ls * 4 ? ls * 4 : hold);
        }
    }
    if (ok)                                     /* note-offs wait for synth playmode (never dropped) */
        offs_due(to);
    now = to;
}

/* ------------------------------------------------------------------ KeyHandler thread */

/* Controls as on DRUM ROLL: a knob turn edits the value under that knob, green press = next page,
 * blue press = the cursor note on / off (any page), white press = hear the step. */
enum {
    F_STEP, F_PITCH, F_VEL, F_PROB, F_LEN, F_NUDGE, F_RATCH, F_VOICE, F_SHAPE, F_INVERT,
    F_LENGTH, F_SWING, F_ZOOM, F_TRANSP, F_NONE
};
#define NR_PAGES    5
#define PG_PAT      3
#define PG_CLEAR    4
static const char *const page_name[NR_PAGES] = { "NOTE", "SHAPE", "CHORD", "PAT", "CLEAR" };
static const uint8_t page_fn[NR_PAGES][4] = {
    { F_STEP, F_PITCH, F_VEL, F_PROB },         /* 1 NOTE: which step, the note's pitch and velocity */
    { F_STEP, F_LEN, F_NUDGE, F_RATCH },        /* 2 SHAPE: the note's length, the step's timing */
    { F_STEP, F_VOICE, F_SHAPE, F_INVERT },     /* 3 CHORD: the step's notes */
    { F_LENGTH, F_SWING, F_ZOOM, F_TRANSP },    /* 4 PAT: the pattern */
    { F_NONE, F_NONE, F_NONE, F_NONE },         /* 5 CLEAR: push = the dialog */
};
static const char *const fn_name[] = {
    "STEP", "PITCH", "VEL", "PROB", "LEN", "NUDGE", "RATCH", "VOICE", "CHORD", "INVERT",
    "LENGTH", "SWING", "ZOOM", "TRANSPOSE", "",
};

static nr_step undo_step;                       /* the step before shift + orange press cleared it */
static volatile int16_t undo_of = -1;           /* pattern * 64 + step, -1 none */
static int8_t last_sel = -1;                    /* TE's selected pattern when last looked at */
static nr_fs_notes te_scratch;                  /* KeyHandler: one TE pattern (import) */

/* the cursor is a step and a pitch row (a piano roll's): the note there, or none */
static int voice_at(const nr_step *s, int key)
{
    int v;

    for (v = 0; v < NR_VOICES; v++)
        if (s->n[v].key == key)
            return v;
    return -1;
}

static nr_note *cnote(void)
{
    nr_step *s = cstep();
    int v = voice_at(s, nr_cpitch);

    return v >= 0 ? &s->n[v] : 0;
}

static void fix_voice(void)                     /* nr_voice: the cursor note's index, or the free slot */
{
    int v = voice_at(cstep(), nr_cpitch), n = nr_voices(cstep());

    nr_voice = (uint8_t)(v >= 0 ? v : n < NR_VOICES ? n : NR_VOICES - 1);
}

/* TE's pattern p as NOTE ROLL shows it (TE's own patterns only; nothing before TE loaded okseq) */
static void import(int p)
{
    int st, h;

    if (owned(p) || !FS_LOADED)
        return;
    for (st = 0; st < NR_FS_STEPS; st++)
        for (h = 0; h < NR_FS_HANDS; h++)
            te_scratch[st][h] = FS_NOTE(p, st, h);
    nr_fs_import(te_scratch, FS_LEN(p), &nr_pat[p]);
}

static void show_pattern(int p)
{
    nr_cur = (uint8_t)p;
    import(p);
    nr_cursor = (uint8_t)(nr_cursor % nr_pat[p].len);
    fix_voice();
}

/* ---- undo / redo: shift + BACK = undo, shift + FORWARD = redo, several steps, in the editor and on
 * TE's synth finger screen with it closed. A step is the state of one pattern before a change:
 * NOTE ROLL's pattern, TE's copy of it (notes + length) and whose it is, so TE's own finger screen
 * edits (found by the main thread's look at TE's copy) go back too. Knob turns of one kind, the
 * STEP REC keys of one step and TE's edits in a row within a second are one step; every press is
 * one. The history swaps: undo puts the step's state back and keeps the state it replaced for
 * redo. KeyHandler thread, but TE's edits are pushed by the main thread (h_busy: the other side
 * skips meanwhile) and TE's copy of a TE pattern is written back by the main thread (te_req). */
#define HIST        16
#define COALESCE    44100                       /* samples: the same kind of edit within 1 s */

typedef struct nr_snap {
    int8_t p;
    uint8_t owned, te_len, pad;
    nr_fs_notes te;
    nr_pattern pat;
} nr_snap;

static nr_snap hist[HIST], h_pend, swap_tmp;
static uint8_t h_start, h_len, h_pos;           /* ring: h_pos steps to undo, h_len - h_pos to redo */
static volatile uint8_t h_busy;
static int8_t pend_p = -1, last_p = -1;
static uint32_t last_src, last_at;
volatile uint8_t nr_undo_n, nr_redo_n;          /* bench reads */
static const char *volatile msg;
static volatile uint32_t msg_until;
static nr_fs_notes te_req[NR_PATTERNS];         /* TE's copy to write back (main thread) */
static int8_t te_req_len[NR_PATTERNS];
static volatile uint16_t te_req_mask;

static void snap_get(int p, nr_snap *sn)
{
    int st, h;

    sn->p = (int8_t)p;
    sn->owned = (uint8_t)owned(p);
    sn->pat = nr_pat[p];
    sn->te_len = (uint8_t)(FS_LOADED ? FS_LEN(p) : 16);
    for (st = 0; st < NR_FS_STEPS; st++)
        for (h = 0; h < NR_FS_HANDS; h++)
            sn->te[st][h] = FS_LOADED ? FS_NOTE(p, st, h) : -1;
}

static void snap_set(const nr_snap *sn)
{
    int p = sn->p, st, h;
    uint16_t bit = (uint16_t)(1 << p);

    nr_pat[p] = sn->pat;
    nr_owned = (uint16_t)(sn->owned ? nr_owned | bit : nr_owned & ~bit);
    if (sn->owned) {
        want_summary |= bit;                    /* TE's copy: the summary again */
    } else {
        for (st = 0; st < NR_FS_STEPS; st++)
            for (h = 0; h < NR_FS_HANDS; h++)
                te_req[p][st][h] = sn->te[st][h];
        te_req_len[p] = (int8_t)sn->te_len;
        te_req_mask |= bit;                     /* TE's own notes back (main thread) */
    }
    dirty = 1;
}

static void hist_counts(void)
{
    nr_undo_n = h_pos;
    nr_redo_n = (uint8_t)(h_len - h_pos);
}

static void hist_push(const nr_snap *sn, uint32_t src)
{
    int p = sn->p;

    if (h_pos && src < 0x400 && src == last_src && p == last_p && now - last_at < COALESCE) {
        last_at = now;                          /* the same kind of edit going on: one step */
        h_len = h_pos;
    } else {
        h_len = h_pos;
        if (h_len == HIST) {
            h_start = (uint8_t)((h_start + 1) % HIST);
            h_len--;
            h_pos--;
        }
        hist[(h_start + h_pos) % HIST] = *sn;
        h_len = ++h_pos;
        last_src = src;
        last_p = (int8_t)p;
        last_at = now;
    }
    hist_counts();
}

static void hist_before(int p)                  /* KeyHandler: before an event that may change p */
{
    snap_get(p, &h_pend);
    pend_p = (int8_t)p;
}

static void hist_after(uint32_t src)            /* after it: a step if the pattern changed */
{
    int p = pend_p;

    pend_p = -1;
    if (p < 0 || h_busy)
        return;
    if (h_pend.owned == owned(p) && nr_hash(&h_pend.pat, (int)sizeof(nr_pattern)) == nr_hash(&nr_pat[p], (int)sizeof(nr_pattern)))
        return;
    h_busy = 1;
    hist_push(&h_pend, src);
    h_busy = 0;
}

static void hist_step(int redo)
{
    nr_snap *e;

    if (h_busy)
        return;
    if (redo ? h_pos >= h_len : !h_pos) {
        msg = redo ? "NOTHING TO REDO" : "NOTHING TO UNDO";
        msg_until = now + COALESCE;
        return;
    }
    h_busy = 1;
    if (!redo)
        h_pos--;
    e = &hist[(h_start + h_pos) % HIST];
    snap_get(e->p, &swap_tmp);
    if (te_req_mask >> e->p & 1) {              /* a write back still to come: that is TE's copy */
        int st, h;

        for (st = 0; st < NR_FS_STEPS; st++)
            for (h = 0; h < NR_FS_HANDS; h++)
                swap_tmp.te[st][h] = te_req[e->p][st][h];
        swap_tmp.te_len = (uint8_t)te_req_len[e->p];
    }
    snap_set(e);
    *e = swap_tmp;
    if (redo)
        h_pos++;
    last_src = 0;
    hist_counts();
    h_busy = 0;
    if (nr_open)
        show_pattern(e->p);
    msg = redo ? "REDO" : "UNDO";
    msg_until = now + COALESCE;
}

static void follow(void)
{
    int s = FS_PLAYER_BUILT ? FS_SELECTED : -1;

    if (s < 0 || s >= NR_PATTERNS || s == last_sel)
        return;
    last_sel = (int8_t)s;
    show_pattern(s);
}

/* a new note at the cursor's pitch (the step full: none) */
static void add_note(void)
{
    nr_add(cstep(), nr_cpitch, 100, NR_LEN_DEF);
    fix_voice();
}

static void clear_pattern(void)
{
    int p = nr_cur % NR_PATTERNS;

    nr_pattern_init(pat());
    nr_pat[p].len = (uint8_t)clampi(FS_LOADED ? FS_LEN(p) : 16, 1, NR_FS_STEPS);
    nr_owned &= (uint16_t)~(1 << p);
    want_summary |= (uint16_t)(1 << p);         /* main thread: TE's copy empty too */
    if (undo_of >= 0 && undo_of / NR_STEPS == p)
        undo_of = -1;
    nr_cursor = 0;
    fix_voice();
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
    nr_pattern *p = pat();
    nr_step *s = cstep();
    nr_note *n = cnote();
    int i;

    switch (f) {
    case F_STEP:
        nr_cursor = (uint8_t)(((nr_cursor + d) % p->len + p->len) % p->len);
        fix_voice();
        break;
    case F_PITCH:                               /* the cursor note's pitch (the cursor goes along) */
        if (!n) {
            add_note();
            break;
        }
        i = nr_set_key(s, voice_at(s, nr_cpitch), n->key + d);
        nr_cpitch = s->n[i].key;
        fix_voice();
        break;
    case F_VEL:    if (n) n->vel = (uint8_t)clampi(n->vel + d * 5, 1, 100); break;
    case F_LEN:    if (n) n->len = (uint8_t)clampi(n->len + d, 0, NR_LENS - 1); break;
    case F_PROB:   s->prob = (uint8_t)clampi(s->prob + d * 5, 0, 100); break;
    case F_NUDGE:  s->nudge = (int8_t)clampi(s->nudge + d * 5, -50, 50); break;
    case F_RATCH:  s->ratch = (uint8_t)clampi(s->ratch + d, 1, 4); break;
    case F_VOICE: {                             /* to the step's next note up / down from the cursor */
        int v, best = -1;

        for (v = 0; v < NR_VOICES && s->n[v].key != NR_NONE; v++)
            if ((d > 0 ? s->n[v].key > nr_cpitch : s->n[v].key < nr_cpitch)
                && (best < 0 || (d > 0 ? s->n[v].key < best : s->n[v].key > best)))
                best = s->n[v].key;
        if (best >= 0)
            nr_cpitch = (uint8_t)best;
        fix_voice();
        break;
    }
    case F_SHAPE: {
        int sh = nr_shape_of(s);

        if (!nr_voices(s))
            add_note();
        nr_chord(s, clampi((sh < 0 ? 0 : sh) + d, 0, NR_SHAPES - 1));
        if (nr_voices(s))
            nr_cpitch = s->n[0].key;            /* the root */
        fix_voice();
        break;
    }
    case F_INVERT:
        nr_invert(s, d > 0);
        if (nr_voices(s))
            nr_cpitch = s->n[0].key;
        fix_voice();
        break;
    case F_LENGTH:
        p->len = (uint8_t)clampi(p->len + d, 1, NR_STEPS);
        nr_cursor = (uint8_t)(nr_cursor % p->len);
        break;
    case F_SWING:  nr_swing = (uint8_t)clampi(nr_swing + d * 5, 0, 100); break;
    case F_ZOOM:   nr_zoom = (uint8_t)zoom_step(nr_zoom, d); break;
    case F_TRANSP:                              /* every note of the pattern, if all stay MIDI notes */
        for (i = 0; i < NR_STEPS * NR_VOICES; i++) {
            int k = p->s[i / NR_VOICES].n[i % NR_VOICES].key;

            if (k != NR_NONE && (k + d < 0 || k + d > 127))
                return;
        }
        for (i = 0; i < NR_STEPS * NR_VOICES; i++)
            if (p->s[i / NR_VOICES].n[i % NR_VOICES].key != NR_NONE)
                p->s[i / NR_VOICES].n[i % NR_VOICES].key = (uint8_t)(p->s[i / NR_VOICES].n[i % NR_VOICES].key + d);
        nr_cpitch = (uint8_t)clampi(nr_cpitch + d, 0, 127);
        break;
    default:
        break;
    }
    if (!nr_play[nr_cur % NR_PATTERNS].on && (f == F_PITCH || f == F_VEL || f == F_LEN || f == F_SHAPE || f == F_INVERT))
        preview_req = 1;                        /* not playing: hear the step while shaping it */
}

/* after a key / knob event on pattern p (hash: p before it): an edit makes a TE pattern NOTE ROLL's;
 * the main thread then writes the summary into TE's copy */
static void after_event(int p, uint32_t before, int cleared)
{
    if (!cleared && nr_hash(&nr_pat[p], (int)sizeof(nr_pattern)) != before) {
        nr_owned |= (uint16_t)(1 << p);
        want_summary |= (uint16_t)(1 << p);
    }
    dirty = 1;
}

/* the black keys, low to high (F# G# A# C# D# F# G# A# C# D#: key ids): punch-in effects on TE's
 * synth finger screen, where a plain black press does nothing in TE (its finger player eats it, as
 * on the drum one): the 10 effects of the bank picked (green press on the finger screen, editor
 * closed: A -> B -> C); shift + black stays TE's step record */
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
        fx = (nr_px_bank % NR_PX_BANKS) * NR_PX_BANK + i;
        px_seq[fx] = ++px_count;
        nr_px_held |= 1u << fx;
        px_of[i] = (int8_t)fx;
        px_consumed |= (uint16_t)(1 << i);
        return 1;
    }
    if (!(px_consumed >> i & 1))
        return 0;                               /* its press was TE's: so is the release */
    px_consumed &= (uint16_t)~(1 << i);
    if (px_of[i] >= 0)
        nr_px_held &= ~(1u << px_of[i]);
    px_of[i] = -1;
    return 1;
}

/* STEP REC (shift + white press while open): the keyboard plays notes into the cursor step (the
 * first key after arriving at a step replaces its notes, more keys add to it), BACK / FORWARD go a
 * step back / on (FORWARD past an untouched step leaves it as it is; shift + BACK / FORWARD = undo /
 * redo), shift + green turn the octave; the 24 keys (ids by place, RE 2026-10-17) and the arrows
 * are ours, press and release */
static const uint8_t kb_ids[24] = {
    39, 13, 40, 14, 41, 15, 42, 28, 32, 29, 22, 30, 31, 23, 48, 24, 49, 25, 50, 51, 26, 52, 27, 53,
};
static uint32_t rec_consumed;                   /* bits 0..23 keys, 24 / 25 BACK / FORWARD */
static uint8_t undo_consumed;                   /* shift + BACK / FORWARD presses whose release is ours */
static uint8_t rec_touched;                     /* the cursor step was played into since arriving */
static int8_t rec_oct;

static void rec_toggle(void)
{
    nr_rec = !nr_rec;
    rec_touched = 0;
    rec_oct = (int8_t)clampi(FS_OCTAVE, -4, 4); /* TE's finger screen octave to start with */
}

static int rec_key(int key, int down)
{
    int i, p, note, bit;
    uint32_t before;

    if (key == TE_KEY_BACK || key == TE_KEY_FORWARD) {
        int d = key == TE_KEY_FORWARD ? 1 : -1;

        bit = 24 + (d > 0);
        if (!down) {
            if (!(rec_consumed >> bit & 1))
                return 0;
            rec_consumed &= ~(1u << bit);
            return 1;
        }
        nr_cursor = (uint8_t)(((nr_cursor + d) % pat()->len + pat()->len) % pat()->len);
        rec_touched = 0;
        fix_voice();
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
    p = nr_cur % NR_PATTERNS;
    before = nr_hash(&nr_pat[p], (int)sizeof(nr_pattern));
    hist_before(p);
    note = clampi(53 + i + 12 * rec_oct, 0, 127);   /* stored as TE's finger screen stores it */
    if (!rec_touched) {
        int v;

        for (v = 0; v < NR_VOICES; v++)
            cstep()->n[v].key = NR_NONE;        /* re-recorded: its old notes go (timing stays) */
        rec_touched = 1;
    }
    nr_add(cstep(), note, 100, NR_LEN_DEF);
    nr_cpitch = (uint8_t)note;
    fix_voice();
    preview_note = (int16_t)note;
    after_event(p, before, 0);
    hist_after(0x200 + nr_cursor);
    rec_consumed |= 1u << i;
    touch_at = now;
    return 1;
}

int nr_key(int key, int down)
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
    if (nr_rec && (shown() || !down) && (p = rec_key(key, down)) >= 0)
        return p;                               /* STEP REC: the keyboard and the arrows */
    if ((p = px_key(key, down)) >= 0)
        return p;
    if (k < 0 || k > 3) {                       /* the keyboard stays TE's (pattern keys, step record) */
        if (!down && shown())
            follow();
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
    if (!nr_open) {
        nr_ask = 0;
        if (k == 1 && !*TE_SHIFT_HELD) {        /* green press: the punch-in bank A -> B -> C */
            nr_px_bank = (uint8_t)((nr_px_bank + 1) % NR_PX_BANKS);
            msg = "FX BANK";
            msg_until = now + COALESCE;
            consumed |= 1 << k;
            return 1;
        }
        if (k != 0 || !*TE_SHIFT_HELD)
            return 0;
        nr_open = 1;                            /* shift + blue press: open on TE's selected pattern */
        last_sel = -1;
        show_pattern(nr_cur % NR_PATTERNS);
        follow();
        want_summary |= nr_owned;               /* TE's copies up to date (after a restart too) */
        consumed |= 1 << k;
        return 1;
    }
    touch_at = now;
    follow();
    p = nr_cur % NR_PATTERNS;
    before = nr_hash(&nr_pat[p], (int)sizeof(nr_pattern));
    hist_before(p);
    if (nr_ask) {                               /* the dialog: any push = ok on the picked answer */
        if (nr_ask_yes) {
            clear_pattern();
            cleared = 1;
        }
        nr_ask = 0;
    } else if (*TE_SHIFT_HELD) {
        if (k == 0 || k == 1) {
            nr_open = 0;                        /* shift + blue (or green) press: close */
            nr_rec = 0;
            save_now = 1;
        } else if (k == 2) {
            rec_toggle();                       /* shift + white: STEP REC on / off */
        } else if (k == 3) {                    /* shift + orange: clear the step (again: undo) */
            int at = p * NR_STEPS + nr_cursor % NR_STEPS;

            if (undo_of == at) {
                *cstep() = undo_step;
                undo_of = -1;
            } else {
                undo_step = *cstep();
                undo_of = (int16_t)at;
                nr_step_init(cstep());
            }
            fix_voice();
        }
    } else if (nr_page == PG_CLEAR && k != 1) {
        nr_ask = 1;                             /* blue / white / orange push: ask, NO picked */
        nr_ask_yes = 0;
    } else if (k == 0) {
        nr_note *n = cnote();                   /* blue: the note at the cursor's pitch on / off */

        if (n) {
            nr_remove(cstep(), voice_at(cstep(), nr_cpitch));
        } else {
            add_note();
            preview_req = !nr_play[p].on;
        }
        fix_voice();
    } else if (k == 1) {
        nr_page = (uint8_t)(nr_page + 1 < NR_PAGES ? nr_page + 1 : 0);
    } else if (k == 2) {
        preview_req = 1;
    }
    after_event(p, before, cleared);
    hist_after(0x400 + (uint32_t)k + (*TE_SHIFT_HELD ? 8u : 0u) + 16u * nr_page);
    consumed |= 1 << k;
    return 1;
}

int nr_knob(int knob, int16_t *diff)
{
    int d = *diff > 0 ? 1 : *diff < 0 ? -1 : 0, p;
    uint32_t before;

    if (!inited || !shown() || (unsigned int)knob > 3)
        return 0;
    if (!d)
        return 1;
    touch_at = now;
    if (nr_ask) {
        nr_ask_yes = d > 0;                     /* any knob: left NO, right YES */
        return 1;
    }
    follow();
    p = nr_cur % NR_PATTERNS;
    before = nr_hash(&nr_pat[p], (int)sizeof(nr_pattern));
    hist_before(p);
    if (*TE_SHIFT_HELD) {
        if (knob == 0) {
            nr_cpitch = (uint8_t)clampi(nr_cpitch + d, 0, 127);    /* the cursor's pitch row */
            fix_voice();
        }
        else if (knob == 1 && nr_rec)
            rec_oct = (int8_t)clampi(rec_oct + d, -4, 4);              /* STEP REC: the octave */
        else if (knob == 1)
            show_pattern(clampi(nr_cur + d, 0, NR_PATTERNS - 1));      /* finger pattern 1..14 */
    } else {
        edit(page_fn[nr_page % NR_PAGES][knob], d);
    }
    after_event(p, before, 0);
    hist_after(0x100 + (uint32_t)knob + 4u * nr_page + (*TE_SHIFT_HELD ? 64u : 0u) + 128u * nr_cursor);
    return 1;                                   /* all four knobs, with and without shift */
}

/* ------------------------------------------------------------------ main thread: TE's copy
 * TE's patterns in its okseq data are compared with what NOTE ROLL last saw there: TE's edits on a
 * NOTE ROLL pattern (step record, delete, clear, its length) go into it, TE's patterns on screen
 * are imported again; NOTE ROLL's summaries and lengths go out (TE's mark-dirty: its saver keeps
 * them). One thread writes TE's copy, so what NOTE ROLL wrote is never taken for TE's edit. */
static nr_fs_notes seen[NR_PATTERNS], cur_te, out_te;
static int8_t seen_len[NR_PATTERNS];
static uint8_t seen_ok[NR_PATTERNS];

static void read_te(int p, nr_fs_notes n)
{
    int s, h;

    for (s = 0; s < NR_FS_STEPS; s++)
        for (h = 0; h < NR_FS_HANDS; h++)
            n[s][h] = FS_NOTE(p, s, h);
}

static int same(const nr_fs_notes a, const nr_fs_notes b)
{
    int s;

    for (s = 0; s < NR_FS_STEPS; s++)
        if (a[s][0] != b[s][0] || a[s][1] != b[s][1])
            return 0;
    return 1;
}

static void sync_te(void)
{
    int p, s, h, len, mine;

    if (!FS_LOADED)
        return;
    for (p = 0; p < NR_PATTERNS; p++) {
        uint16_t bit = (uint16_t)(1 << p);
        int edited;

        if (te_req_mask & bit) {                /* undo / redo of a TE pattern: TE's notes back */
            te_req_mask &= (uint16_t)~bit;
            for (s = 0; s < NR_FS_STEPS; s++)
                for (h = 0; h < NR_FS_HANDS; h++) {
                    FS_NOTE(p, s, h) = te_req[p][s][h];
                    seen[p][s][h] = te_req[p][s][h];
                }
            FS_LEN(p) = seen_len[p] = te_req_len[p];
            FS_MARK_DIRTY(FS_DATA);
            if (p == nr_cur % NR_PATTERNS && nr_open)
                import(p);
        }
        read_te(p, cur_te);
        len = FS_LEN(p);
        if (!seen_ok[p]) {                      /* first look (boot): TE's copy as it is */
            for (s = 0; s < NR_FS_STEPS; s++)
                for (h = 0; h < NR_FS_HANDS; h++)
                    seen[p][s][h] = cur_te[s][h];
            seen_len[p] = (int8_t)len;
            seen_ok[p] = 1;
        }
        edited = !same(seen[p], cur_te) || len != seen_len[p];
        if (edited && !h_busy) {                /* TE's edit (finger screen): an undo step */
            static nr_snap tsn;

            h_busy = 1;
            tsn.p = (int8_t)p;
            tsn.owned = (uint8_t)owned(p);
            tsn.pat = nr_pat[p];
            tsn.te_len = (uint8_t)seen_len[p];
            for (s = 0; s < NR_FS_STEPS; s++)
                for (h = 0; h < NR_FS_HANDS; h++)
                    tsn.te[s][h] = seen[p][s][h];
            hist_push(&tsn, 0x300);
            h_busy = 0;
        }
        if (!owned(p)) {
            if (want_summary & bit) {           /* cleared in NOTE ROLL: TE's copy empty too */
                want_summary &= (uint16_t)~bit;
                for (s = 0; s < NR_FS_STEPS; s++)
                    for (h = 0; h < NR_FS_HANDS; h++)
                        if (FS_NOTE(p, s, h) >= 0)
                            FS_NOTE(p, s, h) = -1;
                FS_MARK_DIRTY(FS_DATA);
                read_te(p, cur_te);
                edited = 1;
            }
            if (edited && p == nr_cur % NR_PATTERNS && nr_open)
                import(p);
        } else {
            if (edited) {
                if (nr_fs_te_edit(&nr_pat[p], seen[p], cur_te))
                    dirty = 1;
                if (len != seen_len[p] && len >= 1 && len <= NR_FS_STEPS
                    && (nr_pat[p].len > NR_FS_STEPS ? NR_FS_STEPS : nr_pat[p].len) != len) {
                    nr_pat[p].len = (uint8_t)len;   /* TE's length: the pattern's */
                    dirty = 1;
                }
                want_summary |= bit;
            }
            if (want_summary & bit) {
                want_summary &= (uint16_t)~bit;
                nr_fs_summary(&nr_pat[p], out_te);
                mine = 0;
                for (s = 0; s < NR_FS_STEPS; s++)
                    for (h = 0; h < NR_FS_HANDS; h++)
                        if (FS_NOTE(p, s, h) != out_te[s][h]) {
                            FS_NOTE(p, s, h) = out_te[s][h];
                            mine = 1;
                        }
                len = nr_pat[p].len > NR_FS_STEPS ? NR_FS_STEPS : nr_pat[p].len;
                if (FS_LEN(p) != len) {
                    FS_LEN(p) = len;
                    mine = 1;
                }
                if (mine)
                    FS_MARK_DIRTY(FS_DATA);
                read_te(p, cur_te);
            }
        }
        for (s = 0; s < NR_FS_STEPS; s++)
            for (h = 0; h < NR_FS_HANDS; h++)
                seen[p][s][h] = cur_te[s][h];
        seen_len[p] = (int8_t)FS_LEN(p);
    }
    if (nr_open && nr_cursor >= pat()->len)
        nr_cursor = (uint8_t)(nr_cursor % pat()->len);
}

/* ------------------------------------------------------------------ GUI thread */

#define HDR_H       13
#define ROLL_Y      15
#define ROLL_H      88
#define GRID_X      50
#define GRID_W      268
#define PANEL_Y     (ROLL_Y + ROLL_H + 2)
#define COL_W       78

static const uint16_t col[4] = {
    GFX_RGB(0x69, 0x8e, 0xff), GFX_RGB(0x00, 0xed, 0x95),           /* blue, green */
    GFX_RGB(0xe6, 0xe6, 0xe6), GFX_RGB(0xff, 0x6a, 0x3d),           /* white, orange */
};
static const uint16_t c_bg = GFX_RGB(0x0c, 0x0c, 0x10), c_sel = GFX_RGB(0x26, 0x26, 0x36),
                      c_bar = GFX_RGB(0x5a, 0x58, 0x66), c_beat = GFX_RGB(0x2c, 0x2b, 0x34),
                      c_off = GFX_RGB(0x3a, 0x39, 0x44), c_dim = GFX_RGB(0x7c, 0x79, 0x90),
                      c_red = GFX_RGB(0xff, 0x3a, 0x5d), c_black_key = GFX_RGB(0x08, 0x08, 0x0b);
static const char *const key_name[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#",
                                        "B" };

static uint16_t shade(uint16_t c, int pct)
{
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31, m = pct * 1311 >> 10;

    return (uint16_t)(((r * m >> 7) << 11) | ((g * m >> 7) << 5) | (b * m >> 7));
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

/* view: steps v0 .. v0 + vn (16 / 32 / 64 steps), x in eighths of a step */
static int v0, vn;

static int xe(int e8)                           /* x of a time in eighths of a step from v0 */
{
    return GRID_X + (e8 - v0 * 8) * GRID_W / (vn * 8);
}

static int note_text(int x, int y, int key, uint16_t c, int scale)    /* "C#4" (60 = C4) */
{
    x = gfx_text(x, y, key_name[key % 12], c, scale);
    return gfx_dec(x, y, key / 12 - 1, c, scale);
}

static int playing(void)
{
    return nr_play[nr_cur % NR_PATTERNS].on;
}

static int te_playing(void)
{
    int p = nr_cur % NR_PATTERNS;

    return !owned(p) && FS_LOADED && (FS_ARTIST(0) == p || FS_ARTIST(1) == p);
}

static int playhead(void)
{
    int p = nr_cur % NR_PATTERNS, a, st;

    if (playing()) {
        uint32_t t = nr_play[p].next;

        return nr_playhead(pat(), t > BLOCK ? t - BLOCK : 0, st16 ? st16 : nr_step_q16(12000));
    }
    if (!te_playing())
        return -1;
    a = FS_ARTIST(0) == p ? 0 : 1;
    st = FS_STEP(a);
    return st < 0 || st >= NR_FS_STEPS ? -1 : st % pat()->len;
}

static void draw_header(void)
{
    int x, p = nr_cur % NR_PATTERNS, i;
    uint32_t bpm = bpm_x100();

    gfx_rect(0, 0, gfx_w, HDR_H, GFX_RGB(0x18, 0x18, 0x22));
    gfx_rect(2, 1, 19, 11, col[3]);             /* finger pattern 1..14 */
    gfx_dec(p + 1 < 10 ? 8 : 4, 2, p + 1, GFX_BLACK, 2);
    x = gfx_text(25, 2, owned(p) ? "NR" : "TE", owned(p) ? col[0] : c_dim, 2);
    if (nr_rec) {                               /* STEP REC */
        gfx_rect(x + 6, 1, 30, 11, c_red);
        x = gfx_text(x + 9, 2, "REC", GFX_BLACK, 2) + 3;
    }
    x = gfx_text(x + 8, 2, playing() || te_playing() ? "PLAY" : "STOP", playing() || te_playing() ? col[1] : c_dim, 2);
    if (bpm) {
        x = gfx_dec(x + 10, 2, (int32_t)((bpm + 50) / 100), GFX_WHITE, 2);
        x = gfx_text(x + 2, 6, "BPM", c_dim, 1);
    }
    if (nr_swing) {
        x = gfx_text(x + 8, 2, "SW", c_dim, 2);
        gfx_dec(x + 2, 2, nr_swing, GFX_WHITE, 2);
    }
    {
        char n[3] = { 'P', (char)('1' + nr_page % NR_PAGES), 0 };

        x = gfx_w - 2 - 8 * 8 - NR_PAGES * 4 - 4;
        for (i = 0; i < NR_PAGES; i++)
            gfx_rect(x + i * 4, 5, 2, 3, i == nr_page ? GFX_WHITE : c_off);
        x = gfx_text(x + NR_PAGES * 4 + 2, 2, n, GFX_WHITE, 2);
        gfx_text(x + 6, 2, page_name[nr_page % NR_PAGES], col[1], 2);
    }
}

static void draw_roll(void)
{
    const nr_pattern *p = pat();
    int len = p->len < 1 ? 1 : p->len, lo = 127, hi = 0, s, v, i, rh, rows, top, ph, span;
    int ck = nr_cpitch;

    /* the shown pitches: the pattern's range (at least an octave), the cursor note inside */
    for (s = 0; s < len; s++)
        for (v = 0; v < NR_VOICES && p->s[s].n[v].key != NR_NONE; v++) {
            lo = p->s[s].n[v].key < lo ? p->s[s].n[v].key : lo;
            hi = p->s[s].n[v].key > hi ? p->s[s].n[v].key : hi;
        }
    lo = ck < lo ? ck : lo;
    hi = ck > hi ? ck : hi;
    while (hi - lo < 11) {
        hi += hi < 127;
        lo -= lo > 0;
    }
    span = hi - lo + 3;
    rh = clampi(ROLL_H / span, 4, 11);          /* always a 1 px gap between rows */
    rows = ROLL_H / rh;
    if (span <= rows) {
        top = hi + 1 + (rows - span) / 2;
    } else {
        top = clampi(ck + rows / 2, lo - 2 + rows, hi + 1);
    }
    /* view: the cursor's page of 1 / 2 / 3 / 3.5 / 4 bars; while playing (and not touched lately) the
     * playhead's */
    vn = 8 * zoom_half[nr_zoom % ZOOMS];
    ph = playhead();
    nr_ph_shown = (int16_t)ph;
    i = ph >= 0 && now - touch_at > TOUCH_HOLD ? ph : nr_cursor;
    v0 = i / vn * vn;
    for (i = 0; i < rows; i++) {                /* rows: black keys darker, names in the column */
        int key = top - i, sem = ((key % 12) + 12) % 12, y = ROLL_Y + i * rh;

        if (key < 0 || key > 127)
            continue;
        if (sem == 1 || sem == 3 || sem == 6 || sem == 8 || sem == 10)
            gfx_rect(GRID_X, y, GRID_W, rh, c_black_key);
        if (key == ck)
            gfx_rect(0, y, GRID_X - 2, rh - 1, c_sel);
        if (sem == 0)
            gfx_rect(GRID_X, y + rh - 1, GRID_W, 1, c_beat);
        if (rh >= 7 || sem == 0 || key == ck)
            note_text(4, y + (rh - 5) / 2, key, sem == 0 || key == ck ? GFX_WHITE : c_dim, 1);
    }
    /* cursor step column, step / beat / bar lines, the loop line */
    {
        int a = xe(nr_cursor * 8), b = xe(nr_cursor * 8 + 8);

        if (clipx(&a, &b))
            gfx_rect(a, ROLL_Y, b - a, ROLL_H, c_sel);
    }
    for (s = v0; s <= v0 + vn; s++) {
        int x = xe(s * 8);

        if (x < GRID_X + GRID_W)
            gfx_rect(x, ROLL_Y, 1, ROLL_H, s % 16 == 0 ? c_bar : s % 4 == 0 ? c_beat : GFX_RGB(0x18, 0x18, 0x20));
    }
    if (len >= v0 && len < v0 + vn)
        gfx_rect(xe(len * 8), ROLL_Y, 1, ROLL_H, col[3]);
    /* notes: bars as long as they are held, velocity = brightness, repeats past the length dim;
     * a ratchet step split, a probable one dotted */
    for (s = v0; s < v0 + vn; s++) {
        const nr_step *st = &p->s[s % len];
        int ghost = s >= len, off = st->nudge * 8 / 100;

        for (v = 0; v < NR_VOICES && st->n[v].key != NR_NONE; v++) {
            const nr_note *n = &st->n[v];
            int r = top - n->key, y, x0, x1, l8 = n->len < NR_LENS - 1 ? nr_len8[n->len] : (len - s % len) * 8, h, k;
            int cur = s == nr_cursor && v == nr_voice;
            uint16_t c;

            if (r < 0 || r >= rows)
                continue;
            y = ROLL_Y + r * rh;
            x0 = xe(s * 8 + off) + 1;
            x1 = xe(s * 8 + off + l8);
            x1 = x1 <= x0 ? x0 + 1 : x1;
            c = shade(cur ? GFX_WHITE : col[1], (ghost ? 20 : 45) + (ghost ? 20 : 55) * n->vel / 100);
            h = rh - 1;
            if (!clipx(&x0, &x1))
                continue;
            for (k = 0; k < st->ratch; k++) {
                int a = x0 + (x1 - x0) * k / st->ratch, b = x0 + (x1 - x0) * (k + 1) / st->ratch - (st->ratch > 1);

                if (st->prob < 100) {
                    int j;

                    for (j = a; j < b; j += 2)
                        gfx_rect(j, y, 1, h, c);
                } else if (b > a) {
                    gfx_rect(a, y, b - a, h, c);
                }
            }
            if (cur)
                frame_rect(x0 - 1, y - 1, x1 - x0 + 2, h + 2, GFX_WHITE);
        }
    }
    if (!cnote() && nr_cursor >= v0 && nr_cursor < v0 + vn && top - nr_cpitch >= 0 && top - nr_cpitch < rows) {
        int a = xe(nr_cursor * 8) + 1, b = xe(nr_cursor * 8 + 8);     /* the cursor on an empty row */

        if (clipx(&a, &b))
            frame_rect(a - 1, ROLL_Y + (top - nr_cpitch) * rh - 1, b - a + 2, rh + 1, nr_rec ? c_red : GFX_WHITE);
    }
    if (ph >= 0 && ph >= v0 && ph < v0 + vn) {  /* playhead */
        int a = xe(ph * 8), b = xe(ph * 8 + 8);

        if (clipx(&a, &b))
            gfx_rect(a, ROLL_Y + ROLL_H - 2, b - a, 2, GFX_WHITE);
    }
}

static int fn_text(int f, int x, int y, uint16_t c)
{
    const nr_pattern *p = pat();
    const nr_step *s = cstep();
    const nr_note *n = cnote();
    int sh;

    switch (f) {
    case F_STEP:
        x = gfx_dec(x, y, nr_cursor + 1, c, 2);
        x = gfx_text(x, y, "/", c_dim, 2);
        return gfx_dec(x, y, p->len, c_dim, 2);
    case F_PITCH:  return n ? note_text(x, y, n->key, c, 2) : gfx_text(x, y, "+", c_dim, 2);
    case F_VEL:    return n ? gfx_dec(x, y, n->vel, c, 2) : x;
    case F_LEN:    return n ? gfx_text(x, y, nr_len_name[n->len % NR_LENS], c, 2) : x;
    case F_PROB:   return gfx_dec(x, y, s->prob, c, 2);
    case F_NUDGE:
        if (s->nudge > 0)
            x = gfx_text(x, y, "+", c, 2);
        return gfx_dec(x, y, s->nudge, c, 2);
    case F_RATCH:  x = gfx_text(x, y, "X", c, 2); return gfx_dec(x, y, s->ratch, c, 2);
    case F_VOICE:
        x = gfx_dec(x, y, nr_voice + 1, c, 2);
        x = gfx_text(x, y, "/", c_dim, 2);
        return gfx_dec(x, y, nr_voices(s), c_dim, 2);
    case F_SHAPE:
        sh = nr_shape_of(s);
        return gfx_text(x, y, !nr_voices(s) ? "-" : sh < 0 ? "FREE" : nr_shape_name[sh], c, 2);
    case F_INVERT: return gfx_text(x, y, "TURN", c_dim, 2);
    case F_LENGTH: return gfx_dec(x, y, p->len, c, 2);
    case F_SWING:  return gfx_dec(x, y, nr_swing, c, 2);
    case F_ZOOM:   x = zoom_text(x, y, nr_zoom, c, 2); return gfx_text(x + 2, y + 4, "BAR", c_dim, 1);
    case F_TRANSP: return gfx_text(x, y, "TURN", c_dim, 2);
    }
    return x;
}

static int fn_frac(int f, int *bip)
{
    const nr_pattern *p = pat();
    const nr_step *s = cstep();
    const nr_note *n = cnote();

    *bip = f == F_NUDGE;
    switch (f) {
    case F_STEP:   return p->len > 1 ? nr_cursor * 256 / (p->len - 1) : 0;
    case F_PITCH:  return n ? n->key * 256 / 127 : 0;
    case F_VEL:    return n ? n->vel * 256 / 100 : 0;
    case F_LEN:    return n ? n->len * 256 / (NR_LENS - 1) : 0;
    case F_PROB:   return s->prob * 256 / 100;
    case F_NUDGE:  return 128 + s->nudge * 128 / 50;
    case F_RATCH:  return (s->ratch - 1) * 256 / 3;
    case F_VOICE:  return nr_voices(s) > 1 ? nr_voice * 256 / (nr_voices(s) - 1) : 0;
    case F_SHAPE:  return nr_shape_of(s) > 0 ? nr_shape_of(s) * 256 / (NR_SHAPES - 1) : 0;
    case F_LENGTH: return (p->len - 1) * 256 / (NR_STEPS - 1);
    case F_SWING:  return nr_swing * 256 / 100;
    case F_ZOOM:   return zoom_pos(nr_zoom) * 256 / (ZOOMS - 1);
    }
    return 0;
}

static void draw_clear_page(void)
{
    int x = gfx_text(2, PANEL_Y + 4, "CLEAR PATTERN", GFX_WHITE, 2);

    gfx_dec(x + 8, PANEL_Y + 4, nr_cur % NR_PATTERNS + 1, col[3], 2);
    gfx_text(2, PANEL_Y + 18, "EMPTY HERE AND IN THE FINGER SEQUENCER - BACK TO TE", c_dim, 1);
    gfx_text(2, PANEL_Y + 26, "OTHER PATTERNS STAY", c_dim, 1);
    x = gfx_text(2, PANEL_Y + 34, "PUSH", c_dim, 1) + 4;
    x = gfx_text(x, PANEL_Y + 34, "BLUE", col[0], 1) + 4;
    x = gfx_text(x, PANEL_Y + 34, "WHITE", col[2], 1) + 4;
    x = gfx_text(x, PANEL_Y + 34, "OR", c_dim, 1) + 4;
    x = gfx_text(x, PANEL_Y + 34, "ORANGE", col[3], 1) + 4;
    gfx_text(x, PANEL_Y + 34, "TO CLEAR - ASKS FIRST", c_dim, 1);
}

#define ASK_W   184
#define ASK_H   62

static void draw_ask(void)
{
    int x0 = (gfx_w - ASK_W) / 2, y0 = ROLL_Y + 10, i;

    gfx_rect(x0, y0, ASK_W, ASK_H, GFX_BLACK);
    frame_rect(x0, y0, ASK_W, ASK_H, GFX_WHITE);
    i = gfx_text(x0 + 12, y0 + 8, "CLEAR PATTERN", GFX_WHITE, 2);
    i = gfx_dec(i + 8, y0 + 8, nr_cur % NR_PATTERNS + 1, col[3], 2);
    gfx_text(i, y0 + 8, "?", col[3], 2);
    for (i = 0; i < 2; i++) {
        int on = i == (nr_ask_yes != 0), bx = x0 + 22 + i * 76, by = y0 + 26;
        uint16_t c = i ? c_red : GFX_WHITE;

        if (on)
            gfx_rect(bx, by, 64, 18, c);
        else
            frame_rect(bx, by, 64, 18, c_off);
        gfx_text(bx + (i ? 20 : 24), by + 4, i ? "YES" : "NO", on ? GFX_BLACK : c_dim, 2);
    }
    gfx_text(x0 + 12, y0 + ASK_H - 10, "TURN: NO / YES   PUSH: OK", c_dim, 1);
}

/* info line: the step's notes */
static void draw_info(void)
{
    const nr_step *s = cstep();
    int x, y = PANEL_Y + 27, v, n = nr_voices(s), sh = nr_shape_of(s);

    x = gfx_text(2, y, "STEP", c_dim, 1);
    x = gfx_dec(x + 4, y, nr_cursor + 1, GFX_WHITE, 1);
    if (!n) {
        x = gfx_text(x + 8, y, "EMPTY - PUSH", c_dim, 1);
        x = gfx_text(x + 4, y, "BLUE", col[0], 1);
        gfx_text(x + 4, y, "FOR A NOTE", c_dim, 1);
    }
    for (v = 0; v < n; v++)
        x = note_text(x + 6, y, s->n[v].key, v == nr_voice ? GFX_WHITE : c_dim, 1);
    if (n > 1 && sh > 0)
        gfx_text(x + 8, y, nr_shape_name[sh], col[1], 1);
    if (nr_rec) {
        x = gfx_text(2, y + 9, "STEP REC: PLAY KEYS - BACK / FWD: STEP -", c_red, 1) + 4;
        x = gfx_shift(x, y + 9, c_red, 1);
        x = gfx_text(x, y + 9, "GREEN: OCTAVE", c_red, 1) + 4;
        gfx_dec(x, y + 9, rec_oct, GFX_WHITE, 1);
    } else if (nr_page == PG_PAT || nr_page == 2) {
        gfx_text(2, y + 9, nr_page == PG_PAT ? "LENGTH 1-64 STEPS - TE KEEPS UP TO 32 - TRANSPOSE: ALL NOTES"
                           : "VOICE: A NOTE OF THE STEP - CHORD: ON ITS LOWEST NOTE - INVERT", c_dim, 1);
    } else {
        x = gfx_shift(2, y + 9, c_dim, 1);
        x = gfx_text(x, y + 9, "BLUE TURN: PITCH ROW - BLUE: NOTE ON / OFF -", c_dim, 1) + 4;
        x = gfx_shift(x, y + 9, c_dim, 1);
        gfx_text(x, y + 9, "ORANGE: CLEAR STEP", c_dim, 1);
    }
}

static void draw_panel(void)
{
    int pg = nr_page % NR_PAGES, i, y = PANEL_Y + 2;

    if (pg == PG_CLEAR) {
        draw_clear_page();
        return;
    }
    for (i = 0; i < 4; i++) {
        int f = page_fn[pg][i], x = 2 + i * COL_W, bip, v = fn_frac(f, &bip), bw = COL_W - 8;

        gfx_text(x, y, fn_name[f], col[i], 1);
        fn_text(f, x, y + 7, GFX_WHITE);
        gfx_rect(x, y + 19, bw, 2, c_beat);
        if (bip) {
            int m = x + bw / 2, e = x + v * bw / 256;

            gfx_rect(e < m ? e : m, y + 19, (e < m ? m - e : e - m) + 1, 2, col[i]);
        } else {
            gfx_rect(x, y + 19, v * bw / 256, 2, col[i]);
        }
    }
    draw_info();
}

/* the held punch-in effects, "FX REPEAT 1/16 + OCT UP"; returns x */
static int px_text(int x, int y, int scale)
{
    int i, n = 0;

    for (i = 0; i < NR_PX_KEYS; i++)
        if (nr_px_held >> i & 1) {
            x = gfx_text(x, y, n ? " + " : "FX ", n ? c_dim : col[3], scale);
            n++;
            x = gfx_text(x, y, nr_px_name[i], col[3], scale);
        }
    return x;
}

static void draw_hints(void)
{
    int x = 2, y = gfx_h - 7;

    if (nr_px_held) {
        px_text(2, y, 1);
        return;
    }
    x = gfx_text(x, y, "PUSH", c_dim, 1) + 4;
    x = gfx_text(x, y, nr_page == PG_CLEAR ? "CLEAR" : "NOTE", col[0], 1) + 4;
    x = gfx_text(x, y, "PAGE", col[1], 1) + 4;
    x = gfx_text(x, y, nr_page == PG_CLEAR ? "CLEAR" : "HEAR", col[2], 1) + 8;
    x = gfx_shift(x, y, c_dim, 1);
    x = gfx_text(x, y, "TURN", c_dim, 1) + 4;
    x = gfx_text(x, y, "PITCH", col[0], 1) + 4;
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

    for (i = 0; i < NR_PX_KEYS; i++)
        if ((nr_px_held >> i & 1) && (last < 0 || px_seq[i] > px_seq[last]))
            last = i;
    if (last < 0)
        return;
    gfx_rect(x0, y0, w, h, frame_col);
    gfx_rect(x0 + 2, y0 + 2, w - 4, h - 4, GFX_BLACK);
    x = gfx_text(x0 + 10, y0 + 8, "PUNCH-IN", col[3], 2);
    x = gfx_text(x + 10, y0 + 12, "BANK", c_dim, 1) + 4;
    bank[0] = (char)('A' + last / NR_PX_BANK);
    x = gfx_text(x, y0 + 12, bank, col[1], 1) + 6;
    x = gfx_text(x, y0 + 12, "KEY", c_dim, 1) + 4;
    gfx_text(x, y0 + 12, key_name[last % NR_PX_BANK], GFX_WHITE, 1);
    for (r = 0; r < 8; r++)                     /* the pictogram, 8 x 8 cells of 6 px */
        for (c = 0; c < 8; c++)
            if (nr_px_icon[last][r] & (0x80 >> c))
                gfx_rect(x0 + 12 + c * 6, y0 + 30 + r * 6, 5, 5, col[3]);
    gfx_text(x0 + 74, y0 + 40, nr_px_name[last], GFX_WHITE, 3);
    for (i = 0, x = x0 + 74, y = y0 + 64; i < NR_PX_KEYS; i++)
        if (i != last && (nr_px_held >> i & 1)) {
            x = gfx_text(x, y, "+ ", c_dim, 1);
            x = gfx_text(x, y, nr_px_name[i], col[1], 1) + 6;
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
        char b[2] = { (char)('A' + nr_px_bank % NR_PX_BANKS), 0 };

        x = gfx_text(108, 49, msg, GFX_WHITE, 2);
        gfx_text(x + 8, 47, b, col[1], 3);
        return;
    }
    x = gfx_shift(104, 49, col[3], 2) + 4;
    x = gfx_text(x, 49, msg, GFX_WHITE, msg[0] == 'N' ? 1 : 2);
    if (msg[0] != 'N') {
        x = gfx_dec(x + 8, 52, msg[0] == 'U' ? nr_undo_n : nr_redo_n, col[3], 1);
        gfx_text(x + 3, 52, "LEFT", c_dim, 1);
    }
}

void nr_frame(uint16_t *buf)
{
    if (!inited || *TE_PLAYMODE != 0)
        return;
    gfx_begin(buf);
    if (!shown()) {
        nr_ask = 0;
        if (finger_screen()) {                  /* TE's finger screen: how to open the editor, at the */
            int x;                              /* size of the engines' "SHIFT+BLUE: PRESETS" */

            char b[2] = { (char)('A' + nr_px_bank % NR_PX_BANKS), 0 };

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
    gfx_rect(0, 0, gfx_w, gfx_h, c_bg);
    draw_header();
    draw_roll();
    gfx_rect(0, PANEL_Y - 1, gfx_w, 1, c_beat);
    draw_panel();
    draw_hints();
    if (nr_ask)
        draw_ask();
    draw_px_window();
    draw_msg();
}

/* ------------------------------------------------------------------ main thread: op1.db
 * Rows in TE's own database, table sdk_noteroll (id 0..13 patterns 1-14, 14 settings), as DRUM
 * ROLL's: the table is created first, every statement is ours and fixed apart from base64 (no
 * quote, no '%'), < 8 KB; reads use TE's lock and sqlite calls. */
#define SAVE_WAIT   375                         /* main-loop passes (~8 ms) without an edit: ~3 s */
#define STEP_TRIES  100

static char row[NR_ROW_TEXT_MAX];
static char sql[NR_ROW_TEXT_MAX + 96];
static uint8_t loaded, pending;
static uint16_t quiet;
static uint32_t saved_hash[NR_ROWS];

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

static int db_read(void *db, int id)
{
    te_db_query q = { db, 0 };
    char *o = sql;
    int rc = 0, n = 0, i;
    void *sem = *TE_DB_LOCK;

    put(&o, "SELECT data FROM sdk_noteroll WHERE id=");
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

            while (t && t[n] && n < (int)NR_ROW_TEXT_MAX - 1) {
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

    put(&o, "INSERT OR REPLACE INTO sdk_noteroll (id, data) VALUES (");
    put_id(&o, id);
    put(&o, ",'");
    put(&o, text);
    put(&o, "')");
    *o = 0;
    TE_DB_EXEC(db, sql);
}

static void settings_get(nr_settings *s)
{
    s->cur = nr_cur;
    s->swing = nr_swing;
    s->zoom = nr_zoom;
    s->pad = s->pad2[0] = s->pad2[1] = 0;
    s->owned[0] = (uint8_t)nr_owned;
    s->owned[1] = (uint8_t)(nr_owned >> 8);
}

static uint32_t row_hash(int id)
{
    nr_settings s;

    if (id < NR_PATTERNS)
        return nr_hash(&nr_pat[id], (int)sizeof(nr_pattern));
    settings_get(&s);
    return nr_hash(&s, (int)sizeof(s));
}

static void load_all(void *db)
{
    static nr_pattern p;
    nr_settings s;
    int id;

    TE_DB_EXEC(db, "CREATE TABLE IF NOT EXISTS sdk_noteroll (id INTEGER PRIMARY KEY, data TEXT NOT NULL)");
    for (id = 0; id < NR_PATTERNS; id++)
        if (db_read(db, id) && nr_load_pattern(id, row, &p) == 0)
            nr_pat[id] = p;
    if (db_read(db, NR_ROW_SETTINGS) && nr_load_settings(row, &s) == 0) {
        nr_cur = s.cur;
        nr_owned = (uint16_t)(s.owned[0] | s.owned[1] << 8);
        nr_swing = s.swing;
        nr_zoom = s.zoom;
    }
    for (id = 0; id < NR_ROWS; id++)
        saved_hash[id] = row_hash(id);          /* as stored (or defaults): nothing to write yet */
}

static void save_changed(void *db)
{
    int id;

    for (id = 0; id < NR_ROWS; id++) {
        uint32_t h = row_hash(id);

        if (h == saved_hash[id] || (id < NR_PATTERNS && !owned(id)))
            continue;                           /* TE's patterns are TE's to keep (okseq) */
        if (id < NR_PATTERNS) {
            nr_store_pattern(id, &nr_pat[id], row);
        } else {
            nr_settings s;

            settings_get(&s);
            nr_store_settings(&s, row);
        }
        db_write(db, id, row);
        saved_hash[id] = h;
    }
}

void nr_main(void)
{
    void *db;

    if (!inited)
        return;
    if (loaded)
        sync_te();                              /* after the load: NOTE ROLL's patterns are known */
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
    save_changed(db);
}
