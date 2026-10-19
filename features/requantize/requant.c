/* Requantize a tape take (spec: README.md, requantize-roll.md), v4: SYNTH ROLL.
 *
 * shift + blue press start a take: every note played while the tape records is logged with its
 *                    tape position, and the range TE records is remembered
 * shift + blue press end the take -> SYNTH ROLL (tape screen, DRUM ROLL's look): the piano roll and
 *                    four pages NOTE / FINE / QUANT / TAKE; a knob turn edits the value under it,
 *                    green press = next page, white press = preview (tape plays the take with its
 *                    track muted, the roll live), orange press = the page's action (quantize the
 *                    note(s) / all notes AUTO / commit onto the TAKE page's track, default the
 *                    take's own = replace); default mode CLEAN; shift + blue = exit
 * commit, same track: TE records overdub, so the take's clip is lifted first (TE's lift key: the
 *                    original goes to TE's clipboard, "drop" brings it back), then a normal record
 *                    pass from standstill replays the quantized notes and stops at the take's end.
 *                    The pass starts REC_PREROLL before the take: TE fades in the start of every
 *                    recording, the silent pre-roll takes the fade instead of the first notes.
 * commit, other track: same record pass on that track; the original track gets muted.
 * piano roll (v3, requantize-roll.md), NOTES knobs. The take is a note list;
 *                    AUTO notes follow the panel's grid + mode, FIXED notes are the user's
 *                    (moved / resized / snapped / as played); pitch and delete on both. Edits act
 *                    on the selection, else the cursor note. Preview and commit play the roll.
 * rq_tick            Seq thread, once per audio block: the sequencing above, replay by tape position
 * rq_frame           requantize screen / TAKE indicator
 *
 * Transport is driven like a user would: rec/play/stop/T/lift keys injected into TE's key queue,
 * each followed by a wake of TE's key thread (TE_KEY_WAKE) so TE's own guards stay in place;
 * seeking only while stopped. */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "rq_roll.h"

#define LOG_MAX     1024                        /* per source (keyboard, MIDI) */
#define OUT_MAX     2048
#define PREROLL     11025                       /* preview starts 0.25 s before the take */
#define REC_PREROLL 11025                       /* commit records from 0.25 s before the take: TE
                                                 * fades in every recording, the fade hits silence */
#define INSIDE      64                          /* lift position: one block into the take, so
                                                 * the take's clip is under the playhead for lift */
#define LIFT_WAIT   138                         /* ~0.2 s for TE to finish the lift */
#define KEY_LIFT    10                          /* "cut" key = lift on the tape screen */
#define WINDOW      1102                        /* 25 ms late window (as live quantize) */
#define CHORD       1764                        /* 40 ms chord window */
#define TIMEOUT     689                         /* ~1 s of audio blocks */
#define EDIT_TAPE   2
/* screen layout (as DRUM ROLL): header, roll with a pitch column, knob panel, hint line */
#define HDR_H       13
#define ROLL_Y      15
#define ROLL_H      88
#define GRID_X      50
#define GRID_W      268
#define PANEL_Y     (ROLL_Y + ROLL_H + 2)
#define KEY_TRIGGER 59                          /* blue knob press, with shift held */

enum { IDLE, TAKE, PANEL, PRE_SEEK, PRE_START, PRE_PLAY, COM_SEEK, COM_LIFT, COM_PRE, COM_ARM, COM_REC,
       COM_STOP };

/* grid sizes, longest first (num/den of a quarter); default 1/16 */
static const uint8_t g_num[] = { 2, 1, 2, 1, 1, 1, 1, 1, 1 };
static const uint8_t g_den[] = { 1, 1, 3, 2, 3, 4, 6, 8, 12 };
static const char *const g_name[] = { "1/2", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32",
                                      "1/32T" };
#define GRIDS 9
#define GRID_DEFAULT 5                          /* 1/16 */

volatile int rq_state;                          /* global: the bench reads it */
static rq_event logs[2][LOG_MAX];
static volatile int nlog[2];
static rq_event out[OUT_MAX];
volatile int rq_nout;
static qz_state scratch;
static volatile int take_track, take_have, range_have;
static volatile uint32_t take_start, take_end;
volatile uint32_t rq_rec_start, rq_rec_end;     /* range TE recorded during the take (bench reads) */
static volatile int p_grid = GRID_DEFAULT, p_mode = RQ_CLEAN, p_track;
static const char *const mode_name[] = { "SNAP", "QUEUE", "STRICT", "CLEAN" };     /* by RQ_* */
static const uint8_t mode_order[] = { RQ_CLEAN, RQ_SNAP, RQ_QUEUE, RQ_STRICT };     /* knob order */
#define MODES 4
static volatile int consumed, idx, timer, saved_mute_orig;
static volatile int mon_track = -1, saved_mute_mon, saved_sel; /* preview: track selected meanwhile */
static volatile uint32_t seek_pos, end_pos;
static volatile int lead;                       /* measured key latency, samples */
static const char *volatile status = "";
static uint8_t sounding[16];                    /* 128 keys: note-on sent, no note-off yet */

/* piano roll: the take as notes (key thread edits, GUI thread draws, render feeds out[]) */
static rq_note notes[RN_MAX];
static rq_event work[2 * LOG_MAX];              /* merged logs / render scratch */
static int ord[RN_MAX];                         /* notes in time order */
volatile int rq_page, rq_nnotes, rq_cur;        /* global: the bench reads them; page (PG_*); rq_cur:
                                                 * the note under the cursor, -1 none */
volatile uint32_t rq_step;                      /* the cursor: a grid step (from tape position 0) */
volatile int rq_pitch = 60;                     /* ... and a pitch row (NOTE ROLL's piano roll cursor) */
enum { PG_NOTE, PG_TIME, PG_QUANT, PG_TAKE, PAGES };
static volatile int zoom = 2, key_lo, key_hi;   /* zoom: 1 << zoom bars per screen */
static volatile uint32_t view_x0, view_w = 1;   /* shown tape range (samples) */
static volatile int64_t view_scale;             /* px per sample, 8.24 */

static void push_key(int key, int down)
{
    TE_KEY_PUSH((uint16_t)(key | (down ? 1 : 0) << 8));
    TE_KEY_WAKE();                              /* else the key waits for the next real press */
}

static void stop_tape(void)
{
    if (*TE_TAPE_STATE == 1 || *TE_TAPE_STATE == 2) {
        push_key(TE_KEY_STOP, 1);
        push_key(TE_KEY_STOP, 0);
    }
}

/* ------------------------------------------------------------------ logging */

static int log_event(int src, int kind, const te_note *n, int extra)
{
    int i = nlog[src], pm = *TE_PLAYMODE;
    rq_event *e;
    uint32_t pos;

    if (rq_state != TAKE || *TE_TAPE_STATE != 1 || i >= LOG_MAX || n->note < 0 || n->note > 127
        || (pm != 0 && pm != 1))
        return 0;
    pos = (uint32_t)(*TE_TAPE_POS >> 6);
    e = &logs[src][i];
    e->pos = pos;
    e->ev.kind = (uint8_t)kind;
    e->ev.key = (uint8_t)n->note;
    e->ev.data[0] = (uint8_t)n->vel;
    e->ev.data[1] = n->midi_out;
    e->ev.data[2] = (uint8_t)extra;
    e->ev.data[3] = (uint8_t)(pm << 1);
    e->ev.data[4] = (uint8_t)TE_DRUM_OCTAVE[*TE_DRUM_SLOT & 7];
    e->ev.data[5] = (uint8_t)*TE_DRUM_SHIFT_NOTE;
    nlog[src] = i + 1;
    if (!take_have || pos < take_start)
        take_start = pos;
    if (!take_have || pos > take_end)
        take_end = pos;
    take_have = 1;
    return 0;                                   /* observe only */
}

