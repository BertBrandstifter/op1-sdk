/* py_view: PLAY's song view and chain view (new_pattern_tape.md 2.3, README.md).
 *
 *   song view   one column per track of the list, 128 rows (7 visible): a SYNTH / DRUM cell is a
 *               chain 00..7F or OF (a rest: one silent bar), an AUDIO cell ON / OF. The cell every column plays now is filled in
 *               the track's colour, the cursor is a white frame.
 *   render      rec, then rec again: the song from the cursor row into an AIFF file (py_render.c);
 *               white turn sets the length in bars first.
 *   live        shift + rec in the song view: the cells are not edited; play cues the cursor row on every
 *               column, white press the cursor cell on its column: a column jumps to its cued row
 *               when the chain it plays ends (an AUDIO column: its ON / OF segment) and loops back
 *               there from then on; an empty cued cell stops the column. Shift + play cues a row in
 *               the plain song view too.
 *   help        speech: a page per view (list, song, chain, live), blue turn pages, any key closes.
 *   chain view  the 16 rows (pattern 1..14 + transpose) of the chain opened from a song cell, in
 *               two blocks of 8; play auditions the chain alone on its track.
 * The song itself and every edit that moves rows is py_song.c (host-tested); playing is
 * py_pattern.c (heads) and py_core.c (song-driven AUDIO tracks). Keys and knobs arrive from
 * play.c (KeyHandler thread), drawing runs in the GUI thread; cursors, the clipboards and the
 * last values are session-only. */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "py_core.h"
#include "py_song.h"
#include "play.h"
#include "py_ui.h"
#include "py_slot.h"

#define CROW_Y(r)   (41 + (r) * 10)

/* song view */
static volatile uint8_t s_row, s_col, s_top;
static uint8_t last_chain[PS_TRACKS];           /* per track: the last chain set there + 1, 0 = none */
static uint8_t clip_kind, clip_val;             /* song cell clipboard: 0 none, 1 a chain, 2 ON / OF */
static volatile uint8_t del_armed;              /* shift + cut pressed once (either view) */
static volatile uint8_t live;                   /* the song view is in live mode */
static volatile uint8_t rd_ask;                 /* rec pressed once: the render's bars are asked */
static volatile uint16_t rd_bars;
static volatile uint8_t help;                   /* the help page shown + 1, 0 = none */

/* songs page */
#define SL_VIS      8                           /* slots on screen */
static volatile uint8_t sl_cur, sl_top;
static volatile uint8_t sl_armed;               /* PY_SL_SAVE / LOAD / DELETE pressed once on sl_cur */
static volatile uint8_t sl_busy;                /* the main thread runs this PY_SL_* operation */
static psl_info sl_info[PSL_SLOTS];             /* main writes (scan), GUI reads */
static volatile int8_t sl_state[PSL_SLOTS] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };   /* PSL_*, 1 = not read */
static char sl_msg[64];
/* the load question: patterns that differ from the ones now, each the slot's or kept */
static volatile uint8_t sl_ask;                 /* 1 = shown, for slot sl_ask_n */
static uint8_t sl_ask_n, sl_nlist, sl_acur, sl_atop;
static uint8_t sl_list[2 * PSL_PATS + PSL_SOUNDS];  /* kind << 4 | pattern (kind 2: a sound slot) */
static uint16_t sl_used[2];
static uint8_t sl_bad;                          /* sounds that cannot come back (kept) */
static volatile uint16_t sl_keep[3];            /* bit p: pattern / sound p stays as it is now */

/* files page: the render folder (play.c: py_rfiles) */
static volatile uint8_t rf_cur, rf_top, rf_armed;   /* rf_armed: cut pressed once on rf_cur */

/* chain view */
static volatile uint8_t c_track, c_chain, c_row, c_col;
static uint8_t last_pat;                        /* the last pattern set + 1, 0 = none */
static ps_crow crow_clip;
static uint8_t crow_clip_ok;

static uint16_t colour(int type)
{
    return type == PS_AUDIO ? C_AMBER : type == PS_SYNTH ? C_BLUE : C_RED;
}

/* the track of the cursor column (-1: no tracks), the cursor kept inside the list */
static int cursor_track(int *list, int *n)
{
    *n = py_rows(list);
    if (!*n)
        return -1;
    if (s_col >= *n)
        s_col = (uint8_t)(*n - 1);
    return list[s_col];
}

static void leave_chain(void);

/* join (the list, the chain view, another screen): the song view, as it was (live mode stays) */
void py_v_enter(void)
{
    if (py_view == PY_V_CHAIN)
        leave_chain();                          /* (ends an audition) */
    del_armed = 0;
    rd_ask = 0;
    help = 0;
    sl_armed = 0;
    sl_ask = 0;
    rf_armed = 0;
    py_view = PY_V_SONG;
}

/* the tape key: the track list */
void py_v_list(void)
{
    if (py_view == PY_V_CHAIN)
        leave_chain();
    del_armed = 0;
    rd_ask = 0;
    help = 0;
    sl_armed = 0;
    sl_ask = 0;
    rf_armed = 0;
    py_view = PY_V_LIST;
}

static void toggle_mute(int t)
{
    py_song.trk[t].mute ^= 1;
    py_changed(0);
}

static void toggle_solo(int t)
{
    py_song.trk[t].solo ^= 1;
    py_changed(0);
}

/* ------------------------------------------------------------------ song view: keys + knobs */

static void set_chain(int t, int c)
{
    py_song.cell[s_row][t] = (uint8_t)c;
    last_chain[t] = (uint8_t)(c + 1);
    py_changed(0);
}

static void open_chain(int t, int c)
{
    c_track = (uint8_t)t;
    c_chain = (uint8_t)c;
    c_row = c_col = 0;
    del_armed = 0;
    py_view = PY_V_CHAIN;
}

/* cue the cursor row on every column (stopped: play starts there) */
static void cue_row(const int *list, int n)
{
    int i;

    if (!py_mx.playing) {
        py_req_start(s_row, -1, 0);
        return;
    }
    for (i = 0; i < n; i++)
        py_req_cue(list[i], s_row);
}

/* live mode: the keys that differ from the song view's; 1 = handled */
static int live_key(int key, int shift, const int *list, int n, int t)
{
    switch (key) {
    case TE_KEY_PLAY:                           /* the cursor row on every column */
        cue_row(list, n);
        return 1;
    case TE_KEY_WHITE_PUSH:                     /* the cursor cell on its column */
        if (t < 0)
            return 1;
        if (!py_mx.playing)
            py_req_start(s_row, -1, 0);
        else
            py_req_cue(t, s_row);
        return 1;
    case TE_KEY_T1: case TE_KEY_T2: case TE_KEY_T3: case TE_KEY_T4:     /* mute; shift: solo */
        if (key - TE_KEY_T1 < n) {
            if (shift)
                toggle_solo(list[key - TE_KEY_T1]);
            else
                toggle_mute(list[key - TE_KEY_T1]);
        }
        return 1;
    case TE_KEY_CUT:
    case TE_KEY_INSERT:
        py_note("LIVE: SHIFT + REC TO EDIT");
        return 1;
    default:
        return 0;
    }
}

/* a note with the loop section as it is now */
static void loop_note(void)
{
    static char msg[40];
    static const char hex[] = "0123456789ABCDEF";
    int a, b, n = py_song_loop(&py_song, &a, &b), i = 0;
    const char *p;

    for (p = n ? "LOOP " : "LOOP OFF ("; *p; p++)
        msg[i++] = *p;
    msg[i++] = hex[a >> 4]; msg[i++] = hex[a & 15];
    msg[i++] = '-';
    msg[i++] = hex[b >> 4]; msg[i++] = hex[b & 15];
    if (n) {
        msg[i++] = ' ';
        msg[i++] = 'X';
        if (n >= 10)
            msg[i++] = '1';
        msg[i++] = (char)('0' + n % 10);
    } else {
        msg[i++] = ')';
    }
    msg[i] = 0;
    py_note(msg);
}

