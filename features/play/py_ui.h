/* py_ui: what PLAY's views share (play.c: the track list; py_view.c: the song and chain views). */
#ifndef PY_UI_H
#define PY_UI_H

#include <stdint.h>
#include <op1/gfx.h>
#include "py_core.h"
#include "py_draw.h"

#define C_GREY      GFX_RGB(0x8a, 0x8a, 0x99)
#define C_DIM       GFX_RGB(0x40, 0x40, 0x48)
#define C_LINE      GFX_RGB(0x30, 0x30, 0x38)
#define C_SEL       GFX_RGB(0x1a, 0x1a, 0x22)
#define C_AMBER     GFX_RGB(0xff, 0xb0, 0x3d)
#define C_GREEN     GFX_RGB(0x3d, 0xdc, 0x97)
#define C_BLUE      GFX_RGB(0x6e, 0x8b, 0xff)
#define C_RED       GFX_RGB(0xff, 0x4f, 0x5e)
#define C_WHITE     GFX_WHITE

/* the footer every view shares: a line, then two text lines (5x7 font: 52 characters each) */
#define PY_FOOT_LINE    120
#define PY_FOOT_Y1      125
#define PY_FOOT_Y2      138

enum { PY_V_LIST, PY_V_SONG, PY_V_CHAIN, PY_V_SLOTS, PY_V_FILES };
extern volatile uint8_t py_view;                /* the view PLAY shows (session only) */
extern py_mixer py_mx;

/* play.c */
int py_rows(int *list);                         /* the tracks in list order (song column i = list[i]) */
void py_changed(int audio);                     /* the song was edited (2: AUDIO cells / song rows) */
void py_note(const char *msg);                  /* a short note in the footer */
const char *py_note_now(void);                  /* GUI: the note to draw this frame, 0 = none */
void py_draw_header(const char *title, int num, uint16_t col);
void py_hdr_fill(py_hm *h, const char *title, int num, uint16_t col);   /* the header's model: tempo, bar, transport */
extern volatile uint8_t py_layout;
const char *py_cpu_ctx(const char *pre);        /* the header's context: pre, then CPU nn while playing */              /* the list: 0 LANES, 1 ROWS (tape on the list toggles it) */
void py_req_pause(void);                        /* play while playing */
void py_req_stop(void);
void py_req_bar(int step);
void py_req_start(int row, int aud_track, int chain);   /* play from song row `row` at bar 1; or
                                                         * aud_track >= 0: that track auditions chain */
void py_req_cue(int t, int row);                /* live: track t jumps to that song row at its chain's end */
int py_cue_row(int t);                          /* the row track t is cued to, -1 = none */
void py_master_step(int d);
int py_sound_key(int key);                      /* keys 1-8 as a slot 0..7, -1 = another key */
void py_set_sound(int t, int slot);             /* SYNTH / DRUM track t on that sound / kit slot, at once */
int py_song_bars(int row);                      /* a render's default length from that song row */
void py_song_audio(void);                       /* main: the AUDIO tracks' segments from the start row */
void py_update_gains(void);
void py_seek_to(uint32_t g, int play);          /* main: stop, heads and rings at frame g, play or not */
uint32_t py_clock_bar(void);                    /* frames per bar */
void py_req_rescan(void);                       /* read the folders' files again (the song stays) */
void py_req_preview(int n);                     /* the files page: play render n alone (-1: stop) */
int py_preview_file(void);                      /* the render playing, -1 none */
uint32_t py_preview_pos(void);                  /* its frames played */
enum { PY_SL_NONE, PY_SL_SCAN, PY_SL_SAVE, PY_SL_LOAD, PY_SL_DELETE,
       PY_SL_DIFF /* before a load: which patterns would change (then the page asks, or loads) */,
       PY_SL_FSCAN /* the files page: the render folder read */, PY_SL_FDELETE /* render n deleted */ };

/* the files page: the render folder (play.c reads it, sorted by name) */
#define PY_RFILES   99
typedef struct py_rfile {
    char name[13];
    uint8_t ch;                                 /* 1 / 2, 0 = not a readable audio file */
    uint32_t size, frames, rate;
} py_rfile;
extern py_rfile py_rfiles[PY_RFILES];
extern volatile uint8_t py_nrfiles;
/* the songs page: a slot operation for the main thread; PY_SL_LOAD: keep = the patterns left as
 * they are (NOTE ROLL bits, DRUM ROLL bits << 16) */
void py_req_slot(int op, int n, uint32_t keep, int skeep);    /* keep: patterns, skeep: sounds */

/* py_render.c */
int py_rd_busy(void);                           /* a render runs or is about to */
void py_rd_request(int row, int bars);          /* render `bars` bars from song row `row` */
void py_rd_cancel(void);
int py_rd_percent(void);
const char *py_rd_name(void);                   /* the file being written */
int py_rd_audio(void);                          /* audio interrupt: 1 = a render owns this block */
int py_rd_main(void);                           /* main loop: 1 = a render runs */

/* py_view.c */
void py_v_enter(void);                          /* join: the song view (from the list, the chain view, another screen) */
void py_v_list(void);                           /* the tape key: the track list */
void py_v_key(int key);                         /* a key press in the song / chain view */
void py_v_knob(int knob, int d);
void py_v_frame(void);
int py_help_on(void);                           /* the help page is shown */
int py_v_fx_key(int key, int down, int shown);  /* live: a black key = a trigger effect; 1 = taken */
int py_help_key(int key);                       /* speech: help on / off; any other key closes it; 1 = taken */
void py_help_knob(int knob, int d);             /* blue turn: the page */
void py_help_frame(void);
int py_v_pattern(int *t);                       /* shift + blue press: the cursor's track and pattern (-1: none) */
void py_v_slots(void);                          /* shift + join: the songs page (save / load) */
void py_v_files(void);                          /* the songs page's shift + join: the files page */
void py_v_files_done(int op, int n, int r);     /* main: the render folder read / a render deleted (r < 0: failed) */
void py_v_slot_scan(int n);                     /* main: slot n's header into the page (-1: all) */
void py_v_slot_done(int op, int n, int r);      /* main: an operation's result (py_slot.h code) */
/* main: before loading slot n (r: py_slot.h code), these patterns would change (bit p of
 * diff[0] NOTE ROLL, diff[1] DRUM ROLL), the song plays these: the page asks which to change */
void py_v_slot_ask(int n, int r, const uint16_t diff[3], const uint16_t used[2], int sbad);

#endif