int rq_kb_on(te_note *n, int extra)   { return log_event(0, QZ_ON, n, extra); }
int rq_kb_off(te_note *n)             { return log_event(0, QZ_OFF, n, 0); }
int rq_midi_on(te_note *n, int extra) { return log_event(1, QZ_ON, n, extra); }
int rq_midi_off(te_note *n)           { return log_event(1, QZ_OFF, n, 0); }

/* ------------------------------------------------------------------ replay */

static void play(const qz_ev *e)
{
    te_note n;
    int drum = (e->data[3] >> 1) & 1, extra = (int8_t)e->data[2];
    int32_t shift_now;

    n.note = e->key;
    n.vel = (int8_t)e->data[0];
    n.midi_out = e->data[1];
    n.pad[0] = n.pad[1] = n.pad[2] = 0;
    n.extra = 0;
    if (e->kind != QZ_ON) {
        sounding[e->key >> 3] &= (uint8_t)~(1 << (e->key & 7));
        TE_VOICE_NOTE_OFF(&n);
        return;
    }
    if (drum != (*TE_PLAYMODE == 1))
        return;                                 /* logged with the other instrument */
    sounding[e->key >> 3] |= (uint8_t)(1 << (e->key & 7));
    if (!drum) {
        TE_VOICE_NOTE_ON(&n, extra);
        return;
    }
    extra += 12 * ((int8_t)e->data[4] - TE_DRUM_OCTAVE[*TE_DRUM_SLOT & 7]);
    shift_now = *TE_DRUM_SHIFT_NOTE;
    *TE_DRUM_SHIFT_NOTE = (int8_t)e->data[5];
    TE_VOICE_NOTE_ON(&n, extra);
    *TE_DRUM_SHIFT_NOTE = shift_now;
}

static void all_off(void)
{
    qz_ev e = { QZ_OFF, 0, { 0, 1, 0, 0, 0, 0 } };
    int k;

    for (k = 0; k < 128; k++)
        if (sounding[k >> 3] & (1 << (k & 7))) {
            e.key = (uint8_t)k;
            play(&e);
        }
}

/* note value num/den of a quarter at the current tempo, 16.16 samples (0: tempo out of range) */
static uint64_t step_of(uint32_t num, uint32_t den)
{
    float b = *TE_BPM;

    if (!(b >= 40.0f && b <= 220.0f))
        return 0;
    return qz_step_q16((uint32_t)(b * 100.0f + 0.5f), TE_SAMPLE_RATE, num, den);
}

static uint64_t grid_step(void)
{
    return step_of(g_num[p_grid], g_den[p_grid]);
}

/* the take ends: the range TE recorded (falls back to first..last note), the merged keyboard +
 * MIDI logs as a note list */
static void build_notes(void)
{
    int n = 0, s, i;

    if (!range_have || rq_rec_end <= rq_rec_start) {
        rq_rec_start = take_start;
        rq_rec_end = take_end;
    }
    for (s = 0; s < 2; s++)
        for (i = 0; i < nlog[s]; i++)
            work[n++] = logs[s][i];
    rq_nnotes = rq_notes_build(work, n, rq_rec_end, notes, RN_MAX);
    rq_cur = 0;
}

/* the note list with the panel settings -> out[] (AUTO notes quantized, FIXED as they are) */
static void render(void)
{
    uint64_t step = grid_step();

    rq_clean_from = rq_rec_start;               /* CLEAN: nothing before the take */
    rq_nout = step ? rq_render(notes, rq_nnotes, work, out, OUT_MAX, &scratch, step, p_mode,
                               WINDOW, CHORD)
                   : 0;
}

static uint32_t step_t(uint32_t k)              /* tape position of grid step k */
{
    uint64_t g = grid_step();

    return g ? rq_grid_pt(g, k) : 0;
}

/* the note under the cursor: its pitch, starting inside the cursor's grid step (first in time) */
static void find_cur(void)
{
    uint32_t a = step_t(rq_step), b = step_t(rq_step + 1);
    int p, i;

    rq_cur = -1;
    for (p = 0; p < rq_nnotes; p++) {
        i = ord[p];
        if (notes[i].key == rq_pitch && notes[i].start >= a && notes[i].start < b) {
            rq_cur = i;
            return;
        }
    }
}

static void cursor_to(int i)                    /* the cursor onto note i */
{
    uint64_t g = grid_step();

    if (i < 0 || i >= rq_nnotes || !g)
        return;
    rq_step = rq_grid_floor(g, notes[i].start);
    rq_pitch = notes[i].key;
}

/* time order, pitch range (the cursor's row in it) and the shown range after a change: the cursor
 * stays on screen; the note under the cursor */
static void follow(void)
{
    uint64_t bar = 4 * step_of(1, 1);
    uint32_t w, s;
    int i, lo = rq_pitch, hi = rq_pitch;

    rq_order(notes, rq_nnotes, ord);
    for (i = 0; i < rq_nnotes; i++) {
        lo = notes[i].key < lo ? notes[i].key : lo;
        hi = notes[i].key > hi ? notes[i].key : hi;
    }
    key_lo = lo;
    key_hi = hi;
    find_cur();
    if (!bar)
        return;
    w = (uint32_t)(((bar << zoom) + 0x8000) >> 16);
    s = step_t(rq_step);
    if (w != view_w || s < view_x0 + w / 10 || s > view_x0 + w - w / 10)
        view_x0 = s > w / 4 ? s - w / 4 : 0;
    view_w = w;
    view_scale = ((int64_t)GRID_W << 24) / w;
}

/* NOTE (P2): the cursor to the next / previous note in time order */
static void navigate(int d)
{
    uint32_t a = step_t(rq_step);
    int p, i = -1;

    if (rq_cur >= 0) {
        for (p = 0; p < rq_nnotes && ord[p] != rq_cur; p++)
            ;
        p += d;
        i = p >= 0 && p < rq_nnotes ? ord[p] : -1;
    } else if (d > 0) {
        for (p = 0; p < rq_nnotes && notes[ord[p]].start < a; p++)
            ;
        i = p < rq_nnotes ? ord[p] : -1;
    } else {
        for (p = rq_nnotes - 1; p >= 0 && notes[ord[p]].start >= a; p--)
            ;
        i = p >= 0 ? ord[p] : -1;
    }
    cursor_to(i);
}

/* ---- undo / redo (KeyHandler thread): shift + BACK = undo, shift + FORWARD = redo, several steps.
 * A list of the take's states (its note list after each edit, the first: as opened) in a pool;
 * undo / redo move through it. Knob turns of one kind within a second are one step, every press
 * one. What counts: pitch, flags, replay data, and start / length of FIXED notes (AUTO notes come
 * from grid + mode, which are settings, not edits). */
#define POOL        1536                        /* notes kept over all states */
#define STATES      24
#define COALESCE    44100                       /* samples: the same kind of edit within 1 s */

static rq_note pool[POOL];
static struct { uint16_t off, n; uint32_t hash; } st_list[STATES];
static int st_count, st_cur;                    /* states kept, the one shown */
static uint32_t h_src, h_at, clock;             /* clock: samples (Seq thread) */
volatile int rq_undo_n, rq_redo_n;              /* bench reads */