static void song_key(int key, int shift)
{
    int list[PS_TRACKS], n, t = cursor_track(list, &n);
    int audio = t >= 0 && py_song.trk[t].type == PS_AUDIO;
    uint8_t *cell = t >= 0 ? &py_song.cell[s_row][t] : 0;

    if (!(key == TE_KEY_CUT && shift))
        del_armed = 0;
    if (rd_ask) {                               /* rec again: render; any other key: no */
        rd_ask = 0;
        if (key == TE_KEY_REC && !shift)
            py_rd_request(s_row, rd_bars);
        return;
    }
    if (key == TE_KEY_REC) {                    /* render the song from this row; shift: live mode */
        if (shift) {
            live ^= 1;
        } else {
            rd_bars = (uint16_t)py_song_bars(s_row);
            rd_ask = 1;
        }
        return;
    }
    if (live && live_key(key, shift, list, n, t))
        return;
    switch (key) {
    case TE_KEY_JOIN:                           /* join always means the song view: stay; shift: the songs page */
        if (shift)
            py_v_slots();
        break;
    case TE_KEY_ORANGE_PUSH:                    /* v0.17: nothing (tape: the list; chain view: back) */
        break;
    case TE_KEY_PLAY:                           /* from the cursor row, bar 1; playing: pause */
        if (shift)
            cue_row(list, n);                   /* shift: cue the row (live, without the mode) */
        else if (py_mx.playing)
            py_req_pause();
        else
            py_req_start(s_row, -1, 0);
        break;
    case TE_KEY_STOP:
        py_req_stop();
        break;
    case TE_KEY_BACK:
    case TE_KEY_FORWARD:
        if (shift) {                            /* the loop section: its first / last row here */
            int a, b, ln = py_song_loop(&py_song, &a, &b);

            if (!ln) {
                a = b = s_row;
                ln = 2;                         /* a new loop: twice */
            }
            if (key == TE_KEY_BACK)
                a = s_row, b = b < a ? a : b;
            else
                b = s_row, a = a > b ? b : a;
            py_song_set_loop(&py_song, a, b, ln);
            loop_note();
            py_changed(2);
        } else {
            py_req_bar(key == TE_KEY_BACK ? -1 : 1);
        }
        break;
    case TE_KEY_T1: case TE_KEY_T2: case TE_KEY_T3: case TE_KEY_T4:
        if (key - TE_KEY_T1 < n) {
            if (shift)
                toggle_mute(list[key - TE_KEY_T1]);
            else
                s_col = (uint8_t)(key - TE_KEY_T1);
        }
        break;
    case TE_KEY_BLUE_PUSH:
        if (t >= 0)
            toggle_mute(t);
        break;
    case TE_KEY_GREEN_PUSH:
        if (t >= 0)
            toggle_solo(t);
        break;
    case TE_KEY_WHITE_PUSH:
        if (t < 0)
            break;
        if (audio) {                            /* ON <-> OF (empty: ON) */
            if (!shift) {
                *cell = (uint8_t)(*cell == PS_ON ? PS_OF : PS_ON);
                py_changed(2);
            }
        } else if (*cell < PS_CHAINS) {         /* a chain: open it; shift: clone it */
            if (!shift) {
                open_chain(t, *cell);
            } else {
                int c = py_song_clone_chain(&py_song, *cell);

                if (c < 0)
                    py_note("NO FREE CHAIN");
                else
                    set_chain(t, c);
            }
        } else {                                /* empty: the last chain here; shift (or none yet): a new one */
            int c = !shift && last_chain[t] ? last_chain[t] - 1 : py_song_new_chain(&py_song);

            if (c < 0)
                py_note("NO FREE CHAIN");
            else
                set_chain(t, c);
        }
        break;
    case TE_KEY_CUT:
        if (shift) {                            /* twice: delete the song row */
            if (del_armed) {
                del_armed = 0;
                py_song_row_delete(&py_song, s_row);
                py_changed(2);
            } else {
                del_armed = 1;
            }
        } else if (t >= 0 && *cell != PS_EMPTY) {
            clip_kind = (uint8_t)(audio ? 2 : 1);
            clip_val = *cell;
            *cell = PS_EMPTY;
            py_changed(audio ? 2 : 0);
        }
        break;
    case TE_KEY_INSERT:
        if (shift) {                            /* a new empty song row here */
            if (py_song_row_insert(&py_song, s_row) < 0)
                py_note("ROW 7F IS USED");
            else
                py_changed(2);
        } else if (t >= 0) {
            if (!clip_kind) {
                py_note("CLIPBOARD EMPTY");
            } else if ((clip_kind == 2) != audio) {
                py_note("NOT FOR THIS TRACK TYPE");
            } else {
                *cell = clip_val;
                if (!audio)
                    last_chain[t] = (uint8_t)(clip_val + 1);
                py_changed(audio ? 2 : 0);
            }
        }
        break;
    default:                                    /* keys 1-8: the cursor track's sound / kit */
        if (t >= 0 && py_sound_key(key) >= 0)
            py_set_sound(t, py_sound_key(key));
        break;
    }
}

static void song_knob(int knob, int d, int shift)
{
    int list[PS_TRACKS], n, t = cursor_track(list, &n), v;

    del_armed = 0;
    if (rd_ask) {                               /* the render's length: white turn (shift: 8 bars) */
        if (knob == 2) {
            v = rd_bars + d * (shift ? 8 : 1);
            rd_bars = (uint16_t)(v < 1 ? 1 : v > 999 ? 999 : v);
        }
        return;
    }
    switch (knob) {
    case 0:                                     /* blue: row (shift: 16) */
        v = s_row + d * (shift ? 16 : 1);
        s_row = (uint8_t)(v < 0 ? 0 : v >= PS_ROWS ? PS_ROWS - 1 : v);
        break;
    case 1:                                     /* green: column */
        v = s_col + d;
        s_col = (uint8_t)(v < 0 || !n ? 0 : v >= n ? n - 1 : v);
        break;
    case 2:                                     /* white: the cell */
        if (t < 0)
            break;
        if (live) {
            py_note("LIVE: SHIFT + REC TO EDIT");
            break;
        }
        if (py_song.trk[t].type == PS_AUDIO) {  /* empty, ON, OF */
            uint8_t *cell = &py_song.cell[s_row][t], old = *cell;

            v = (old == PS_EMPTY ? 0 : old == PS_ON ? 1 : 2) + d;
            v = v < 0 ? 0 : v > 2 ? 2 : v;
            *cell = (uint8_t)(v == 0 ? PS_EMPTY : v == 1 ? PS_ON : PS_OF);
            if (*cell != old)
                py_changed(2);
        } else {                                /* empty, OF (a rest), 00..7F (shift: 16) */
            uint8_t c = py_song.cell[s_row][t];

            v = (c < PS_CHAINS ? c : c == PS_REST ? -1 : -2) + d * (shift ? 16 : 1);
            v = v < -2 ? -2 : v >= PS_CHAINS ? PS_CHAINS - 1 : v;
            if (v < 0) {
                py_song.cell[s_row][t] = (uint8_t)(v == -1 ? PS_REST : PS_EMPTY);
                py_changed(0);
            } else {
                set_chain(t, v);
            }
        }
        break;
    default:                                    /* orange: the loop's times (X1 = off); shift = master */
        if (shift) {
            py_master_step(d);
        } else {
            int a, b, ln = py_song_loop(&py_song, &a, &b);

            if (!ln && !a && !b) {
                py_note("SHIFT + < / >: LOOP START / END");
                break;
            }
            v = (ln ? ln : 1) + d;
            v = v < 1 ? 1 : v > PS_LOOP_MAX ? PS_LOOP_MAX : v;
            if (v != (ln ? ln : 1)) {
                py_song_set_loop(&py_song, a, b, v);
                loop_note();
                py_changed(2);
            }
        }
        break;
    }
}

/* ------------------------------------------------------------------ chain view: keys + knobs */

static void leave_chain(void)
{
    if (py_pat_audition_track() >= 0)
        py_req_stop();                          /* the audition ends with the view */
    del_armed = 0;
    py_view = PY_V_SONG;
}

