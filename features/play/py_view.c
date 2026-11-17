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

#define VIS         7                           /* song rows on screen */
#define COL_X(i)    (28 + (i) * 36)
#define SROW_Y(r)   (34 + (r) * 12)
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
    case TE_KEY_JOIN:                           /* join always means the song view: stay */
        break;
    case TE_KEY_ORANGE_PUSH:                    /* (and the tape key, play.c) */
        py_view = PY_V_LIST;
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
        py_req_bar(key == TE_KEY_BACK ? -1 : 1);
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
    default:                                    /* orange: shift = master */
        if (shift)
            py_master_step(d);
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

    if (chain_ok())
        chain_key(key, shift);
    else
        song_key(key, shift);
}

void py_v_knob(int knob, int d)
{
    int shift = *TE_SHIFT_HELD != 0;

    if (!d)
        return;
    if (chain_ok())
        chain_knob(knob, d, shift);
    else
        song_knob(knob, d, shift);
}

int py_v_pattern(int *t)
{
    int list[PS_TRACKS], n, c;
    const ps_head *h;

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

static void song_frame(void)
{
    static uint8_t blink;
    int list[PS_TRACKS], n, t = cursor_track(list, &n), i, r, x;
    int row = s_row, top = s_top;

    py_draw_header(live ? "LIVE" : "SONG", -1, live ? C_RED : C_WHITE);
    gfx_rect(0, PY_FOOT_LINE, gfx_w, 1, C_LINE);
    blink++;
    top = row < top ? row : row >= top + VIS ? row - VIS + 1 : top;
    s_top = (uint8_t)top;
    for (i = 0; i < n; i++) {                   /* the columns' names */
        const ps_track *tr = &py_song.trk[list[i]];

        x = gfx_dec7(COL_X(i), 22, i + 1, tr->mute ? C_DIM : C_WHITE);
        gfx_text7(x, 22, tr->type == PS_AUDIO ? "AUD" : tr->type == PS_SYNTH ? "SYN" : "DRM",
                  tr->mute ? C_DIM : py_pat_starved(list[i]) ? C_RED : colour(tr->type));   /* red: BUSY */
        if (tr->solo)
            gfx_rect(COL_X(i), 30, 24, 1, C_AMBER);
    }
    for (r = 0; r < VIS; r++) {
        int y = SROW_Y(r), sr = top + r;

        if (sr == row)
            gfx_rect(0, y - 3, gfx_w, 12, C_SEL);
        gfx_hex7(4, y, (uint32_t)sr, 2, sr == row ? C_WHITE : C_GREY);
        for (i = 0; i < n; i++) {
            int k = list[i], audio = py_song.trk[k].type == PS_AUDIO;
            uint8_t v = py_song.cell[sr][k];
            int here = head_row(k) == sr;
            uint16_t ink = here ? GFX_BLACK : C_WHITE;

            x = COL_X(i);
            if (here)
                gfx_rect(x - 2, y - 2, 16, 11, colour(py_song.trk[k].type));
            if (v == PS_EMPTY) {
                if (sr == 0 || py_song.cell[sr - 1][k] != PS_EMPTY)
                    gfx_text7(x, y, "--", C_DIM);       /* the column ends here */
            } else if (audio) {
                gfx_text7(x, y, v == PS_ON ? "ON" : "OF", here ? ink : v == PS_ON ? C_WHITE : C_GREY);
            } else if (v == PS_REST) {
                gfx_text7(x, y, "OF", here ? ink : C_GREY);        /* a rest: one silent bar */
            } else {
                gfx_hex7(x, y, v, 2, v < PS_CHAINS && py_song_chain_empty(&py_song, v) && !here ? C_GREY : ink);
            }
            if (py_mx.playing && py_cue_row(k) == sr && (blink & 8))    /* cued: it jumps here */
                frame_rect(x - 3, y - 3, 18, 13, colour(py_song.trk[k].type));
            if (sr == row && i == s_col)
                frame_rect(x - 4, y - 4, 20, 15, C_WHITE);
        }
    }
    if (rd_ask) {                               /* rec pressed: the render is asked */
        uint32_t tenths = (uint32_t)rd_bars * py_clock_bar() / 26214u;   /* stereo: 4 bytes a frame */

        x = gfx_text7(4, PY_FOOT_Y1, "RENDER ", C_AMBER);
        x = gfx_dec7(x, PY_FOOT_Y1, rd_bars, C_WHITE);
        x = gfx_text7(x, PY_FOOT_Y1, " BARS FROM ROW ", C_AMBER);
        x = gfx_hex7(x, PY_FOOT_Y1, (uint32_t)row, 2, C_WHITE);
        x = gfx_text7(x, PY_FOOT_Y1, " - UP TO ", C_GREY);
        x = gfx_dec7(x, PY_FOOT_Y1, (int32_t)(tenths / 10u), C_GREY);
        x = gfx_text7(x, PY_FOOT_Y1, ".", C_GREY);
        x = gfx_dec7(x, PY_FOOT_Y1, (int32_t)(tenths % 10u), C_GREY);
        gfx_text7(x, PY_FOOT_Y1, " MB", C_GREY);
        gfx_text7(4, PY_FOOT_Y2, "WHITE: BARS  REC: START  ANY OTHER KEY: CANCEL", C_GREY);
        return;
    }
    if (!footer_note("SHIFT + CUT AGAIN: DELETE THIS SONG ROW")) {
        uint8_t v = t >= 0 ? py_song.cell[row][t] : PS_EMPTY;

        if (t < 0) {
            gfx_text7(4, PY_FOOT_Y1, "NO TRACKS: ADD THEM ON THE LIST - TAPE", C_GREY);
        } else if (py_song.trk[t].type == PS_AUDIO) {
            gfx_text7(4, PY_FOOT_Y1, v == PS_ON ? "ON: THE FILE ONCE" : v == PS_OF ? "OF: ONE BAR SILENT"
                      : py_song_col_used(&py_song, t) ? "THE COLUMN ENDS HERE" : "NO CELLS: THE TRACK PLAYS FREE",
                      C_WHITE);
        } else if (v < PS_CHAINS) {
            int other = other_user(py_song_chain_users(&py_song, v), t, list, n);

            x = gfx_text7(4, PY_FOOT_Y1, "CHAIN ", C_WHITE);
            x = gfx_hex7(x, PY_FOOT_Y1, v, 2, C_WHITE);
            if (py_song_chain_empty(&py_song, v)) {
                gfx_text7(x, PY_FOOT_Y1, "  NO ROWS YET", C_GREY);
            } else if (other) {
                x = gfx_text7(x, PY_FOOT_Y1, "  ALSO ON TRACK ", C_GREY);
                gfx_dec7(x, PY_FOOT_Y1, other, C_GREY);
            }
        } else if (v == PS_REST) {
            gfx_text7(4, PY_FOOT_Y1, "OF: A REST, ONE BAR SILENT", C_WHITE);
        } else {
            gfx_text7(4, PY_FOOT_Y1, "EMPTY: THE COLUMN ENDS HERE", C_GREY);
        }
        gfx_text7(232, PY_FOOT_Y1, "SPEECH: HELP", C_DIM);
    }
    if (live)
        gfx_text7(4, PY_FOOT_Y2, "PLAY: CUE ROW  WHITE PRESS: CUE CELL  REC: RENDER", C_GREY);
    else if (t >= 0 && py_song.trk[t].type == PS_AUDIO)
        gfx_text7(4, PY_FOOT_Y2, "WHITE PRESS: ON / OF  REC: RENDER", C_GREY);
    else if (t >= 0 && py_song.cell[row][t] < PS_CHAINS)
        gfx_text7(4, PY_FOOT_Y2, "WHITE PRESS: OPEN  SHIFT: CLONE  REC: RENDER", C_GREY);
    else
        gfx_text7(4, PY_FOOT_Y2, "WHITE PRESS: LAST CHAIN  SHIFT: NEW  REC: RENDER", C_GREY);
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

void py_v_frame(void)
{
    if (chain_ok())
        chain_frame();
    else
        song_frame();
}

/* ------------------------------------------------------------------ help (speech) */

typedef struct help_line { const char *key, *text; } help_line;     /* key 0: text over the full width */

#define HELP_PAGES  4
#define HELP_LINES  12

static const char *const help_title[HELP_PAGES] = { "HELP: PLAY", "HELP: SONG", "HELP: CHAIN", "HELP: LIVE" };

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
        { "< >", "ONE BAR BACK / FORWARD" },
        { "JOIN", "THE SONG VIEW - THE TRACKER" },
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
        { "PLAY", "FROM THE CURSOR ROW, BAR 1" },
        { "SHIFT+PLAY", "CUE THE ROW: AT THE CHAIN'S END" },
        { "REC, REC", "RENDER THE SONG TO AN AIF FILE" },
        { "SHIFT+REC", "LIVE MODE ON / OFF" },
        { "KEYS 1-8", "SOUND / KIT 1-8 OF THE CURSOR TRACK" },
        { "TAPE, ORANGE PR", "BACK TO THE TRACK LIST" },
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
    help = (uint8_t)(1 + (py_view == PY_V_LIST ? 0 : chain_ok() ? 2 : live ? 3 : 1));
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
        } else {
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