static uint32_t take_hash(void)
{
    uint32_t h = 2166136261u;
    int i;

    for (i = 0; i < rq_nnotes; i++) {
        const rq_note *t = &notes[i];
        uint32_t v[4];
        int k;

        v[0] = t->key | (uint32_t)(t->flags & ~RN_SEL) << 8 | (uint32_t)t->data[0] << 16;  /* (not the
                                                 * selection: no step) */
        v[1] = t->flags & RN_FIXED ? t->start : 0;
        v[2] = t->flags & RN_FIXED ? t->len : 0;
        v[3] = (uint32_t)i;
        for (k = 0; k < 4; k++)
            h = (h ^ v[k]) * 16777619u;
    }
    return h ^ (uint32_t)rq_nnotes;
}

static void hist_counts(void)
{
    rq_undo_n = st_cur;
    rq_redo_n = st_count - 1 - st_cur;
}

static void drop_oldest(void)                   /* the oldest state out, the pool moved down */
{
    int i, n = st_list[0].n, end = st_list[st_count - 1].off + st_list[st_count - 1].n;

    for (i = 0; i + n < end; i++)
        pool[i] = pool[i + n];
    for (i = 1; i < st_count; i++) {
        st_list[i - 1] = st_list[i];
        st_list[i - 1].off = (uint16_t)(st_list[i - 1].off - n);
    }
    st_count--;
    st_cur--;
}

/* the take as it is now: a new state after the shown one (states after it are gone) */
static void hist_push(void)
{
    int off, i;

    st_count = st_cur + 1;
    if (rq_nnotes > POOL) {                     /* too long to keep: no undo for it */
        st_count = st_cur = 0;
        hist_counts();
        return;
    }
    while (st_count >= STATES || (st_count && st_list[st_count - 1].off + st_list[st_count - 1].n + rq_nnotes > POOL))
        drop_oldest();
    off = st_count ? st_list[st_count - 1].off + st_list[st_count - 1].n : 0;
    for (i = 0; i < rq_nnotes; i++)
        pool[off + i] = notes[i];
    st_list[st_count].off = (uint16_t)off;
    st_list[st_count].n = (uint16_t)rq_nnotes;
    st_list[st_count].hash = take_hash();
    st_cur = st_count++;
    hist_counts();
}

static void hist_reset(void)                    /* opened: the take as it came, the first state */
{
    st_count = st_cur = 0;
    h_src = 0;
    hist_push();
}

/* after an edit: a step if the take changed (the same kind of knob turn within a second: the last
 * step grows instead) */
static void hist_commit(uint32_t src)
{
    if (st_count && st_list[st_cur].hash == take_hash())
        return;
    if (st_cur > 0 && st_cur == st_count - 1 && src < 0x400 && src == h_src && clock - h_at < COALESCE)
        st_cur--;                               /* replace the last step */
    hist_push();
    h_src = src;
    h_at = clock;
}

static void hist_step(int redo)
{
    int i;

    if (redo ? st_cur >= st_count - 1 : st_cur <= 0) {
        status = redo ? "NOTHING TO REDO" : "NOTHING TO UNDO";
        return;
    }
    st_cur += redo ? 1 : -1;
    rq_nnotes = st_list[st_cur].n;
    for (i = 0; i < rq_nnotes; i++)
        notes[i] = pool[st_list[st_cur].off + i];
    h_src = 0;
    hist_counts();
    render();
    follow();
    status = redo ? "REDO" : "UNDO";
}

/* an edit on the targets (the selection, else the note under the cursor); the cursor goes along
 * with its note */
static void edit_src(int op, int32_t amount, uint32_t src)
{
    int c = rq_cur, sel = 0, i;

    for (i = 0; i < rq_nnotes; i++)
        sel |= (notes[i].flags & RN_SEL) != 0;
    if (rq_edit(notes, rq_nnotes, c, op, amount, grid_step())) {
        render();
        if (c >= 0 && !sel && op != RE_SELECT_ALL && op != RE_AUTO_ALL)
            cursor_to(c);
        follow();
        hist_commit(src);
    }
}

static void edit(int op, int32_t amount)        /* a press's edit: a step of its own */
{
    edit_src(op, amount, 0x400 + (uint32_t)op);
}

/* blue press: a note at the cursor (one grid step long, FIXED; its replay data as the take's), or
 * the note under the cursor: one added here goes, one of the take is deleted / back */
static void toggle_note(void)
{
    uint8_t d[6] = { 100, 1, 0, 0, 0, 0xff };
    uint32_t a = step_t(rq_step), b = step_t(rq_step + 1);
    int i;

    if (rq_cur >= 0 && (notes[rq_cur].flags & RN_ADDED)) {
        rq_nnotes = rq_note_remove(notes, rq_nnotes, rq_cur);  /* added here: gone for good */
    } else if (rq_cur >= 0) {
        notes[rq_cur].flags ^= RN_DEL;          /* the take's: deleted / back */
    } else if (grid_step() && b > a) {
        if (rq_nnotes) {
            for (i = 0; i < 6; i++)
                d[i] = notes[ord[0]].data[i];
            d[0] = 100;
        } else {
            d[3] = (uint8_t)((*TE_PLAYMODE == 1) << 1);
            d[4] = (uint8_t)TE_DRUM_OCTAVE[*TE_DRUM_SLOT & 7];
            d[5] = (uint8_t)*TE_DRUM_SHIFT_NOTE;
        }
        rq_nnotes = rq_note_add(notes, rq_nnotes, RN_MAX, a, b - a, rq_pitch, d);
    }
    render();
    follow();
    hist_commit(0x500);
}

/* open: the smallest zoom that shows the whole take, starting just before it; the cursor on the
 * first note (none: the take's start, C4) */
static void open_roll(void)
{
    uint64_t bar = 4 * step_of(1, 1), g = grid_step();
    uint32_t len = rq_rec_end - rq_rec_start, w;

    render();
    for (zoom = 0; bar && zoom < 4 && ((bar << zoom) >> 16) < len + len / 8; zoom++)
        ;
    w = bar ? (uint32_t)(((bar << zoom) + 0x8000) >> 16) : 1;
    view_x0 = rq_rec_start > w / 16 ? rq_rec_start - w / 16 : 0;
    view_w = w;
    rq_page = rq_nnotes ? PG_NOTE : PG_TAKE;
    rq_order(notes, rq_nnotes, ord);
    rq_pitch = 60;
    rq_step = g ? rq_grid_floor(g, rq_rec_start) : 0;
    if (rq_nnotes)
        cursor_to(ord[0]);
    follow();
    hist_reset();
}

static void set_mute(int track, int on)
{
    TE_TAPE_MUTE[track & 3] = (uint8_t)(on != 0);
    TE_TAPE_GAINS_UPDATE();
}

static void select_track(int track)
{
    if (*TE_TAPE_TRACK != track) {
        push_key(TE_KEY_T1 + track, 1);         /* select a track like a user would */
        push_key(TE_KEY_T1 + track, 0);
    }
}

static void finish(const char *msg, int restore_mute)
{
    all_off();
    if (restore_mute)
        set_mute(take_track, saved_mute_orig);
    if (mon_track >= 0) {                       /* preview: back to the user's track selection */
        set_mute(mon_track, saved_mute_mon);
        select_track(saved_sel);
        mon_track = -1;
    }
    status = msg;
    rq_state = PANEL;
}

/* ------------------------------------------------------------------ Seq thread */