static void chain_key(int key, int shift)
{
    ps_crow *row = &py_song.chain[c_chain][c_row];

    if (!(key == TE_KEY_CUT && shift))
        del_armed = 0;
    switch (key) {
    case TE_KEY_JOIN:
    case TE_KEY_ORANGE_PUSH:
        leave_chain();
        break;
    case TE_KEY_PLAY:                           /* this chain alone on its track, from its top */
        py_req_start(0, c_track, c_chain);
        break;
    case TE_KEY_STOP:
        py_req_stop();
        break;
    case TE_KEY_BLUE_PUSH:
        toggle_mute(c_track);
        break;
    case TE_KEY_GREEN_PUSH:
        toggle_solo(c_track);
        break;
    case TE_KEY_WHITE_PUSH:                     /* an empty row: the last pattern used here */
        if (row->pat >= PS_PATTERNS) {
            row->pat = (uint8_t)(last_pat ? last_pat - 1 : 0);
            last_pat = (uint8_t)(row->pat + 1);
            py_changed(0);
        }
        break;
    case TE_KEY_CUT:
        if (shift) {                            /* twice: delete the row */
            if (del_armed) {
                del_armed = 0;
                py_song_crow_delete(&py_song, c_chain, c_row);
                py_changed(0);
            } else {
                del_armed = 1;
            }
        } else {
            crow_clip = *row;
            crow_clip_ok = 1;
            row->pat = row->snd = PS_EMPTY;
            row->tsp = 0;
            py_changed(0);
        }
        break;
    case TE_KEY_INSERT:
        if (shift) {
            if (py_song_crow_insert(&py_song, c_chain, c_row) < 0)
                py_note("ROW F IS USED");
            else
                py_changed(0);
        } else if (!crow_clip_ok) {
            py_note("CLIPBOARD EMPTY");
        } else {
            *row = crow_clip;
            py_changed(0);
        }
        break;
    default:                                    /* keys 1-8: the track's sound / kit; the rest: nothing */
        if (py_sound_key(key) >= 0)
            py_set_sound(c_track, py_sound_key(key));
        break;
    }
}

static void chain_knob(int knob, int d, int shift)
{
    ps_crow *row = &py_song.chain[c_chain][c_row];
    int v;

    del_armed = 0;
    switch (knob) {
    case 0:                                     /* blue: row */
        v = c_row + d;
        c_row = (uint8_t)(v < 0 ? 0 : v >= PS_CROWS ? PS_CROWS - 1 : v);
        break;
    case 1:                                     /* green: PAT / TSP / SND */
        v = c_col + d;
        c_col = (uint8_t)(v < 0 ? 0 : v > 2 ? 2 : v);
        break;
    case 2:                                     /* white: the value */
        if (!c_col) {                           /* --, 1..14 */
            v = (row->pat < PS_PATTERNS ? row->pat : -1) + d;
            v = v < -1 ? -1 : v >= PS_PATTERNS ? PS_PATTERNS - 1 : v;
            row->pat = (uint8_t)(v < 0 ? PS_EMPTY : v);
            if (v >= 0)
                last_pat = (uint8_t)(v + 1);
        } else if (c_col == 1) {                /* -48..+48 (shift: octaves) */
            v = row->tsp + d * (shift ? 12 : 1);
            row->tsp = (int8_t)(v < -48 ? -48 : v > 48 ? 48 : v);
        } else {                                /* --: the track's sound; 1..8 */
            v = (row->snd < 8 ? row->snd : -1) + d;
            v = v < -1 ? -1 : v > 7 ? 7 : v;
            row->snd = (uint8_t)(v < 0 ? PS_EMPTY : v);
        }
        py_changed(0);
        break;
    default:
        if (shift)
            py_master_step(d);
        break;
    }
}

/* ------------------------------------------------------------------ songs page: keys + knobs
 * 16 slots, each a song with all its patterns (py_slot.c, py_pattern.c). rec / play / cut twice:
 * save / load / delete the cursor slot; the main thread does it (py_req_slot) and reports back. */

void py_v_slots(void)
{
    if (py_view == PY_V_CHAIN)
        leave_chain();
    del_armed = 0;
    rd_ask = 0;
    help = 0;
    sl_armed = 0;
    sl_ask = 0;
    rf_armed = 0;
    py_view = PY_V_SLOTS;
    sl_busy = PY_SL_SCAN;
    py_req_slot(PY_SL_SCAN, 0, 0, 0);
}

/* main thread: slot n's header (-1: all, the scan is done) */
void py_v_slot_scan(int n)
{
    int i;

    for (i = n < 0 ? 0 : n; i < (n < 0 ? PSL_SLOTS : n + 1); i++)
        sl_state[i] = (int8_t)py_pat_slot_peek(i, &sl_info[i]);
    if (n < 0)
        sl_busy = 0;
}

/* main thread: what came of an operation on slot n */
void py_v_slot_done(int op, int n, int r)
{
    const char *what;

    sl_busy = 0;
    if (r == PSL_OK) {
        what = op == PY_SL_SAVE ? "SAVED IN SLOT" : op == PY_SL_LOAD ? "LOADED SLOT" : "DELETED SLOT";
        if (op == PY_SL_LOAD) {                 /* the song view, from its top */
            int t;

            s_row = s_top = s_col = 0;
            live = 0;
            for (t = 0; t < PS_TRACKS; t++)
                last_chain[t] = 0;
            py_view = PY_V_SONG;
        }
    } else {
        what = r == PSL_WAIT ? "PATTERNS NOT READ YET - TRY AGAIN: SLOT"
             : r == PSL_BUSY ? "A PATTERN CHANGED MEANWHILE - TRY AGAIN: SLOT"
             : r == PSL_NOFILE ? "EMPTY: SLOT"
             : r == PSL_OTHER ? "OTHER VERSION, CANNOT LOAD: SLOT"
             : r == PSL_BAD ? "DAMAGED, CANNOT LOAD: SLOT"
             : op == PY_SL_SAVE ? "CANNOT WRITE: SLOT" : op == PY_SL_LOAD ? "CANNOT READ: SLOT" : "CANNOT DELETE: SLOT";
    }
    TE_SNPRINTF(sl_msg, sizeof(sl_msg), "%s %d%d", what, (n + 1) / 10, (n + 1) % 10);
    py_note(sl_msg);
}

/* main thread: the patterns / sounds a load of slot n would change; none: the main thread loads
 * at once */
void py_v_slot_ask(int n, int r, const uint16_t diff[3], const uint16_t used[2], int sbad)
{
    int k, p, i = 0;

    if (r != PSL_OK) {
        py_v_slot_done(PY_SL_LOAD, n, r);
        return;
    }
    for (k = 0; k < 3; k++)
        for (p = 0; p < (k < 2 ? PSL_PATS : PSL_SOUNDS); p++)
            if ((diff[k] >> p) & 1)
                sl_list[i++] = (uint8_t)(k << 4 | p);
    sl_nlist = (uint8_t)i;
    sl_ask_n = (uint8_t)n;
    sl_acur = sl_atop = 0;
    for (k = 0; k < 2; k++) {
        sl_used[k] = used[k];
        sl_keep[k] = (uint16_t)(diff[k] & ~used[k]);    /* the song's own: from the slot; the others: kept */
    }
    sl_bad = (uint8_t)sbad;
    sl_keep[2] = (uint16_t)(diff[2] & sbad);    /* every sound from the slot, but the ones that cannot */
    sl_ask = 1;
    sl_busy = 0;
}

static int ask_keeps(int e)
{
    return (sl_keep[e >> 4] >> (e & 15)) & 1;
}

static void ask_set(int e, int keep)
{
    uint16_t b = (uint16_t)(1u << (e & 15));

    if ((e >> 4) == 2 && ((sl_bad >> (e & 15)) & 1))
        return;                                 /* cannot come back: stays */
    sl_keep[e >> 4] = keep ? (uint16_t)(sl_keep[e >> 4] | b) : (uint16_t)(sl_keep[e >> 4] & ~b);
}

/* an SDK engine's name by its tag (eh_host.h) */
static const char *engine_name(int tag)
{
    static const char *const names[] = { "", "PARTY", "MONO 3", "STATION 1", "TRIO", "WHEEL", "WARP",
                                         "MALLET", "ACID 3", "PALETTE" };

    return tag > 0 && tag < (int)(sizeof(names) / sizeof(names[0])) ? names[tag] : "SDK ENGINE";
}

