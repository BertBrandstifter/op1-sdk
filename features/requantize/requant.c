/* Requantize a tape take (spec: README.md), v2.
 *
 * shift + blue press start a take: every note played while the tape records is logged with its
 *                    tape position, and the range TE records is remembered
 * shift + blue press end the take -> requantize screen (tape screen, v3.1): the piano roll with
 *                    the take settings in its header. Green press flips the knobs between NOTES
 *                    (default: edit notes, see below) and TAKE: blue knob grid, green knob
 *                    SNAP/QUEUE/STRICT, white knob target track (default: the take's own track =
 *                    replace), orange knob zoom, click orange = commit, shift + click orange = every
 *                    note back to AUTO. Click white = preview in both (tape plays the take with its
 *                    track muted, quantized notes live); shift + blue = exit
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
static volatile int p_grid = GRID_DEFAULT, p_mode = RQ_QUEUE, p_track;
static const char *const mode_name[] = { "SNAP", "QUEUE", "STRICT" };
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
volatile int rq_take_focus, rq_nnotes, rq_cur;  /* global: the bench reads them; knobs on the
                                                 * take settings (1) or the notes (0) */
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

    rq_nout = step ? rq_render(notes, rq_nnotes, work, out, OUT_MAX, &scratch, step, p_mode,
                               WINDOW, CHORD)
                   : 0;
}

/* time order, pitch range and the shown range after a change: the cursor note stays on screen */
static void follow(void)
{
    uint64_t bar = 4 * step_of(1, 1);
    uint32_t w, s;
    int i, lo = 127, hi = 0;

    rq_order(notes, rq_nnotes, ord);
    for (i = 0; i < rq_nnotes; i++) {
        lo = notes[i].key < lo ? notes[i].key : lo;
        hi = notes[i].key > hi ? notes[i].key : hi;
    }
    key_lo = lo;
    key_hi = hi < lo ? lo : hi;
    if (!bar || !rq_nnotes)
        return;
    w = (uint32_t)(((bar << zoom) + 0x8000) >> 16);
    s = notes[rq_cur].start;
    if (w != view_w || s < view_x0 + w / 10 || s > view_x0 + w - w / 10)
        view_x0 = s > w / 4 ? s - w / 4 : 0;
    view_w = w;
    view_scale = ((int64_t)320 << 24) / w;
}

/* cursor to the previous / next note in time order */
static void navigate(int d)
{
    int p;

    for (p = 0; p < rq_nnotes && ord[p] != rq_cur; p++)
        ;
    p += d;
    rq_cur = ord[p < 0 ? 0 : p >= rq_nnotes ? rq_nnotes - 1 : p];
}

static void edit(int op, int32_t amount)
{
    if (rq_edit(notes, rq_nnotes, rq_cur, op, amount, grid_step())) {
        render();
        follow();
    }
}

/* open: the smallest zoom that shows the whole take, starting just before it; knobs on the notes
 * (on the settings when the take has none) */