/* replay from the pass start: events before it (notes moved before the pre-roll) are skipped */
static void replay_from(uint32_t pos)
{
    for (idx = 0; idx < rq_nout && out[idx].pos < pos; idx++)
        ;
}

static void replay_until(uint32_t pos)
{
    while (idx < rq_nout && out[idx].pos <= pos + 64 && out[idx].pos < end_pos) {
        play(&out[idx].ev);
        idx++;
    }
}

static int landed(uint32_t pos, int track)
{
    return pos + 64 > seek_pos && pos < seek_pos + 64 && *TE_TAPE_TRACK == track;
}

static uint32_t preroll_pos(uint32_t start, uint32_t len)
{
    return start > len ? start - len : 0;
}

/* seek to the record pass start (stopped); COM_PRE starts the pass once the tape is there */
static void seek_record(void)
{
    seek_pos = preroll_pos(rq_rec_start, REC_PREROLL);
    TE_TAPE_SEEK((int32_t)seek_pos);            /* only stores a request: fine on this thread */
    rq_state = COM_PRE;
    timer = 0;
}

static void start_record(void)
{
    push_key(TE_KEY_REC, 1);                    /* rec armed, play -> record pass, latched */
    push_key(TE_KEY_PLAY, 1);
    push_key(TE_KEY_PLAY, 0);
    push_key(TE_KEY_REC, 0);
    replay_from(seek_pos);                      /* seek_pos = the pass start (pre-roll) */
    rq_state = COM_ARM;
    timer = 0;
}

void rq_tick(void)
{
    int st = rq_state, ts = *TE_TAPE_STATE;
    uint32_t pos = (uint32_t)(*TE_TAPE_POS >> 6);

    clock += TE_AUDIO_FRAMES;
    switch (st) {
    case TAKE:                                  /* remember the range TE records during the take */
        if (ts == 1) {
            if (!range_have || pos < rq_rec_start)
                rq_rec_start = pos;
            if (!range_have || pos > rq_rec_end)
                rq_rec_end = pos;
            range_have = 1;
        }
        return;
    case PRE_SEEK:
        if (landed(pos, mon_track)) {
            push_key(TE_KEY_PLAY, 1);
            push_key(TE_KEY_PLAY, 0);
            rq_state = PRE_START;
            replay_from(seek_pos);
            timer = 0;
        } else if (++timer > TIMEOUT)
            finish("SEEK FAILED", 1);
        return;
    case PRE_START:                             /* TE needs a few blocks to start the tape */
        if (ts == 2)
            rq_state = PRE_PLAY;
        else if (++timer > TIMEOUT)
            finish("PLAY FAILED", 1);
        return;
    case PRE_PLAY:
        replay_until(pos);
        if (ts != 2 || pos >= end_pos) {
            stop_tape();
            finish(ts != 2 ? "PREVIEW STOPPED" : "PREVIEW DONE", 1);
        }
        return;
    case COM_SEEK:
        if (landed(pos, p_track)) {
            timer = 0;
            idx = 0;
            if (p_track == take_track) {
                push_key(KEY_LIFT, 1);          /* lift the take's clip: region silent */
                push_key(KEY_LIFT, 0);
                rq_state = COM_LIFT;
            } else {
                start_record();                 /* start() already seeked to the pre-roll */
            }
        } else if (++timer > TIMEOUT)
            finish("TRACK/SEEK FAILED", 1);
        return;
    case COM_LIFT:
        if (++timer >= LIFT_WAIT)
            seek_record();                      /* back to the pre-roll before the take */
        return;
    case COM_PRE:
        if (landed(pos, p_track))
            start_record();
        else if (++timer > TIMEOUT)
            finish("SEEK FAILED", 0);
        return;
    case COM_ARM:                               /* measure TE's key latency while it starts */
        if (ts == 1) {
            lead = (timer + 1) * 64;
            rq_state = COM_REC;
        } else if (++timer > TIMEOUT) {
            stop_tape();
            finish("REC FAILED (OUT OF CLIPS?)", 0);
        }
        return;
    case COM_REC:
        replay_until(pos);
        if (ts != 1) {
            all_off();
            finish("RECORDING STOPPED", 0);
            return;
        }
        if (pos + (uint32_t)lead >= end_pos) {  /* stop exactly at the take's end */
            stop_tape();
            all_off();
            rq_state = COM_STOP;
            timer = 0;
        }
        return;
    case COM_STOP:
        if (ts == 0 || ++timer > TIMEOUT)
            finish(p_track == take_track ? "DONE: TAKE REPLACED (DROP = ORIGINAL)"
                                         : "DONE: ORIGINAL TRACK MUTED", 0);
        return;
    }
}

/* ------------------------------------------------------------------ KeyHandler thread */

static void start(int commit)
{
    if (*TE_TAPE_STATE != 0) {
        status = "STOP THE TAPE FIRST";
        return;
    }
    render();
    if (!rq_nout) {
        status = "NOTHING TO PLAY";
        return;
    }
    end_pos = rq_rec_end;
    saved_mute_orig = TE_TAPE_MUTE[take_track & 3];
    if (!commit || p_track != take_track)
        set_mute(take_track, 1);                /* hear / keep only the quantized version */
    if (commit) {
        set_mute(p_track, 0);
        select_track(p_track);
        /* replace: lift first (playhead inside the take), then seek_record(); another track:
         * straight to the pre-roll */
        seek_pos = p_track == take_track ? rq_rec_start + INSIDE
                                         : preroll_pos(rq_rec_start, REC_PREROLL);
    } else {
        /* TE plays the live synth through the selected track's gain: with the take's track muted
         * and selected the preview notes are silent (device finding). Select an unmuted track
         * meanwhile (none: unmute the next one); finish() restores mute and selection. */
        int i, t = (take_track + 1) & 3;

        for (i = 1; i < 4; i++)
            if (!TE_TAPE_MUTE[(take_track + i) & 3]) {
                t = (take_track + i) & 3;
                break;
            }
        saved_sel = *TE_TAPE_TRACK & 3;
        saved_mute_mon = TE_TAPE_MUTE[t];
        mon_track = t;
        set_mute(t, 0);
        select_track(t);
        seek_pos = preroll_pos(rq_rec_start, PREROLL);
    }
    TE_TAPE_SEEK((int32_t)seek_pos);
    timer = 0;
    status = commit ? "RECORDING..." : "PREVIEW...";
    rq_state = commit ? COM_SEEK : PRE_SEEK;
}

static void abort_run(void)
{
    int st = rq_state;

    stop_tape();
    finish("STOPPED", st == PRE_SEEK || st == PRE_START || st == PRE_PLAY);
}

/* Controls as NOTE ROLL's (the piano roll): the cursor is a grid step and a pitch row; a knob turn
 * edits the value under that knob, green press = next page, blue press = a note at the cursor on /
 * off (an empty place: a new note a grid step long; a note: deleted / back), white press = preview
 * (again = stop), orange press = the page's action (NOTE / TIME: quantize the targets, QUANT: every
 * note back to AUTO, TAKE: commit). Shift: blue turn = the pitch row; blue press exit, green press
 * select all / none, white press select the cursor note, orange press as played <-> AUTO. Targets:
 * the selection, else the note under the cursor. */