static void ask_key(int key)
{
    int i, any = 0;

    switch (key) {
    case TE_KEY_PLAY:                           /* load: the choices */
        sl_ask = 0;
        sl_busy = PY_SL_LOAD;
        py_req_slot(PY_SL_LOAD, sl_ask_n, (uint32_t)sl_keep[0] | (uint32_t)sl_keep[1] << 16, sl_keep[2]);
        return;
    case TE_KEY_JOIN:
    case TE_KEY_ORANGE_PUSH:
        sl_ask = 0;
        py_note("NOT LOADED");
        return;
    case TE_KEY_STOP:
        py_req_stop();
        return;
    case TE_KEY_GREEN_PUSH:                     /* the cursor pattern: the other choice */
        if (sl_acur < sl_nlist)
            ask_set(sl_list[sl_acur], !ask_keeps(sl_list[sl_acur]));
        return;
    case TE_KEY_WHITE_PUSH:                     /* all: kept if one is the slot's, else all the slot's */
        for (i = 0; i < sl_nlist; i++)
            any |= !ask_keeps(sl_list[i]);      /* (a sound that cannot come back: kept anyway) */
        for (i = 0; i < sl_nlist; i++)
            ask_set(sl_list[i], any);
        return;
    default:
        return;
    }
}

static void ask_knob(int knob, int d)
{
    int v;

    if (knob == 1) {                            /* green: right the slot's, left kept */
        if (sl_acur < sl_nlist)
            ask_set(sl_list[sl_acur], d < 0);
        return;
    }
    if (knob != 0 && knob != 2)
        return;
    v = sl_acur + d;
    sl_acur = (uint8_t)(v < 0 ? 0 : v >= sl_nlist ? sl_nlist - 1 : v);
}

static void slots_key(int key, int shift)
{
    int armed = sl_armed, st = sl_state[sl_cur], want = 0;

    if (sl_busy)
        return;                                 /* an operation runs: wait for it */
    if (sl_ask) {
        ask_key(key);
        return;
    }
    sl_armed = 0;
    if (key == TE_KEY_JOIN && shift) {          /* the files page */
        py_v_files();
        return;
    }
    switch (key) {
    case TE_KEY_JOIN:
    case TE_KEY_ORANGE_PUSH:
        py_view = PY_V_SONG;
        return;
    case TE_KEY_STOP:
        py_req_stop();
        return;
    case TE_KEY_REC:
        want = PY_SL_SAVE;
        break;
    case TE_KEY_PLAY:
        want = PY_SL_LOAD;
        break;
    case TE_KEY_CUT:
        want = PY_SL_DELETE;
        break;
    default:
        return;                                 /* (and an armed operation is off) */
    }
    if (want == PY_SL_LOAD && st != PSL_OK) {
        py_note(st == PSL_NOFILE ? "THIS SLOT IS EMPTY" : st == PSL_OTHER ? "OTHER VERSION: CANNOT LOAD IT"
                : "THIS SLOT IS DAMAGED");
        return;
    }
    if (want == PY_SL_DELETE && st == PSL_NOFILE) {
        py_note("THIS SLOT IS EMPTY");
        return;
    }
    if (armed != want) {                        /* the first press asks */
        sl_armed = (uint8_t)want;
        return;
    }
    if (want == PY_SL_LOAD)
        want = PY_SL_DIFF;                      /* first: which patterns would change */
    sl_busy = (uint8_t)want;
    py_req_slot(want, sl_cur, 0, 0);
}

/* ------------------------------------------------------------------ files page: keys + knobs
 * The render folder: every RENDnn.AIF with its length and size; cut twice deletes the cursor
 * file (the main thread: playback stops, the pool is read again). */

void py_v_files(void)
{
    if (py_view == PY_V_CHAIN)
        leave_chain();
    del_armed = 0;
    rd_ask = 0;
    help = 0;
    sl_armed = 0;
    sl_ask = 0;
    rf_armed = 0;
    py_view = PY_V_FILES;
    sl_busy = PY_SL_FSCAN;
    py_req_slot(PY_SL_FSCAN, 0, 0, 0);
}

/* main thread: the folder read anew (after a delete: what came of it) */
void py_v_files_done(int op, int n, int r)
{
    if (rf_cur >= py_nrfiles)
        rf_cur = (uint8_t)(py_nrfiles ? py_nrfiles - 1 : 0);
    sl_busy = 0;
    (void)n;
    if (op == PY_SL_FDELETE)
        py_note(r < 0 ? "CANNOT DELETE THE FILE" : "FILE DELETED");
}

/* AUDIO tracks of the song that play file `name`: bit t */
static int file_users(const char *name)
{
    int t, i, m = 0;

    for (t = 0; t < PS_TRACKS; t++) {
        if (py_song.trk[t].type != PS_AUDIO)
            continue;
        for (i = 0; i < PS_NAME && name[i] == py_song.trk[t].file[i] && name[i]; i++)
            ;
        if (i < PS_NAME && name[i] == py_song.trk[t].file[i])
            m |= 1 << t;
    }
    return m;
}

static void files_key(int key, int shift)
{
    int armed = rf_armed;

    if (sl_busy)
        return;
    rf_armed = 0;
    switch (key) {
    case TE_KEY_JOIN:
        if (shift)
            py_v_slots();                       /* shift + join: the songs page again */
        else
            py_view = PY_V_SONG;
        return;
    case TE_KEY_ORANGE_PUSH:
        py_view = PY_V_SONG;
        return;
    case TE_KEY_STOP:
        py_req_stop();
        return;
    case TE_KEY_CUT:
        if (!py_nrfiles)
            return;
        if (!armed) {                           /* the first press asks */
            rf_armed = 1;
            return;
        }
        sl_busy = PY_SL_FDELETE;
        py_req_slot(PY_SL_FDELETE, rf_cur, 0, 0);
        return;
    default:
        return;
    }
}

static void files_knob(int knob, int d)
{
    int v;

    if (sl_busy || (knob != 0 && knob != 2) || !py_nrfiles)
        return;
    rf_armed = 0;
    v = rf_cur + d;
    rf_cur = (uint8_t)(v < 0 ? 0 : v >= py_nrfiles ? py_nrfiles - 1 : v);
}

static void slots_knob(int knob, int d)
{
    int v;

    if (sl_busy)
        return;
    if (sl_ask) {
        ask_knob(knob, d);
        return;
    }
    if (knob != 0 && knob != 2)                 /* blue or white: the slot */
        return;
    sl_armed = 0;
    v = sl_cur + d;
    sl_cur = (uint8_t)(v < 0 ? 0 : v >= PSL_SLOTS ? PSL_SLOTS - 1 : v);
}

/* the chain view only makes sense on a SYNTH / DRUM track (the list may have changed: a new set) */
static int chain_ok(void)
{
    if (py_view == PY_V_CHAIN && !py_song_is_pattern(&py_song, c_track))
        leave_chain();
    return py_view == PY_V_CHAIN;
}

void py_v_key(int key)
{
    int shift = *TE_SHIFT_HELD != 0;

    if (py_view == PY_V_SLOTS)
        slots_key(key, shift);
    else if (py_view == PY_V_FILES)
        files_key(key, shift);
    else if (chain_ok())
        chain_key(key, shift);
    else
        song_key(key, shift);
}

void py_v_knob(int knob, int d)
{
    int shift = *TE_SHIFT_HELD != 0;

    if (!d)
        return;
    if (py_view == PY_V_SLOTS)
        slots_knob(knob, d);
    else if (py_view == PY_V_FILES)
        files_knob(knob, d);
    else if (chain_ok())
        chain_knob(knob, d, shift);
    else
        song_knob(knob, d, shift);
}

int py_v_pattern(int *t)
{
    int list[PS_TRACKS], n, c;
    const ps_head *h;

    if (py_view == PY_V_SLOTS || py_view == PY_V_FILES) {
        *t = -1;
        return -1;
    }
    if (chain_ok()) {
        *t = c_track;
        c = py_song.chain[c_chain][c_row].pat;
        return c < PS_PATTERNS ? c : -1;
    }
    *t = cursor_track(list, &n);
    if (*t < 0 || !py_song_is_pattern(&py_song, *t))
        return -1;
    h = py_pat_head(*t);                        /* what the column plays now, else the cell's first row */
    if ((py_mx.playing || py_mx.pos) && h->active && h->pat < PS_PATTERNS)
        return h->pat;
    c = py_song.cell[s_row][*t];
    return c < PS_CHAINS && py_song.chain[c][0].pat < PS_PATTERNS ? py_song.chain[c][0].pat : -1;
}

/* ------------------------------------------------------------------ drawing (GUI thread) */