static void open_roll(void)
{
    uint64_t bar = 4 * step_of(1, 1);
    uint32_t len = rq_rec_end - rq_rec_start, w;

    render();
    for (zoom = 0; bar && zoom < 4 && ((bar << zoom) >> 16) < len + len / 8; zoom++)
        ;
    w = bar ? (uint32_t)(((bar << zoom) + 0x8000) >> 16) : 1;
    view_x0 = rq_rec_start > w / 16 ? rq_rec_start - w / 16 : 0;
    view_w = w;
    rq_take_focus = !rq_nnotes;
    follow();
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

/* NOTES knobs, press (k: 0 blue .. 3 orange) */
static void roll_key(int k, int shift)
{
    status = "";
    if (k == 0)
        edit(RE_SELECT, 0);
    else if (k == 1 && shift)
        edit(RE_SELECT_ALL, 0);
    else if (k == 1)
        rq_take_focus = 1;                      /* knobs to the take settings */
    else if (k == 2 && shift)
        edit(RE_DELETE, 0);
    else if (k == 2)
        start(0);                               /* preview, as in the panel */
    else if (shift)
        edit(RE_PLAYED, 0);                     /* as played <-> AUTO */
    else
        edit(RE_SNAP, 0);                       /* quantize the targets */
}

/* TAKE knobs, press: green back to the notes, white preview, orange commit / shift: all AUTO */
static void take_key(int k, int shift)
{
    status = "";
    if (k == 1 && !shift && rq_nnotes)
        rq_take_focus = 0;
    else if (k == 2 && !shift)
        start(0);
    else if (k == 3 && shift)
        edit(RE_AUTO_ALL, 0);                   /* the whole take follows grid + mode again */
    else if (k == 3)
        start(1);
}

/* TAKE knobs, turn (shift the same): grid, mode, target track, zoom */
static void take_knob(int knob, int d)
{
    status = "";
    if (knob == 0)
        p_grid = p_grid + d < 0 ? 0 : p_grid + d >= GRIDS ? GRIDS - 1 : p_grid + d;
    else if (knob == 1)
        p_mode = p_mode + d < RQ_SNAP ? RQ_SNAP : p_mode + d > RQ_STRICT ? RQ_STRICT : p_mode + d;
    else if (knob == 2)
        p_track = (p_track + d) & 3;
    else
        zoom = zoom + d < 0 ? 0 : zoom + d > 4 ? 4 : zoom + d;
    if (knob != 2) {
        render();                               /* AUTO notes move with grid and mode */
        follow();
    }
}

/* NOTES knobs, turn */
static void roll_knob(int knob, int d, int shift)
{
    status = "";
    if (knob == 0 && shift) {
        zoom = zoom + d < 0 ? 0 : zoom + d > 4 ? 4 : zoom + d;
        follow();
    } else if (knob == 0) {
        navigate(d);
        follow();
    } else if (knob == 1) {
        edit(shift ? RE_MOVE_STEP : RE_MOVE, shift ? d : d * RN_FINE);
    } else if (knob == 2) {
        edit(RE_PITCH, shift ? 12 * d : d);
    } else {
        edit(shift ? RE_LEN_STEP : RE_LEN, shift ? d : d * RN_FINE);
    }
}

int rq_key(int key, int down)
{
    int st = rq_state, k = key - 59;

    if (key == KEY_TRIGGER && *TE_SHIFT_HELD) {
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
        consumed |= 1;
        return 1;
    }
    if (key == KEY_TRIGGER && !down && (consumed & 1)) {
        consumed &= ~1;
        return 1;
    }
    /* requantize screen and running preview/commit: knob clicks on the tape screen are ours */
    if (k < 0 || k > 3 || st < PANEL || *TE_EDIT_MODE != EDIT_TAPE)
        return 0;
    if (down && st == PANEL && rq_take_focus)
        take_key(k, *TE_SHIFT_HELD);
    else if (down && st == PANEL)
        roll_key(k, *TE_SHIFT_HELD);
    else if (down && (st == PRE_SEEK || st == PRE_START || st == PRE_PLAY) && k == 2)
        abort_run();                            /* white again: stop the preview */
    return 1;
}

int rq_knob(int knob, int16_t *diff)
{
    int d = *diff > 0 ? 1 : *diff < 0 ? -1 : 0;

    if (rq_state != PANEL || *TE_EDIT_MODE != EDIT_TAPE)
        return 0;
    if (d && rq_take_focus)
        take_knob(knob, d);
    else if (d)
        roll_knob(knob, d, *TE_SHIFT_HELD);
    return 1;                                   /* all four knobs, with and without shift */
}

/* ------------------------------------------------------------------ GUI thread */

#define AREA_Y      28                          /* roll area: below two lines of text */
#define AREA_H      118

static const uint16_t c_red = GFX_RGB(0xff, 0x3a, 0x5d), c_grey = GFX_RGB(0x7c, 0x79, 0x90),
                      c_blue = GFX_RGB(0x69, 0x8e, 0xff), c_green = GFX_RGB(0x00, 0xed, 0x95),
                      c_orange = GFX_RGB(0xff, 0x8a, 0x00), c_yellow = GFX_RGB(0xff, 0xe0, 0x40);
static const char *const key_name[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#",
                                        "B" };

/* screen x of a tape position (clamped far outside the screen) */
static int xpos(uint32_t pos)
{
    int64_t x = (((int64_t)pos - (int64_t)view_x0) * view_scale) >> 24;

    return x < -1000 ? -1000 : x > 1000 ? 1000 : (int)x;
}

/* vertical lines every step (16.16) across the shown range, if at least min_px apart */
static void lines(uint64_t step, int min_px, uint16_t c)
{
    uint32_t k, p, end = view_x0 + view_w;

    if (!step || (((int64_t)(step >> 16) * view_scale) >> 24) < min_px)
        return;
    for (k = rq_grid_floor(step, view_x0); (p = rq_grid_pt(step, k)) <= end; k++)
        if (p >= view_x0)
            gfx_rect(xpos(p), AREA_Y, 1, AREA_H, c);
}

/* "+12MS" / "-3MS" / "ON GRID": the cursor note's offset to the nearest grid point */
static int offset_text(int x, int y, uint32_t pos, uint16_t c)
{
    uint64_t step = grid_step();
    int32_t off;

    if (!step)
        return x;
    off = (int32_t)(pos - rq_grid_nearest(step, pos));
    if (!off)
        return gfx_text(x, y, "ON GRID", c, 2);
    x = gfx_text(x, y, off > 0 ? "+" : "-", c, 2);
    x = gfx_dec(x, y, ((off < 0 ? -off : off) * 10 + 220) / 441, c, 2);
    return gfx_text(x, y, "MS", c, 2);
}

/* line 2: the cursor note (pitch, bar.beat, offset to the grid, length, timing state) */
static void cursor_text(int y)
{
    const rq_note *t = &notes[rq_cur];
    uint64_t beat = step_of(1, 1);
    uint32_t b;
    int x;

    x = gfx_text(2, y, key_name[t->key % 12], GFX_WHITE, 2);
    x = gfx_dec(x, y, t->key / 12 - 1, GFX_WHITE, 2);
    if (beat) {
        b = rq_grid_floor(beat, t->start);
        x = gfx_dec(x + 8, y, (int32_t)(b / 4 + 1), GFX_WHITE, 2);
        x = gfx_text(x, y, ".", GFX_WHITE, 2);
        x = gfx_dec(x, y, (int32_t)(b % 4 + 1), GFX_WHITE, 2);
    }
    x = offset_text(x + 8, y, t->start, c_yellow);
    x = gfx_text(x + 8, y, "L", c_grey, 2);
    x = gfx_dec(x, y, (int32_t)(t->len * 10 / 441), GFX_WHITE, 2);
    x = gfx_text(x, y, "MS", GFX_WHITE, 2);
    gfx_text(x + 8, y, t->flags & RN_DEL ? "DEL" : t->flags & RN_FIXED ? "FIX" : "AUTO",
             t->flags & RN_DEL ? c_grey : t->flags & RN_FIXED ? c_blue : c_green, 2);
}

static void draw_roll(int st)
{
    int i, x, y, p, sel = 0, span, rh, rows, top, cx1 = 0, cx2 = -1, cy = 0;
    uint64_t beat = step_of(1, 1);

    gfx_rect(0, 0, 320, gfx_h, GFX_BLACK);
    for (i = p = 0; i < rq_nnotes; i++) {
        sel += (notes[i].flags & RN_SEL) != 0;
        if (ord[i] == rq_cur)
            p = i;
    }
    /* line 1: take settings (lit while the knobs are on them), cursor position, selection */
    y = 2;
    if (rq_take_focus)
        gfx_rect(0, 0, 320, 13, GFX_RGB(0x30, 0x30, 0x48));
    x = gfx_text(2, y, g_name[p_grid], c_blue, 2);
    x = gfx_text(x + 8, y, mode_name[p_mode], c_green, 2);
    x = gfx_text(x + 8, y, "T", GFX_WHITE, 2);
    x = gfx_dec(x, y, p_track + 1, p_track == take_track ? c_red : GFX_WHITE, 2);
    if (p_track == take_track)
        x = gfx_text(x + 8, y, "REPLACE", c_red, 2);
    if (rq_nnotes) {
        x = gfx_dec(x + 8, y, p + 1, GFX_WHITE, 2);
        x = gfx_text(x, y, "/", c_grey, 2);
        x = gfx_dec(x, y, rq_nnotes, GFX_WHITE, 2);
    }
    if (sel) {
        x = gfx_text(x + 8, y, "SEL ", c_orange, 2);
        gfx_dec(x, y, sel, c_orange, 2);
    }
    /* line 2: status while there is one, else the cursor note */
    y += 12;
    if (*status)
        gfx_text(2, y, status, st >= PRE_SEEK ? GFX_RGB(0xff, 0x42, 0x3f) : GFX_WHITE, 2);
    else if (rq_nnotes)
        cursor_text(y);
    /* grid (dim), beats, bars (bright); the take's range in red */
    lines(grid_step(), 4, GFX_RGB(0x20, 0x20, 0x2c));
    lines(beat, 3, GFX_RGB(0x40, 0x40, 0x50));
    lines(4 * beat, 3, c_grey);
    for (i = 0; i < 2; i++) {
        x = xpos(i ? rq_rec_end : rq_rec_start);
        gfx_rect(x, AREA_Y, 1, AREA_H, c_red);
    }
    /* pitch rows: fit the take's range, else scroll with the cursor note */
    span = key_hi - key_lo + 3;
    rh = AREA_H / span;
    rh = rh < 2 ? 2 : rh > 10 ? 10 : rh;
    rows = AREA_H / rh;
    if (span <= rows) {
        top = key_hi + 1 + (rows - span) / 2;
    } else {
        top = notes[rq_cur].key + rows / 2;
        top = top > key_hi + 1 ? key_hi + 1 : top < key_lo - 2 + rows ? key_lo - 2 + rows : top;
    }
    /* notes: AUTO green, FIXED blue, selected orange, deleted grey outline */
    for (i = 0; i < rq_nnotes; i++) {
        const rq_note *t = &notes[i];
        int r = top - t->key, x1, x2, h = rh > 2 ? rh - 1 : rh;

        if (r < 0 || r >= rows)
            continue;
        x1 = xpos(t->start);
        x2 = xpos(t->start + t->len);
        if (x2 < 0 || x1 >= 320)
            continue;
        x1 = x1 < 0 ? 0 : x1;
        x2 = x2 > 319 ? 319 : x2 > x1 ? x2 : x1 + 1;
        y = AREA_Y + r * rh;
        if (i == rq_cur) {
            cx1 = x1;
            cx2 = x2;
            cy = y;
        }
        if (t->flags & RN_DEL) {
            gfx_rect(x1, y, x2 - x1, 1, t->flags & RN_SEL ? c_orange : c_grey);
            gfx_rect(x1, y + h - 1, x2 - x1, 1, t->flags & RN_SEL ? c_orange : c_grey);
            continue;
        }
        gfx_rect(x1, y, x2 - x1, h, t->flags & RN_SEL ? c_orange : t->flags & RN_FIXED ? c_blue : c_green);
    }
    /* cursor: white frame; yellow tick where it was played */
    if (cx2 >= 0) {
        int h = rh > 2 ? rh - 1 : rh;

        gfx_rect(cx1 - 1, cy - 1, cx2 - cx1 + 2, 1, GFX_WHITE);
        gfx_rect(cx1 - 1, cy + h, cx2 - cx1 + 2, 1, GFX_WHITE);
        gfx_rect(cx1 - 1, cy - 1, 1, h + 2, GFX_WHITE);
        gfx_rect(cx2, cy - 1, 1, h + 2, GFX_WHITE);
        if (notes[rq_cur].played != notes[rq_cur].start)
            gfx_rect(xpos(notes[rq_cur].played), cy - 3, 1, h + 6, c_yellow);
    }
    if (st >= PRE_SEEK)                         /* tape position while previewing / recording */
        gfx_rect(xpos((uint32_t)(*TE_TAPE_POS >> 6)), AREA_Y, 1, AREA_H, GFX_WHITE);
    /* hint line, scale 1 */
    gfx_text(2, AREA_Y + AREA_H + 3, rq_take_focus
             ? (*TE_SHIFT_HELD ? "SHIFT: BLUE EXIT  ORANGE ALL NOTES AUTO (QUANTIZE WHOLE TAKE)"
                               : "TAKE: BLUE GRID  GREEN MODE/NOTES  WHITE TRACK/PLAY  ORANGE ZOOM/COMMIT")
             : (*TE_SHIFT_HELD ? "SHIFT: ZOOM/EXIT  STEP/ALL  OCTAVE/DELETE  STEP/PLAYED-AUTO"
                               : "BLUE NOTE/SEL  GREEN TIME/TAKE  WHITE PITCH/PLAY  ORANGE LEN/QUANT"),
             c_grey, 1);
}

void rq_frame(uint16_t *buf)
{
    static const uint16_t red = GFX_RGB(0xff, 0x3a, 0x5d), grey = GFX_RGB(0x7c, 0x79, 0x90);
    int st = rq_state;

    if (st == IDLE)
        return;
    gfx_begin(buf);
    if (st == TAKE) {
        gfx_rect(0, 0, 50, 14, GFX_BLACK);
        gfx_disc(7, 7, 4, *TE_TAPE_STATE == 1 ? red : grey);
        gfx_text(15, 2, "TAKE", GFX_WHITE, 2);
        return;
    }
    if (*TE_EDIT_MODE != EDIT_TAPE)
        return;
    draw_roll(st);
}