static int mode_index(void)
{
    int i;

    for (i = 0; i < MODES && mode_order[i] != p_mode; i++)
        ;
    return i < MODES ? i : 0;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void panel_key(int k, int shift)
{
    status = "";
    if (shift) {
        if (k == 1)
            edit(RE_SELECT_ALL, 0);
        else if (k == 2)
            edit(RE_SELECT, 0);
        else if (k == 3)
            edit(RE_PLAYED, 0);                 /* as played <-> AUTO */
        return;
    }
    if (k == 0)
        toggle_note();
    else if (k == 1)
        rq_page = (rq_page + 1) % PAGES;
    else if (k == 2)
        start(0);                               /* preview */
    else if (rq_page == PG_QUANT)
        edit(RE_AUTO_ALL, 0);                   /* the whole take follows grid + mode again */
    else if (rq_page == PG_TAKE)
        start(1);                               /* commit */
    else
        edit(RE_SNAP, 0);                       /* quantize the targets (start + length) */
}

static void panel_knob(int knob, int d, int shift)
{
    int pg = rq_page;

    status = "";
    if (knob == 0 && shift) {                   /* the cursor's pitch row */
        rq_pitch = clampi(rq_pitch + d, 0, 127);
        follow();
    } else if (pg == PG_QUANT) {
        uint32_t t = step_t(rq_step);
        uint64_t g;

        if (knob == 0)
            p_grid = clampi(p_grid + d, 0, GRIDS - 1);
        else if (knob == 1)
            p_mode = mode_order[clampi(mode_index() + d, 0, MODES - 1)];
        else if (knob == 2)
            zoom = clampi(zoom + d, 0, 4);
        else
            return;
        g = grid_step();
        rq_step = g ? rq_grid_floor(g, t) : 0;  /* the cursor keeps its time on the new grid */
        render();                               /* AUTO notes move with grid and mode */
        follow();
    } else if (pg == PG_TAKE) {
        if (knob == 0)
            p_track = (p_track + d) & 3;
    } else if (pg == PG_NOTE) {
        if (knob == 0) {
            rq_step = d < 0 && !rq_step ? 0 : rq_step + (uint32_t)d;
            follow();
        } else if (knob == 1) {
            if (rq_cur >= 0)
                edit_src(RE_PITCH, d, 0x101);
            else {
                rq_pitch = clampi(rq_pitch + d, 0, 127);    /* an empty cursor: its row */
                follow();
            }
        } else if (knob == 2) {
            edit_src(RE_VEL, 5 * d, 0x102);
        } else {
            edit_src(RE_LEN_STEP, d, 0x103);
        }
    } else if (knob == 0) {                     /* TIME */
        navigate(d);
        follow();
    } else if (knob == 1) {
        edit_src(RE_MOVE_STEP, d, 0x111);
    } else if (knob == 2) {
        edit_src(RE_MOVE, d * RN_FINE, 0x112);
    } else {
        edit_src(RE_LEN, d * RN_FINE, 0x113);
    }
}

int rq_key(int key, int down)
{
    int st = rq_state, k = key - 59;

    if (key == KEY_TRIGGER && *TE_SHIFT_HELD
        && !(rq_state == IDLE && (*TE_PLAYMODE == 1 || *TE_EDIT_MODE == TE_EDIT_MODE_SEQ))) {
        /* drum playmode and the sequencer screen: shift + blue stays TE's (DRUM / NOTE ROLL) */
        if (down) {
            if (st == IDLE || st == PANEL) {
                if (st == PANEL) {
                    rq_state = IDLE;            /* exit the requantize screen */
                } else {
                    nlog[0] = nlog[1] = 0;      /* new take */
                    take_have = range_have = 0;
                    take_track = *TE_TAPE_TRACK & 3;
                    p_track = take_track;       /* default: replace the take */
                    p_grid = GRID_DEFAULT;
                    p_mode = RQ_CLEAN;          /* default: clean and sharp on the grid */
                    status = "";
                    rq_state = TAKE;
                }
            } else if (st == TAKE) {
                if (nlog[0] || nlog[1]) {
                    build_notes();
                    status = "";
                    open_roll();
                    rq_state = PANEL;
                } else {
                    rq_state = IDLE;            /* nothing was recorded during the take */
                }
            } else {
                abort_run();
            }
        }
        if (down)
            consumed |= 1;
        else
            consumed &= ~1;                     /* released with shift still held */
        return 1;
    }
    if (key == KEY_TRIGGER && !down && (consumed & 1)) {
        consumed &= ~1;
        return 1;
    }
    /* SYNTH ROLL: shift + BACK / FORWARD = undo / redo */
    if ((key == TE_KEY_BACK || key == TE_KEY_FORWARD) && st == PANEL && *TE_EDIT_MODE == EDIT_TAPE
        && (down ? *TE_SHIFT_HELD : (consumed >> (2 + (key == TE_KEY_FORWARD))) & 1)) {
        int bit = 2 + (key == TE_KEY_FORWARD);

        if (down) {
            hist_step(key == TE_KEY_FORWARD);
            consumed |= 1 << bit;
        } else {
            consumed &= ~(1 << bit);
        }
        return 1;
    }
    /* requantize screen and running preview/commit: knob clicks on the tape screen are ours */
    if (k < 0 || k > 3 || st < PANEL || *TE_EDIT_MODE != EDIT_TAPE)
        return 0;
    if (down && st == PANEL)
        panel_key(k, *TE_SHIFT_HELD);
    else if (down && (st == PRE_SEEK || st == PRE_START || st == PRE_PLAY) && k == 2)
        abort_run();                            /* white again: stop the preview */
    return 1;
}

int rq_knob(int knob, int16_t *diff)
{
    int d = *diff > 0 ? 1 : *diff < 0 ? -1 : 0;

    if (rq_state != PANEL || *TE_EDIT_MODE != EDIT_TAPE)
        return 0;
    if (d)
        panel_knob(knob, d, *TE_SHIFT_HELD);
    return 1;                                   /* all four knobs, with and without shift */
}

/* ------------------------------------------------------------------ GUI thread
 * SYNTH ROLL: DRUM ROLL's look (black, thin lines, the four knob colours): header with the take's
 * track and the page, the roll (pitch rows, bars / beats / grid, the take's range in red), four
 * knob columns per page, an info line and the hint line. */

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
static const char *const page_name[PAGES] = { "NOTE", "TIME", "QUANT", "TAKE" };
static const char *const fn_name[PAGES][4] = {
    { "STEP", "PITCH", "VEL", "LENGTH" },
    { "NOTE", "MOVE", "NUDGE", "LEN MS" },
    { "GRID", "MODE", "ZOOM", "ALL AUTO" },
    { "TRACK", "", "", "" },
};

/* c at pct % brightness (pct 0..100) */
static uint16_t shade(uint16_t c, int pct)
{
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31, m = pct * 1311 >> 10;

    return (uint16_t)(((r * m >> 7) << 11) | ((g * m >> 7) << 5) | (b * m >> 7));
}

/* screen x of a tape position (clamped far outside the grid) */
static int xpos(uint32_t pos)
{
    int64_t x = (((int64_t)pos - (int64_t)view_x0) * view_scale) >> 24;

    return GRID_X + (x < -1000 ? -1000 : x > 1000 ? 1000 : (int)x);
}

static int clipx(int *x0, int *x1)
{
    if (*x0 < GRID_X)
        *x0 = GRID_X;
    if (*x1 > GRID_X + GRID_W)
        *x1 = GRID_X + GRID_W;
    return *x1 > *x0;
}

static void frame_rect(int x, int y, int w, int h, uint16_t c)
{
    gfx_rect(x, y, w, 1, c);
    gfx_rect(x, y + h - 1, w, 1, c);
    gfx_rect(x, y, 1, h, c);
    gfx_rect(x + w - 1, y, 1, h, c);
}

/* vertical lines every step (16.16) across the shown range, if at least min_px apart */
static void lines(uint64_t step, int min_px, uint16_t c)
{
    uint32_t k, p, end = view_x0 + view_w;

    if (!step || (((int64_t)(step >> 16) * view_scale) >> 24) < min_px)
        return;
    for (k = rq_grid_floor(step, view_x0); (p = rq_grid_pt(step, k)) <= end; k++)
        if (p >= view_x0)
            gfx_rect(xpos(p), ROLL_Y, 1, ROLL_H, c);
}

static int note_text(int x, int y, int key, uint16_t c, int scale)     /* "C#4" (60 = C4) */
{
    x = gfx_text(x, y, key_name[key % 12], c, scale);
    return gfx_dec(x, y, key / 12 - 1, c, scale);
}

static int ms_text(int x, int y, int32_t samples, int sign, uint16_t c, int scale)
{
    int32_t a = samples < 0 ? -samples : samples;

    if (sign)
        x = gfx_text(x, y, samples < 0 ? "-" : "+", c, scale);
    x = gfx_dec(x, y, (a * 10 + 220) / 441, c, scale);
    return gfx_text(x, y, "MS", c, scale);
}

static int barbeat_text(int x, int y, uint32_t pos, uint16_t c, int scale)    /* "3.2" */
{
    uint64_t beat = step_of(1, 1);
    uint32_t b;

    if (!beat)
        return x;
    b = rq_grid_floor(beat, pos);
    x = gfx_dec(x, y, (int32_t)(b / 4 + 1), c, scale);
    x = gfx_text(x, y, ".", c, scale);
    return gfx_dec(x, y, (int32_t)(b % 4 + 1), c, scale);
}

/* length in grid steps, rounded ("2" / "~2" when not whole) */
static int steps_text(int x, int y, uint32_t len, uint16_t c, int scale)
{
    uint64_t step = grid_step();
    uint32_t n;

    if (!step)
        return x;
    n = (uint32_t)((((uint64_t)len << 16) + step / 2) / step);
    {
        int32_t d = (int32_t)(len - (uint32_t)(((uint64_t)n * step) >> 16));

        if (d > 1 || d < -1)                    /* triplet grid points truncate by 1 */
            x = gfx_text(x, y, "~", c_dim, scale);
    }
    return gfx_dec(x, y, (int32_t)n, c, scale);
}

static int cursor_pos(void)
{
    int i;

    for (i = 0; i < rq_nnotes && ord[i] != rq_cur; i++)
        ;
    return i;
}

static int fixed_count(void)
{
    int i, n = 0;

    for (i = 0; i < rq_nnotes; i++)
        n += (notes[i].flags & (RN_FIXED | RN_DEL)) == RN_FIXED;
    return n;
}

static int vel_of(const rq_note *t)
{
    int v = (int8_t)t->data[0];

    return v < 0 ? 127 : v;
}

/* "3.2.4": bar, beat, grid step in the beat */
static int step_text(int x, int y, uint32_t k, uint16_t c, int scale)
{
    uint64_t beat = step_of(1, 1), g = grid_step();
    uint32_t t = step_t(k), b0;

    x = barbeat_text(x, y, t, c, scale);
    if (!beat || !g)
        return x;
    b0 = rq_grid_pt(beat, rq_grid_floor(beat, t));
    x = gfx_text(x, y, ".", c, scale);
    return gfx_dec(x, y, (int32_t)(k - rq_grid_floor(g, b0)) + 1, c, scale);
}

/* the value under knob i of the page (scale 2) and its bar 0..256 (*bip: from the middle) */
static int fn_text(int i, int x, int y, uint16_t c)
{
    const rq_note *t = rq_cur >= 0 ? &notes[rq_cur] : 0;
    uint64_t step = grid_step();

    switch (rq_page * 4 + i) {
    case PG_NOTE * 4 + 0:
        return step_text(x, y, rq_step, c, 2);
    case PG_NOTE * 4 + 1:
        return note_text(x, y, rq_pitch, t ? c : c_dim, 2);
    case PG_NOTE * 4 + 2:
        return t ? gfx_dec(x, y, vel_of(t), c, 2) : gfx_text(x, y, "-", c_dim, 2);
    case PG_NOTE * 4 + 3:
        return t ? steps_text(x, y, t->len, c, 2) : gfx_text(x, y, "-", c_dim, 2);
    case PG_TIME * 4 + 0:
        if (!t)
            x = gfx_text(x, y, "-", c_dim, 2);
        else
            x = gfx_dec(x, y, cursor_pos() + 1, c, 2);
        x = gfx_text(x, y, "/", c_dim, 2);
        return gfx_dec(x, y, rq_nnotes, c_dim, 2);
    case PG_TIME * 4 + 1:
        return t ? barbeat_text(x, y, t->start, c, 2) : gfx_text(x, y, "-", c_dim, 2);
    case PG_TIME * 4 + 2:
        if (!t || !step)
            return gfx_text(x, y, "-", c_dim, 2);
        if (t->start == rq_grid_nearest(step, t->start))
            return gfx_text(x, y, "GRID", c, 2);
        return ms_text(x, y, (int32_t)(t->start - rq_grid_nearest(step, t->start)), 1, c, 2);
    case PG_TIME * 4 + 3:
        return t ? ms_text(x, y, (int32_t)t->len, 0, c, 2) : gfx_text(x, y, "-", c_dim, 2);
    case PG_QUANT * 4 + 0:
        return gfx_text(x, y, g_name[p_grid], c, 2);
    case PG_QUANT * 4 + 1:
        return gfx_text(x, y, mode_name[p_mode & 3], c, 2);
    case PG_QUANT * 4 + 2:
        x = gfx_dec(x, y, 1 << zoom, c, 2);
        return gfx_text(x + 2, y + 4, zoom ? "BARS" : "BAR", c_dim, 1);
    case PG_QUANT * 4 + 3:
        x = gfx_dec(x, y, fixed_count(), c, 2);
        return gfx_text(x + 2, y + 4, "FIXED", c_dim, 1);
    case PG_TAKE * 4 + 0:
        x = gfx_text(x, y, "T", c, 2);
        return gfx_dec(x, y, p_track + 1, p_track == take_track ? c_red : c, 2);
    }
    return x;
}

static int fn_frac(int i, int *bip)
{
    const rq_note *t = rq_cur >= 0 ? &notes[rq_cur] : 0;
    uint64_t step = grid_step();
    uint32_t len = rq_rec_end - rq_rec_start, a = step_t(rq_step);

    *bip = 0;
    switch (rq_page * 4 + i) {
    case PG_NOTE * 4 + 0:
        return len && a > rq_rec_start ? clampi((int)((uint64_t)(a - rq_rec_start) * 256 / len), 0, 256) : 0;
    case PG_NOTE * 4 + 1:
        return rq_pitch * 256 / 127;
    case PG_NOTE * 4 + 2:
        return t ? vel_of(t) * 256 / 127 : 0;
    case PG_NOTE * 4 + 3: case PG_TIME * 4 + 3:
        return t && step ? (int)clampi((int)((((uint64_t)t->len << 16) / step) * 16), 0, 256) : 0;
    case PG_TIME * 4 + 0:
        return t && rq_nnotes > 1 ? cursor_pos() * 256 / (rq_nnotes - 1) : 0;
    case PG_TIME * 4 + 1:
        return t && len && t->start > rq_rec_start ? clampi((int)((uint64_t)(t->start - rq_rec_start) * 256 / len), 0, 256) : 0;
    case PG_TIME * 4 + 2: {
        int32_t off = t && step ? (int32_t)(t->start - rq_grid_nearest(step, t->start)) : 0, half = (int32_t)(step >> 17);

        *bip = 1;
        return half ? 128 + clampi(off * 128 / half, -128, 128) : 128;
    }
    case PG_QUANT * 4 + 0:
        return p_grid * 256 / (GRIDS - 1);
    case PG_QUANT * 4 + 1:
        return mode_index() * 256 / (MODES - 1);
    case PG_QUANT * 4 + 2:
        return zoom * 64;
    case PG_QUANT * 4 + 3:
        return rq_nnotes ? fixed_count() * 256 / rq_nnotes : 0;
    case PG_TAKE * 4 + 0:
        return p_track * 256 / 3;
    }
    return 0;
}

static int running(int st)
{
    return st >= PRE_SEEK;
}

static void draw_header(int st)
{
    float b = *TE_BPM;
    int x, i;

    gfx_rect(0, 0, 320, HDR_H, GFX_RGB(0x18, 0x18, 0x22));
    gfx_rect(2, 1, 19, 11, col[3]);             /* the take's track 1..4 */
    gfx_dec(8, 2, take_track + 1, GFX_BLACK, 2);
    x = gfx_text(25, 2, "SR", col[1], 2);
    if (st >= COM_SEEK)
        x = gfx_text(x + 8, 2, "REC", c_red, 2);
    else if (running(st))
        x = gfx_text(x + 8, 2, "PLAY", col[1], 2);
    else
        x = gfx_text(x + 8, 2, "STOP", c_dim, 2);
    x = barbeat_text(x + 8, 2, running(st) ? (uint32_t)(*TE_TAPE_POS >> 6)
                               : step_t(rq_step), GFX_WHITE, 2);
    if (b >= 40.0f && b <= 220.0f) {
        x = gfx_dec(x + 10, 2, (int32_t)(b + 0.5f), GFX_WHITE, 2);
        gfx_text(x + 2, 6, "BPM", c_dim, 1);
    }
    {
        char n[3] = { 'P', (char)('1' + rq_page), 0 };     /* "P1 NOTE" + page dots, as DRUM ROLL */

        x = 320 - 2 - 8 * 8 - PAGES * 4 - 4;
        for (i = 0; i < PAGES; i++)
            gfx_rect(x + i * 4, 5, 2, 3, i == rq_page ? GFX_WHITE : c_off);
        x = gfx_text(x + PAGES * 4 + 2, 2, n, GFX_WHITE, 2);
        gfx_text(x + 6, 2, page_name[rq_page], col[1], 2);
    }
}

static void draw_roll(int st)
{
    int i, y, span, rh, rows, top, cx0 = 0, cx1 = -1, cy = 0, h;
    uint64_t beat = step_of(1, 1);

    if (running(st)) {                          /* the view follows the tape while it runs */
        uint32_t pos = (uint32_t)(*TE_TAPE_POS >> 6);

        if (pos < view_x0 || pos >= view_x0 + view_w)
            view_x0 = pos > view_w / 8 ? pos - view_w / 8 : 0;
    }
    /* pitch rows: fit the take's range, else scroll with the cursor note */
    span = key_hi - key_lo + 3;
    rh = ROLL_H / span;
    rh = rh < 4 ? 4 : rh > 11 ? 11 : rh;        /* always a 1 px gap between rows */
    rows = ROLL_H / rh;
    if (span <= rows) {
        top = key_hi + 1 + (rows - span) / 2;
    } else {
        top = rq_pitch + rows / 2;
        top = top > key_hi + 1 ? key_hi + 1 : top < key_lo - 2 + rows ? key_lo - 2 + rows : top;
    }
    h = rh - 1;
    for (i = 0; i < rows; i++) {                /* rows: black keys darker, names in the column */
        int key = top - i, sem = ((key % 12) + 12) % 12;

        y = ROLL_Y + i * rh;
        if (key < 0 || key > 127)
            continue;
        if (sem == 1 || sem == 3 || sem == 6 || sem == 8 || sem == 10)
            gfx_rect(GRID_X, y, GRID_W, rh, c_black_key);
        if (key == rq_pitch)
            gfx_rect(0, y, GRID_X - 2, rh - 1, c_sel);
        if (sem == 0)
            gfx_rect(GRID_X, y + rh - 1, GRID_W, 1, c_beat);
        if (rh >= 7 || sem == 0 || key == rq_pitch)
            note_text(4, y + (rh - 5) / 2, key, sem == 0 ? GFX_WHITE : c_dim, 1);
    }
    gfx_rect(GRID_X - 2, ROLL_Y, 1, ROLL_H, c_beat);
    {                                           /* the cursor's grid step: its column lit */
        int a = xpos(step_t(rq_step)), b = xpos(step_t(rq_step + 1));

        if (clipx(&a, &b))
            gfx_rect(a, ROLL_Y, b - a, ROLL_H, c_sel);
    }
    /* grid (dim), beats, bars; the take's range in red */
    lines(grid_step(), 4, c_beat);
    lines(beat, 3, c_beat);
    lines(4 * beat, 3, c_bar);
    for (i = 0; i < 2; i++) {
        int x = xpos(i ? rq_rec_end : rq_rec_start);

        if (x >= GRID_X && x < GRID_X + GRID_W)
            gfx_rect(x, ROLL_Y, 1, ROLL_H, c_red);
    }
    /* notes: AUTO green, FIXED blue, selected orange (velocity = brightness), deleted outline */
    for (i = 0; i < rq_nnotes; i++) {
        const rq_note *t = &notes[i];
        int r = top - t->key, x0, x1;
        uint16_t c = t->flags & RN_SEL ? col[3] : t->flags & RN_FIXED ? col[0] : col[1];

        if (r < 0 || r >= rows)
            continue;
        x0 = xpos(t->start) + 1;
        x1 = xpos(t->start + t->len);
        if (x1 <= x0)
            x1 = x0 + 1;
        y = ROLL_Y + r * rh;
        if (i == rq_cur) {
            cx0 = x0;
            cx1 = x1;
            cy = y;
        }
        if (!clipx(&x0, &x1))
            continue;
        if (t->flags & RN_DEL) {                /* deleted: a thin line through its middle */
            gfx_rect(x0, y + h / 2, x1 - x0, 1, t->flags & RN_SEL ? col[3] : c_off);
            continue;
        }
        gfx_rect(x0, y, x1 - x0, h, shade(c, 45 + 55 * (t->data[0] & 127) / 127));
    }
    /* cursor: white frame round its note (none: round the empty place), the same size; a dim line
     * inside it where the note was played */
    if (rq_cur < 0 && top - rq_pitch >= 0 && top - rq_pitch < rows) {
        int a = xpos(step_t(rq_step)) + 1, b = xpos(step_t(rq_step + 1));

        if (clipx(&a, &b))                      /* as round a note: one cell, 1 px outside */
            frame_rect(a - 1, ROLL_Y + (top - rq_pitch) * rh - 1, b - a + 2, h + 2, GFX_WHITE);
    }
    if (cx1 >= 0 && rq_cur >= 0) {
        int a = cx0 - 1, b = cx1 + 1;

        if (clipx(&a, &b))
            frame_rect(a, cy - 1, b - a, h + 2, GFX_WHITE);
        if (notes[rq_cur].played != notes[rq_cur].start) {
            int xp = xpos(notes[rq_cur].played);

            if (xp >= GRID_X && xp < GRID_X + GRID_W)
                gfx_rect(xp, cy, 1, h, c_dim);  /* inside its own row */
        }
    }
    if (running(st)) {                          /* tape position while previewing / recording */
        int x = xpos((uint32_t)(*TE_TAPE_POS >> 6));

        if (x >= GRID_X && x < GRID_X + GRID_W)
            gfx_rect(x, ROLL_Y, 1, ROLL_H, GFX_WHITE);
    }
}

#define COL_W   78

static void draw_take_page(void)
{
    int x = 2 + COL_W + 6, y = PANEL_Y + 2, replace = p_track == take_track;

    gfx_rect(x - 4, y, 1, 38, c_beat);
    x = gfx_text(x, y, "COMMIT TO T", GFX_WHITE, 2);
    gfx_dec(x, y, p_track + 1, replace ? c_red : col[3], 2);
    x = 2 + COL_W + 6;
    gfx_text(x, y + 14, replace ? "REPLACES THE TAKE - DROP = ORIGINAL" : "KEEPS THE TAKE - ITS TRACK MUTED", c_dim, 1);
    gfx_text(x, y + 22, "RECORDS THE ROLL AS SHOWN", c_dim, 1);
    x = gfx_text(x, y + 30, "PUSH", c_dim, 1) + 4;
    x = gfx_text(x, y + 30, "ORANGE", col[3], 1) + 4;
    x = gfx_text(x, y + 30, "= COMMIT", c_dim, 1) + 8;
    x = gfx_text(x, y + 30, "WHITE", col[2], 1) + 4;
    gfx_text(x, y + 30, "= PREVIEW", c_dim, 1);
}

/* info line: the status while there is one, else the cursor note */
static void draw_info(int st)
{
    const rq_note *t = rq_cur >= 0 ? &notes[rq_cur] : 0;
    int x, y = PANEL_Y + 27, sel = 0, i;
    uint64_t step = grid_step();

    if (*status) {
        gfx_text(2, y + 4, status, running(st) ? c_red : GFX_WHITE, 1);
        return;
    }
    for (i = 0; i < rq_nnotes; i++)
        sel += (notes[i].flags & RN_SEL) != 0;
    if (!t) {
        x = note_text(2, y, rq_pitch, GFX_WHITE, 1);
        x = gfx_text(x + 6, y, "BAR", c_dim, 1);
        x = step_text(x + 4, y, rq_step, GFX_WHITE, 1);
        x = gfx_text(x + 6, y, "EMPTY - PUSH", c_dim, 1);
        x = gfx_text(x + 4, y, "BLUE", col[0], 1);
        x = gfx_text(x + 4, y, "FOR A NOTE", c_dim, 1);
    } else {
        x = note_text(2, y, t->key, GFX_WHITE, 1);
        x = gfx_text(x + 6, y, "BAR", c_dim, 1);
        x = barbeat_text(x + 4, y, t->start, GFX_WHITE, 1);
        if (step) {
            x = gfx_text(x + 6, y, t->start == rq_grid_nearest(step, t->start) ? "ON GRID" : "OFF GRID",
                         t->start == rq_grid_nearest(step, t->start) ? col[1] : c_dim, 1);
            x = gfx_text(x + 6, y, "LEN", c_dim, 1);
            x = steps_text(x + 4, y, t->len, GFX_WHITE, 1);
            x = gfx_text(x + 2, y, "X", c_dim, 1);
            x = gfx_text(x + 2, y, g_name[p_grid], GFX_WHITE, 1);
        }
        x = gfx_text(x + 6, y, t->flags & RN_DEL ? "DELETED" : t->flags & RN_FIXED ? "FIXED" : "AUTO",
                     t->flags & RN_DEL ? c_red : t->flags & RN_FIXED ? col[0] : col[1], 1);
    }
    if (sel) {
        x = gfx_text(x + 6, y, "SEL", col[3], 1);
        gfx_dec(x + 4, y, sel, col[3], 1);
    }
    if (rq_page == PG_QUANT) {
        gfx_text(2, y + 9, "AUTO NOTES FOLLOW GRID + MODE - ORANGE: ALL NOTES AUTO", c_dim, 1);
    } else {
        x = gfx_shift(2, y + 9, c_dim, 1);
        x = gfx_text(x, y + 9, rq_page == PG_NOTE ? "BLUE TURN: PITCH ROW - BLUE: NOTE ON / OFF - ORANGE: QUANTIZE"
                                                  : "BLUE TURN: PITCH ROW - NOTE: JUMP - ORANGE: QUANTIZE", c_dim, 1);
    }
}

static void draw_panel(int st)
{
    int i, y = PANEL_Y + 2, n = rq_page == PG_TAKE ? 1 : 4;

    for (i = 0; i < n; i++) {
        int x = 2 + i * COL_W, bip, v = fn_frac(i, &bip), bw = COL_W - 8;

        gfx_text(x, y, fn_name[rq_page][i], col[i], 1);
        fn_text(i, x, y + 7, GFX_WHITE);
        gfx_rect(x, y + 19, bw, 2, c_beat);
        if (bip) {
            int m = x + bw / 2, e = x + v * bw / 256;

            gfx_rect(e < m ? e : m, y + 19, (e < m ? m - e : e - m) + 1, 2, col[i]);
        } else {
            gfx_rect(x, y + 19, v * bw / 256, 2, col[i]);
        }
    }
    if (rq_page == PG_TAKE) {
        draw_take_page();
        if (*status)
            gfx_text(2, PANEL_Y + 33, status, running(st) ? c_red : GFX_WHITE, 1);
    } else {
        draw_info(st);
    }
}

static void draw_hints(void)
{
    int x = 2, y = gfx_h - 7;

    x = gfx_text(x, y, "PUSH", c_dim, 1) + 4;
    x = gfx_text(x, y, "NOTE", col[0], 1) + 4;
    x = gfx_text(x, y, "PAGE", col[1], 1) + 4;
    x = gfx_text(x, y, "PLAY", col[2], 1) + 4;
    x = gfx_text(x, y, rq_page == PG_TAKE ? "COMMIT" : rq_page == PG_QUANT ? "AUTO" : "QUANT", col[3], 1) + 8;
    x = gfx_shift(x, y, c_dim, 1);
    x = gfx_text(x, y, "TURN", c_dim, 1) + 4;
    x = gfx_text(x, y, "PITCH", col[0], 1) + 8;
    x = gfx_shift(x, y, c_dim, 1);
    x = gfx_text(x, y, "PUSH", c_dim, 1) + 4;
    x = gfx_text(x, y, "EXIT", col[0], 1) + 4;
    x = gfx_text(x, y, "ALL", col[1], 1) + 4;
    x = gfx_text(x, y, "SEL", col[2], 1) + 4;
    gfx_text(x, y, "PLAYED", col[3], 1);
}

void rq_frame(uint16_t *buf)
{
    int st = rq_state;

    if (st == IDLE)
        return;
    gfx_begin(buf);
    if (st == TAKE) {
        gfx_rect(0, 0, 50, 14, GFX_BLACK);
        gfx_disc(7, 7, 4, *TE_TAPE_STATE == 1 ? c_red : c_dim);
        gfx_text(15, 2, "TAKE", GFX_WHITE, 2);
        return;
    }
    if (*TE_EDIT_MODE != EDIT_TAPE)
        return;
    gfx_rect(0, 0, 320, gfx_h, c_bg);
    draw_header(st);
    draw_roll(st);
    gfx_rect(0, PANEL_Y - 1, 320, 1, c_beat);
    draw_panel(st);
    draw_hints();
}