static void frame_rect(int x, int y, int w, int h, uint16_t c)
{
    gfx_rect(x, y, w, 1, c);
    gfx_rect(x, y + h - 1, w, 1, c);
    gfx_rect(x, y, 1, h, c);
    gfx_rect(x + w - 1, y, 1, h, c);
}

/* the song row track t plays now; -1 = none (stopped at bar 1, an audition, nothing to play) */
static int head_row(int t)
{
    if ((!py_mx.playing && !py_mx.pos) || py_pat_audition_track() >= 0)
        return -1;
    if (py_song_is_pattern(&py_song, t)) {
        const ps_head *h = py_pat_head(t);

        return h->active && h->solo == PS_EMPTY ? h->row : -1;
    }
    return py_mx.set ? py_set_row(py_mx.set, t, py_mx.pos) : -1;       /* AUDIO: its segment's row */
}

/* list position (1-based) of the first track in `mask` other than track `not`; 0 = none */
static int other_user(int mask, int not, const int *list, int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (list[i] != not && ((mask >> list[i]) & 1))
            return i + 1;
    return 0;
}

/* footer line 1: an armed delete or a note instead of the view's own text; 1 = drawn */
static int footer_note(const char *armed)
{
    const char *note = py_note_now();

    if (del_armed)
        gfx_text7(4, PY_FOOT_Y1, armed, C_RED);
    else if (note)
        gfx_text7(4, PY_FOOT_Y1, note, C_AMBER);
    return del_armed || note != 0;
}

/* the footer's render question: RENDER 8 BARS FROM ROW 00 - UP TO 1.2 MB */
static const char *render_ask(void)
{
    static char t[64];
    uint32_t tenths = (uint32_t)rd_bars * py_clock_bar() / 26214u;     /* stereo: 4 bytes a frame */
    const char *p;
    char num[8];
    int i = 0;

#define PUT(str) for (p = (str); *p; p++) t[i++] = *p
    PUT("RENDER ");
    py_d_fmt(num, rd_bars);
    PUT(num);
    PUT(" BARS FROM ROW ");
    py_d_hex2(num, s_row);
    PUT(num);
    PUT(" - UP TO ");
    py_d_fmt(num, (int)(tenths / 10u));
    PUT(num);
    t[i++] = '.';
    t[i++] = (char)('0' + tenths % 10u);
    PUT(" MB");
#undef PUT
    t[i] = 0;
    return t;
}

static void song_frame(void)
{
    static uint8_t blink, s_left;
    static py_sm m;
    int list[PS_TRACKS], n, t = cursor_track(list, &n), i, la, lb, ln, changed, slot;
    const char *note;
    py_hm h;

    blink++;
    m.s = &py_song;
    m.n = (uint8_t)n;
    m.live = live;
    m.playing = py_mx.playing;
    m.row = s_row;
    m.col = s_col;
    m.blink = blink;
    m.top = (uint8_t *)&s_top;
    m.left = &s_left;
    ln = py_song_loop(&py_song, &la, &lb);
    m.loop_a = la;
    m.loop_b = lb;
    m.loop_n = ln;
    for (i = 0; i < n; i++) {
        const ps_track *tr = &py_song.trk[list[i]];
        py_sc *c = &m.c[i];

        m.list[i] = (uint8_t)list[i];
        c->k = (uint8_t)list[i];
        c->type = tr->type;
        c->mute = tr->mute;
        c->solo = tr->solo;
        c->starved = (uint8_t)(py_pat_starved(list[i]) != 0);
        c->slot = (int8_t)(tr->type == PS_AUDIO ? 0 : py_pat_slot(list[i]) + 1);
        c->file = tr->file;
        c->head = (int16_t)head_row(list[i]);
        c->cue = (int16_t)(py_mx.playing ? py_cue_row(list[i]) : -1);
    }
    note = py_note_now();
    m.note = 0;
    if (rd_ask) {
        m.note = render_ask();
        m.note_col = D_AMBER;
    } else if (del_armed) {
        m.note = "SHIFT + CUT AGAIN: DELETE THIS SONG ROW";
        m.note_col = D_RED;
    } else if (note) {
        m.note = note;
        m.note_col = D_AMBER;
    } else if (t < 0) {
        m.note = "NO TRACKS: ADD THEM ON THE LIST - TAPE";
        m.note_col = D_MUTED;
    }
    py_hdr_fill(&h, live ? "LIVE" : "SONG", -1, live ? C_RED : C_WHITE);
    slot = py_pat_slot_now(&changed);
    if (!h.ctx) {
        static char sl[12] = "SLOT 00";

        if (slot >= 0 && !py_mx.playing) {
            sl[5] = (char)('0' + (slot + 1) / 10);
            sl[6] = (char)('0' + (slot + 1) % 10);
            sl[7] = changed ? '*' : 0;
            sl[8] = 0;
            h.ctx = sl;
        } else {
            h.ctx = py_cpu_ctx("");
        }
    }
    h.row = s_row;
    h.bare = 1;                                 /* v0.17: no slot, BPM, ROW, BAR in the song view */
    h.loop_a = la;
    h.loop_b = lb;
    h.loop_n = ln;
    py_d_header(&h);
    py_d_song(&m);
    if (m.note) {                               /* v0.17: no hint row, only notes */
        py_d_text3(4, D_FOOT_Y, m.note, m.note_col, 0);
        if (rd_ask)
            py_d_text3(232, D_FOOT_Y, "REC: START", D_TEXT, 0);
    }
}

static void chain_frame(void)
{
    const ps_track *tr = &py_song.trk[c_track];
    const ps_head *h = py_pat_head(c_track);
    const ps_crow *cur = &py_song.chain[c_chain][c_row];
    int list[PS_TRACKS], n = py_rows(list), i, x, other, pos = 0, ended = 0, play_row = -1;
    uint16_t col = colour(tr->type);

    py_draw_header("CHAIN", c_chain, C_WHITE);
    gfx_rect(0, PY_FOOT_LINE, gfx_w, 1, C_LINE);
    for (i = 0; i < n; i++)
        if (list[i] == c_track)
            pos = i + 1;
    x = gfx_text7(4, 21, "TRACK ", C_GREY);
    x = gfx_dec7(x, 21, pos, C_WHITE);
    x = gfx_text7(x + 6, 21, tr->type == PS_SYNTH ? "SYN" : "DRM", col);
    x = gfx_text7(x + 12, 21, tr->type == PS_DRUM ? "KIT " : "SND ", C_GREY);
    gfx_dec7(x, 21, (tr->sound < 8 ? tr->sound : py_pat_slot(c_track)) + 1, C_WHITE);
    other = other_user(py_song_chain_users(&py_song, c_chain), c_track, list, n);
    if (py_pat_audition_track() >= 0) {
        gfx_text7(164, 21, "AUDITION: OTHERS SILENT", C_GREEN);
    } else if (other) {
        x = gfx_text7(164, 21, "ALSO ON TRACK ", C_GREY);
        gfx_dec7(x, 21, other, C_GREY);
    }
    /* the row this track plays now, if it is in this chain */
    if ((py_mx.playing || py_mx.pos) && h->active
        && (h->solo == c_chain || (h->solo == PS_EMPTY && py_pat_audition_track() < 0
                                   && py_song.cell[h->row][c_track] == c_chain)))
        play_row = h->crow;
    for (i = 0; i < 2; i++) {
        gfx_text7(4 + i * 160, 31, "ROW", C_DIM);
        gfx_text7(34 + i * 160, 31, "PAT", C_DIM);
        gfx_text7(70 + i * 160, 31, "TSP", C_DIM);
        gfx_text7(106 + i * 160, 31, tr->type == PS_DRUM ? "KIT" : "SND", C_DIM);
    }
    for (i = 0; i < PS_CROWS; i++) {
        const ps_crow *cr = &py_song.chain[c_chain][i];
        int x0 = 4 + (i / 8) * 160, y = CROW_Y(i % 8), has = cr->pat < PS_PATTERNS;
        int here = i == play_row;
        uint16_t ink = here ? GFX_BLACK : ended ? C_DIM : C_WHITE;

        if (i == c_row)
            gfx_rect(x0 - 4, y - 2, 152, 10, C_SEL);
        if (here)
            gfx_rect(x0 + 28, y - 1, 98, 9, col);
        gfx_hex7(x0, y, (uint32_t)i, 1, i == c_row ? C_WHITE : C_GREY);
        if (has) {
            if (cr->pat < 9)
                gfx_text7(x0 + 30, y, "0", ink);
            gfx_dec7(x0 + (cr->pat < 9 ? 36 : 30), y, cr->pat + 1, ink);
            x = x0 + 66;
            if (cr->tsp >= 0)
                x = gfx_text7(x, y, "+", ink);
            gfx_dec7(x, y, cr->tsp, ink);
            if (cr->snd < 8)
                gfx_dec7(x0 + 112, y, cr->snd + 1, ink);
            else
                gfx_text7(x0 + 112, y, "-", here ? ink : C_DIM);     /* the track's own */
        } else if (!ended) {
            gfx_text7(x0 + 30, y, "--", C_DIM);         /* the chain ends here */
            ended = 1;
        }
        if (i == c_row)
            frame_rect(x0 + (c_col == 2 ? 103 : c_col ? 63 : 27), y - 2, c_col == 1 ? 24 : 18, 11, C_WHITE);
    }
    if (!footer_note("SHIFT + CUT AGAIN: DELETE THIS CHAIN ROW")) {
        x = gfx_text7(4, PY_FOOT_Y1, "ROW ", C_WHITE);
        x = gfx_hex7(x, PY_FOOT_Y1, c_row, 1, C_WHITE);
        if (cur->pat < PS_PATTERNS) {
            x = gfx_text7(x, PY_FOOT_Y1, ": PATTERN ", C_WHITE);
            x = gfx_dec7(x, PY_FOOT_Y1, cur->pat + 1, C_WHITE);
            if (cur->snd < 8) {
                x = gfx_text7(x, PY_FOOT_Y1, tr->type == PS_DRUM ? "  KIT " : "  SOUND ", C_WHITE);
                x = gfx_dec7(x, PY_FOOT_Y1, cur->snd + 1, C_WHITE);
            }
            if (tr->type == PS_DRUM && cur->tsp)
                gfx_text7(x, PY_FOOT_Y1, "  TSP = TONE", C_GREY);
        } else {
            gfx_text7(x, PY_FOOT_Y1, ": EMPTY  WHITE PRESS: LAST", C_GREY);
        }
        gfx_text7(232, PY_FOOT_Y1, "SPEECH: HELP", C_DIM);
    }
    gfx_text7(4, PY_FOOT_Y2, "PLAY: AUDITION  SHIFT+BLUE: EDIT  ORANGE PRESS: BACK", C_GREY);
}

static int bits(uint32_t m)
{
    int n = 0;

    for (; m; m >>= 1)
        n += (int)(m & 1);
    return n;
}

static void two(char *out, int v)
{
    out[0] = (char)('0' + v / 10);
    out[1] = (char)('0' + v % 10);
    out[2] = 0;
}

/* the load question over the slot list */
static void ask_frame(void)
{
    int r, x, n = sl_nlist, cur = sl_acur, top = sl_atop, keeps = 0, i, ns = 0;
    char num[4];

    for (i = 0; i < n; i++)
        ns += (sl_list[i] >> 4) == 2;
    py_draw_header("SONGS", -1, C_WHITE);
    gfx_rect(0, PY_FOOT_LINE, gfx_w, 1, C_LINE);
    top = cur < top ? cur : cur >= top + SL_VIS ? cur - SL_VIS + 1 : top;
    sl_atop = (uint8_t)top;
    two(num, sl_ask_n + 1);
    x = gfx_text7(4, 22, "LOAD SLOT ", C_WHITE);
    x = gfx_text7(x, 22, num, C_WHITE);
    x = gfx_text7(x, 22, ": ", C_WHITE);
    if (n > ns) {
        x = gfx_dec7(x, 22, n - ns, C_AMBER);
        x = gfx_text7(x, 22, n - ns == 1 ? " PATTERN" : " PATTERNS", C_AMBER);
    }
    if (ns) {
        x = gfx_text7(x, 22, n > ns ? ", " : "", C_AMBER);
        x = gfx_dec7(x, 22, ns, C_AMBER);
        x = gfx_text7(x, 22, ns == 1 ? " SOUND" : " SOUNDS", C_AMBER);
    }
    gfx_text7(x, 22, n == 1 ? " DIFFERS" : " DIFFER", C_AMBER);
    for (r = 0; r < SL_VIS && top + r < n; r++) {
        int e = sl_list[top + r], k = e >> 4, p = e & 15, y = 34 + r * 10;
        int keep = ask_keeps(e), used = k < 2 && ((sl_used[k] >> p) & 1);

        if (top + r == cur)
            gfx_rect(0, y - 2, gfx_w, 10, C_SEL);
        if (k == 2) {                           /* a synth sound: its engine, or why it stays */
            int why = py_snd_why[p];

            x = gfx_text7(4, y, "SOUND ", C_WHITE);
            gfx_dec7(x, y, p + 1, C_WHITE);
            gfx_text7(100, y, why == PY_SND_TE ? "TE ENGINE" : engine_name(py_snd_tag[p]), C_WHITE);
            if ((sl_bad >> p) & 1)
                gfx_text7(200, y, why == PY_SND_MISSING ? "KEPT: NOT HERE" : why == PY_SND_OTHER
                          ? "KEPT: OTHER ENGINE" : "KEPT", C_DIM);
            else
                gfx_text7(200, y, keep ? "KEEP NOW" : "FROM THE SLOT", keep ? C_AMBER : C_GREEN);
            continue;
        }
        x = gfx_text7(4, y, k ? "DRUM ROLL " : "NOTE ROLL ", k ? C_RED : C_BLUE);
        two(num, p + 1);
        gfx_text7(x, y, num, k ? C_RED : C_BLUE);
        gfx_text7(100, y, used ? "IN THE SONG" : "NOT IN THE SONG", used ? C_WHITE : C_DIM);
        gfx_text7(200, y, keep ? "KEEP NOW" : "FROM THE SLOT", keep ? C_AMBER : C_GREEN);
    }
    for (i = 0; i < n; i++)
        keeps += ask_keeps(sl_list[i]);
    if (!footer_note("")) {
        if (!keeps) {
            gfx_text7(4, PY_FOOT_Y1, "ALL AS SAVED IN THE SLOT", C_GREEN);
        } else {
            x = gfx_dec7(4, PY_FOOT_Y1, keeps, C_AMBER);
            gfx_text7(x, PY_FOOT_Y1, " KEPT AS NOW, THE REST AS SAVED", C_AMBER);
        }
        gfx_text7(232, PY_FOOT_Y1, "SPEECH: HELP", C_DIM);
    }
    gfx_text7(4, PY_FOOT_Y2, "GREEN: CHANGE  WHITE PR: ALL  PLAY: LOAD  JOIN: NO", C_GREY);
}

static void slots_frame(void)
{
    int i, r, x, changed, now = py_pat_slot_now(&changed), cur = sl_cur, top = sl_top;
    int st = sl_state[cur];
    char num[4];

    if (sl_ask) {
        ask_frame();
        return;
    }
    py_draw_header("SONGS", -1, C_WHITE);
    gfx_rect(0, PY_FOOT_LINE, gfx_w, 1, C_LINE);
    top = cur < top ? cur : cur >= top + SL_VIS ? cur - SL_VIS + 1 : top;
    sl_top = (uint8_t)top;
    gfx_text7(4, 22, "SLOT", C_DIM);
    gfx_text7(40, 22, "TRACKS", C_DIM);
    gfx_text7(232, 22, "ROWS", C_DIM);
    gfx_text7(274, 22, "PAT", C_DIM);
    for (r = 0; r < SL_VIS; r++) {
        int n = top + r, y = 34 + r * 10, k = sl_state[n];
        const psl_info *in = &sl_info[n];

        if (n == cur)
            gfx_rect(0, y - 2, gfx_w, 10, C_SEL);
        num[0] = (char)('0' + (n + 1) / 10);
        num[1] = (char)('0' + (n + 1) % 10);
        num[2] = 0;
        gfx_text7(4, y, num, n == now ? (changed ? C_AMBER : C_GREEN) : n == cur ? C_WHITE : C_GREY);
        if (n == now)
            gfx_text7(18, y, "*", changed ? C_AMBER : C_GREEN);
        if (k == 1 || (sl_busy == PY_SL_SCAN)) {
            gfx_text7(40, y, "...", C_DIM);
        } else if (k == PSL_NOFILE) {
            gfx_text7(40, y, "--", C_DIM);
        } else if (k == PSL_OTHER) {
            gfx_text7(40, y, "OTHER VERSION", C_AMBER);
        } else if (k != PSL_OK) {
            gfx_text7(40, y, "DAMAGED", C_RED);
        } else {
            for (i = x = 0; i < PS_TRACKS; i++)
                if (in->type[i] != PS_NONE) {
                    gfx_text7(40 + x * 22, y, in->type[i] == PS_AUDIO ? "AUD" : in->type[i] == PS_SYNTH ? "SYN" : "DRM",
                              colour(in->type[i]));
                    x++;
                }
            if (!x)
                gfx_text7(40, y, "NO TRACKS", C_GREY);
            gfx_dec7(232, y, in->rows, C_WHITE);
            gfx_dec7(274, y, bits(in->mask[0]) + bits(in->mask[1]), C_WHITE);
        }
    }
    if (sl_busy) {
        gfx_text7(4, PY_FOOT_Y1, sl_busy == PY_SL_SAVE ? "SAVING ..." : sl_busy == PY_SL_LOAD ? "LOADING ..."
                  : sl_busy == PY_SL_DELETE ? "DELETING ..." : sl_busy == PY_SL_DIFF ? "COMPARING THE PATTERNS ..."
                  : "READING THE SLOTS ...", C_AMBER);
    } else if (sl_armed) {
        x = gfx_text7(4, PY_FOOT_Y1, sl_armed == PY_SL_SAVE ? (st == PSL_NOFILE ? "REC AGAIN: SAVE IN SLOT " : "REC AGAIN: REPLACE SLOT ")
                      : sl_armed == PY_SL_LOAD ? "PLAY AGAIN: LOAD SLOT " : "CUT AGAIN: DELETE SLOT ", C_RED);
        num[0] = (char)('0' + (cur + 1) / 10);
        num[1] = (char)('0' + (cur + 1) % 10);
        num[2] = 0;
        gfx_text7(x, PY_FOOT_Y1, num, C_RED);
    } else if (!footer_note("")) {
        if (now < 0) {
            gfx_text7(4, PY_FOOT_Y1, "THE SONG NOW IS NOT IN A SLOT", C_GREY);
        } else {
            num[0] = (char)('0' + (now + 1) / 10);
            num[1] = (char)('0' + (now + 1) % 10);
            num[2] = 0;
            x = gfx_text7(4, PY_FOOT_Y1, "THE SONG NOW: SLOT ", C_GREY);
            x = gfx_text7(x, PY_FOOT_Y1, num, changed ? C_AMBER : C_GREEN);
            if (changed)
                gfx_text7(x, PY_FOOT_Y1, ", CHANGED SINCE", C_AMBER);
        }
        gfx_text7(232, PY_FOOT_Y1, "SPEECH: HELP", C_DIM);
    }
    if (sl_armed == PY_SL_LOAD)
        gfx_text7(4, PY_FOOT_Y2, now < 0 || changed ? "REPLACES THE SONG - NOT SAVED! (ASKS ABOUT PATTERNS)"
                  : "REPLACES THE SONG (ASKS ABOUT PATTERNS)", now < 0 || changed ? C_AMBER : C_GREY);
    else
        gfx_text7(4, PY_FOOT_Y2, "REC/PLAY/CUT X2: SAVE/LOAD/DEL  SHIFT+JOIN: FILES", C_GREY);
}

/* m:ss of n frames at rate */
static int draw_len(int x, int y, uint32_t frames, uint32_t rate, uint16_t c)
{
    uint32_t sec = rate ? frames / rate : 0;
    char b[4];

    x = gfx_dec7(x, y, (int32_t)(sec / 60u), c);
    x = gfx_text7(x, y, ":", c);
    two(b, (int)(sec % 60u));
    return gfx_text7(x, y, b, c);
}

/* MB with one decimal */
static int draw_mb(int x, int y, uint32_t bytes, uint16_t c)
{
    uint32_t t = (bytes + 52429u) / 104858u;    /* tenths of a MB */

    x = gfx_dec7(x, y, (int32_t)(t / 10u), c);
    x = gfx_text7(x, y, ".", c);
    return gfx_dec7(x, y, (int32_t)(t % 10u), c);
}

static void files_frame(void)
{
    int r, x, n = py_nrfiles, cur = rf_cur, top = rf_top, i, users;
    uint32_t total = 0;

    py_draw_header("FILES", -1, C_WHITE);
    gfx_rect(0, PY_FOOT_LINE, gfx_w, 1, C_LINE);
    top = cur < top ? cur : cur >= top + SL_VIS ? cur - SL_VIS + 1 : top;
    rf_top = (uint8_t)top;
    gfx_text7(4, 22, "RENDER", C_DIM);
    gfx_text7(100, 22, "LENGTH", C_DIM);
    gfx_text7(160, 22, "MB", C_DIM);
    gfx_text7(200, 22, "PLAYED BY", C_DIM);
    if (sl_busy == PY_SL_FSCAN) {
        gfx_text7(4, 34, "...", C_DIM);
        n = 0;
    } else if (!n) {
        gfx_text7(4, 34, "NO RENDERS (REC IN THE SONG VIEW MAKES ONE)", C_GREY);
    }
    for (r = 0; r < SL_VIS && top + r < n; r++) {
        const py_rfile *f = &py_rfiles[top + r];
        int y = 34 + r * 10;

        if (top + r == cur)
            gfx_rect(0, y - 2, gfx_w, 10, C_SEL);
        gfx_text7(4, y, f->name, top + r == cur ? C_WHITE : C_GREY);
        if (f->ch) {
            draw_len(100, y, f->frames, f->rate, C_WHITE);
            gfx_text7(136, y, f->ch == 2 ? "ST" : "MO", C_DIM);
        } else {
            gfx_text7(100, y, "?", C_RED);
        }
        draw_mb(160, y, f->size, C_WHITE);
        users = file_users(f->name);
        if (!users) {
            gfx_text7(200, y, "-", C_DIM);
        } else {
            for (i = 0, x = 200; i < PS_TRACKS; i++)
                if ((users >> i) & 1)
                    x = gfx_dec7(x, y, i + 1, C_AMBER) + 6;
        }
    }
    for (i = 0; i < n; i++)
        total += py_rfiles[i].size;
    if (sl_busy) {
        gfx_text7(4, PY_FOOT_Y1, sl_busy == PY_SL_FDELETE ? "DELETING ..." : "READING THE FOLDER ...", C_AMBER);
    } else if (rf_armed && cur < n) {
        users = file_users(py_rfiles[cur].name);
        x = gfx_text7(4, PY_FOOT_Y1, "CUT AGAIN: DELETE ", C_RED);
        x = gfx_text7(x, PY_FOOT_Y1, py_rfiles[cur].name, C_RED);
        if (users)
            gfx_text7(x, PY_FOOT_Y1, " (PLAYED!)", C_AMBER);
    } else if (!footer_note("")) {
        x = gfx_dec7(4, PY_FOOT_Y1, n, C_GREY);
        x = gfx_text7(x, PY_FOOT_Y1, n == 1 ? " FILE, " : " FILES, ", C_GREY);
        x = draw_mb(x, PY_FOOT_Y1, total, C_GREY);
        gfx_text7(x, PY_FOOT_Y1, " MB", C_GREY);
        gfx_text7(232, PY_FOOT_Y1, "SPEECH: HELP", C_DIM);
    }
    gfx_text7(4, PY_FOOT_Y2, rf_armed ? "DELETING STOPS PLAYBACK" : "CUT X2: DELETE  SHIFT+JOIN: SONGS  JOIN: BACK",
              rf_armed ? C_AMBER : C_GREY);
}

void py_v_frame(void)
{
    if (py_view == PY_V_FILES)
        files_frame();
    else if (py_view == PY_V_SLOTS)
        slots_frame();
    else if (chain_ok())
        chain_frame();
    else
        song_frame();
}

/* ------------------------------------------------------------------ help (speech) */

typedef struct help_line { const char *key, *text; } help_line;     /* key 0: text over the full width */

#define HELP_PAGES  6
#define HELP_LINES  12

static const char *const help_title[HELP_PAGES] = { "HELP: PLAY", "HELP: SONG", "HELP: CHAIN", "HELP: LIVE",
                                                       "HELP: SONGS", "HELP: FILES" };

static const help_line help_page[HELP_PAGES][HELP_LINES] = {
    {   /* the track list */
        { "BLUE", "CURSOR - PRESS: MUTE" },
        { "GREEN", "LEVEL - PRESS: SOLO" },
        { "WHITE", "FILE, SOUND, KIT - PRESS: OPTIONS" },
        { "ORANGE", "PATTERN 1-14 - SHIFT: MASTER" },
        { "SHIFT+WHITE PR", "ECO: THE SYNTH AT HALF RATE" },
        { "SHIFT+BLUE PR", "EDIT THE PATTERN (NOTE / DRUM ROLL)" },
        { "INSERT X2", "NEW TRACK - CUT X2: REMOVE IT" },
        { "PLAY / STOP", "START, PAUSE / PAUSE, AGAIN: BAR 1" },
        { "< > / METRONOME", "BAR BACK, FORWARD / TEMPO: THE BPM" },
        { "JOIN / TAPE", "SONG VIEW / LAYOUT: LANES, ROWS" },
        { "KEYS 1-8", "SOUND / KIT 1-8 OF THE CURSOR TRACK" },
        { "SPEECH", "THIS HELP - BLUE: THE OTHER PAGES" },
    },
    {   /* the song view */
        { "BLUE / GREEN", "ROW (SHIFT: 16) / COLUMN" },
        { "WHITE", "CHAIN 00-7F, OF = A SILENT BAR, ON" },
        { "WHITE PRESS", "LAST CHAIN / OPEN CHAIN / ON-OF" },
        { "SHIFT+WHITE PR", "NEW CHAIN / CLONE THE CHAIN" },
        { "CUT / INSERT", "CUT / PASTE THE CELL" },
        { "SHIFT+INSERT", "NEW ROW - SHIFT+CUT X2: DELETE ROW" },
        { "PLAY / SHIFT", "FROM THE ROW / CUE IT (CHAIN'S END)" },
        { "SHIFT+< / >", "LOOP START / END - ORANGE: TIMES" },
        { "REC, REC", "RENDER THE SONG TO AN AIF FILE" },
        { "SHIFT+REC", "LIVE MODE ON / OFF" },
        { "KEYS 1-8", "SOUND / KIT 1-8 OF THE CURSOR TRACK" },
        { "TAPE", "BACK TO THE TRACK LIST" },
    },
    {   /* the chain view */
        { 0, "16 ROWS: PATTERN, TRANSPOSE, SOUND - EMPTY ROW = END" },
        { "BLUE / GREEN", "ROW / PAT, TSP, SND" },
        { "WHITE", "PATTERN 1-14 / TRANSPOSE (SHIFT: 12)" },
        { "WHITE ON SND", "THE ROW'S SOUND 1-8, - = THE TRACK'S" },
        { "WHITE PRESS", "EMPTY ROW: THE LAST PATTERN" },
        { "CUT / INSERT", "CUT / PASTE THE ROW" },
        { "SHIFT+INSERT", "NEW ROW - SHIFT+CUT X2: DELETE ROW" },
        { "PLAY / STOP", "AUDITION THIS CHAIN ALONE / STOP" },
        { "SHIFT+BLUE PR", "EDIT THE ROW'S PATTERN" },
        { "BLUE, GREEN PR", "MUTE / SOLO THE TRACK" },
        { "JOIN, ORANGE PR", "BACK TO THE SONG VIEW" },
        { 0, "A CHAIN IS SHARED: ANY TRACK CAN USE IT" },
    },
    {   /* live mode */
        { 0, "SHIFT+REC IN THE SONG VIEW: LIVE - NO CELL EDITS" },
        { "PLAY", "CUE THE CURSOR ROW ON ALL TRACKS" },
        { "WHITE PRESS", "CUE THE CURSOR CELL: ONE TRACK" },
        { 0, "A TRACK JUMPS TO ITS CUE WHEN ITS CHAIN ENDS" },
        { 0, "AND LOOPS FROM THERE - AN EMPTY CELL STOPS IT" },
        { "T1-T4", "MUTE TRACK 1-4 - SHIFT: SOLO" },
        { "BLUE, GREEN PR", "MUTE / SOLO THE CURSOR TRACK" },
        { "BLUE / GREEN", "ROW / COLUMN" },
        { "STOP", "PAUSE, AGAIN: BAR 1, ROW 00" },
        { "SHIFT+REC", "BACK TO EDITING" },
        { "REC, REC", "RENDER FROM THE CURSOR ROW (NO CUES)" },
        { "KEYS 1-8", "SOUND / KIT OF THE CURSOR TRACK, NOW" },
    },
    {   /* the songs page */
        { 0, "16 SLOTS: SONG, PATTERNS, SYNTH SOUNDS" },
        { "SHIFT+JOIN", "THIS PAGE (SONG VIEW, TRACK LIST)" },
        { "BLUE / WHITE", "THE SLOT" },
        { "REC, REC", "SAVE THE SONG IN THE SLOT" },
        { "PLAY, PLAY", "LOAD IT - WHAT DIFFERS FROM NOW: ASKED" },
        { "  GREEN", "FROM THE SLOT OR KEEP NOW" },
        { "  WHITE PR", "ALL - PLAY: LOAD, JOIN: DO NOT" },
        { "CUT, CUT", "DELETE THE SLOT" },
        { "JOIN, ORANGE PR", "BACK TO THE SONG VIEW" },
        { 0, "DISK MODE: THE SLOTS AS SONGNN.PSL FILES;" },
        { 0, "NEW ONES PUT THERE GO INTO FREE SLOTS" },
        { 0, "GREEN * = THE SONG NOW, AMBER: CHANGED SINCE" },
    },
    {   /* the files page */
        { 0, "THE RENDERED FILES (REC IN THE SONG VIEW)" },
        { "SHIFT+JOIN", "THIS PAGE (FROM THE SONGS PAGE)" },
        { "BLUE / WHITE", "THE FILE" },
        { "CUT, CUT", "DELETE IT (PLAYBACK STOPS)" },
        { "JOIN, ORANGE PR", "BACK TO THE SONG VIEW" },
        { 0, "PLAYED BY: THE AUDIO TRACKS USING IT; THEY" },
        { 0, "SHOW GONE ONCE IT IS DELETED" },
    },
};

int py_help_on(void)
{
    return help != 0;
}

int py_help_key(int key)
{
    if (help) {                                 /* any key closes it */
        help = 0;
        return 1;
    }
    if (key != TE_KEY_SPEECH)
        return 0;
    help = (uint8_t)(1 + (py_view == PY_V_LIST ? 0 : py_view == PY_V_SLOTS ? 4 : py_view == PY_V_FILES ? 5 : chain_ok() ? 2 : live ? 3 : 1));
    return 1;
}

void py_help_knob(int knob, int d)
{
    int p = help - 1 + d;

    if (knob == 0 && help)                      /* blue: the page */
        help = (uint8_t)(1 + (p < 0 ? 0 : p >= HELP_PAGES ? HELP_PAGES - 1 : p));
}

void py_help_frame(void)
{
    int p = help ? help - 1 : 0, i, x;

    gfx_text(4, 4, help_title[p], C_WHITE, 2);
    gfx_rect(0, 17, gfx_w, 1, C_LINE);
    for (i = 0; i < HELP_LINES; i++) {
        const help_line *l = &help_page[p][i];
        int y = 21 + i * 9;

        if (l->key) {
            gfx_text7(4, y, l->key, C_WHITE);
            gfx_text7(100, y, l->text, C_GREY);
        } else if (l->text) {
            gfx_text7(4, y, l->text, C_AMBER);
        }
    }
    gfx_rect(0, 133, gfx_w, 1, C_LINE);
    x = gfx_text7(4, 140, "BLUE: PAGE ", C_GREY);
    x = gfx_dec7(x, 140, p + 1, C_WHITE);
    x = gfx_text7(x, 140, "/", C_GREY);
    gfx_dec7(x, 140, HELP_PAGES, C_GREY);
    gfx_text7(226, 140, "ANY KEY: BACK", C_GREY);
}
