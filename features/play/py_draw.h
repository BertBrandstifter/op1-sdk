/* py_draw: PLAY's screens from 0.17 (SCREENS.md): the track list as LANES or ROWS, the song view,
 * the header and footer every view shares. Drawn from plain view models: no TE addresses here,
 * so sim/preview.c draws the same code on the host. play.c / py_view.c fill the models. */
#ifndef PY_DRAW_H
#define PY_DRAW_H

#include <stdint.h>
#include <op1/gfx.h>
#include "py_song.h"

/* colours (the handover's tokens.json) */
#define D_TEXT      GFX_RGB(0xf2, 0xf2, 0xf2)
#define D_MUTED     GFX_RGB(0x8c, 0x8c, 0x8c)
#define D_LINE      GFX_RGB(0x1c, 0x1c, 0x1c)
#define D_LANE      GFX_RGB(0x18, 0x18, 0x18)
#define D_SEG_OFF   GFX_RGB(0x1f, 0x1f, 0x1f)
#define D_STEP_OFF  GFX_RGB(0x2e, 0x2e, 0x2e)
#define D_CTRL_OFF  GFX_RGB(0x44, 0x44, 0x44)
#define D_DISABLED  GFX_RGB(0x55, 0x55, 0x55)
#define D_EMPTY     GFX_RGB(0x3a, 0x3a, 0x3a)
#define D_SEL       GFX_RGB(0x10, 0x10, 0x18)   /* the design's #0C0C11, a step up for the panel */
#define D_PLAYROW   GFX_RGB(0x16, 0x16, 0x1e)
#define D_ORANGE    GFX_RGB(0xff, 0x7a, 0x3d)
#define D_BLUE      GFX_RGB(0x5b, 0x7c, 0xff)
#define D_GREEN     GFX_RGB(0x3b, 0xe0, 0x8f)
#define D_RED       GFX_RGB(0xff, 0x4d, 0x5e)
#define D_AMBER     GFX_RGB(0xff, 0xb0, 0x3d)
#define D_CUT_BG    GFX_RGB(0x40, 0x10, 0x14)
#define D_INS_BG    GFX_RGB(0x10, 0x30, 0x22)

#define D_HEAD_LINE 17                          /* the header's line (the views below start at 18) */
#define D_FOOT_LINE 130                         /* the new footer's line, text at D_FOOT_Y (3x5) */
#define D_FOOT_Y    134

uint16_t py_d_track_col(int i);                 /* list position i: blue, green, white, red, again */
uint16_t py_d_scale(uint16_t c, int a);         /* c * a / 256 (on black) */

/* text: the 3x5 font with `sp` px between characters (also '%', '<', '>', and '\1' = a middle dot);
 * returns the x after it; py_d_w3: its width */
int py_d_text3(int x, int y, const char *s, uint16_t c, int sp);
int py_d_w3(const char *s, int sp);
int py_d_text7(int x, int y, const char *s, uint16_t c, int sp);   /* the 5x7 font, tracked */
int py_d_w7(const char *s, int sp);
/* thin numerals h px high (0-9 A-F N O - . : / + and space): antialiased strokes; returns the x
 * after the last; py_d_wnum: the width */
int py_d_num(int x, int y, int h, const char *s, uint16_t c);
int py_d_wnum(int h, const char *s);
void py_d_fmt(char *out, int v);                /* v in decimal ("-9", "0") */
void py_d_hex2(char *out, int v);               /* "0F" */

/* shapes */
void py_d_ring(int cx, int cy, int r, uint16_t c);              /* 1 px circle round a pixel centre */
void py_d_dial(int cx, int cy, int r, int num, uint16_t c);     /* a track's dial: ring, number, white pointer */
void py_d_button(int cx, int cy, char letter, int on, uint16_t on_col);   /* M / S */
void py_d_frame(int x, int y, int w, int h, uint16_t c);
int py_d_chip(int x, int y, const char *s, uint16_t c);         /* bordered 5x7 text; returns its width */
int py_d_chip3(int x, int y, const char *s, uint16_t c);        /* the same, 3x5 text (9 high) */

/* the live meter: py_d_meter_feed every frame with track k's peak since the last frame (linear,
 * 32767 = 0 dBFS); py_d_meter draws its state: `segs` segments of `seg` px with `gap`, h high,
 * the fader marker at level_db under it (marker: 1, + 0 dB tick: 2) */
void py_d_meter_feed(int k, uint32_t peak, uint32_t blocks);   /* blocks: audio blocks since the last */
void py_d_meter_set(int k, int db16, int hold16);               /* (preview) */
int py_d_meter_lit(int k, int segs);            /* lit segments now */
void py_d_meter(int x, int y, int seg, int gap, int segs, int h, int k, int level_db, uint16_t col,
                int a, int marks);

/* ------------------------------------------------------------------ header + footer */

typedef struct py_hm {
    const char *title;
    uint16_t title_col;
    int num;                                    /* >= 0: two hex digits after the title */
    const char *ctx;                            /* the context label (3x5 grey), 0 = none */
    int bpm, bar, beat;                         /* bar / beat from 1 */
    int row;                                    /* >= 0: ROW (song view) */
    int loop_a, loop_b, loop_n;                 /* loop_n >= 2: the loop section, red */
    uint8_t state;                              /* 0 stop, 1 play, 2 rendering */
    uint8_t bare;                               /* the title, the loop and the transport only (SONG) */
    uint8_t nobar;                              /* no BAR (the track list) */
} py_hm;
void py_d_header(const py_hm *h);

/* a footer hint: an encoder ring (D_BLUE, ...) or a key name (white), then the action (grey) */
typedef struct py_hint { uint16_t ring; const char *key, *act; } py_hint;
#define PY_HINT_KEY 0                           /* ring = 0: a key name */
void py_d_footer(const py_hint *h, int n, int master_db, int with_master);
void py_d_footer_text(int x, const char *s, uint16_t c);        /* a line of its own (3x5) */
void py_d_footer_render(const char *name, int pc);              /* a render runs */

/* ------------------------------------------------------------------ the track list */

typedef struct py_lt {                          /* one track as the list shows it */
    uint8_t k;                                  /* the track (meter state) */
    uint8_t type;                               /* PS_AUDIO / PS_SYNTH / PS_DRUM */
    uint8_t mute, solo;
    int8_t level;                               /* dB */
    int8_t slot;                                /* SND / KIT 1..8 (0: none) */
    uint8_t slot_alt;                           /* the slot is a chain row's, not the track's own */
    uint8_t seq_song;                           /* chip: 0 PAT, 1 SNG */
    int8_t seq;                                 /* pattern 1..14, 0 = none */
    uint8_t pend;                               /* the chosen pattern waits for the playing one's end */
    uint8_t voices, eco, busy;                  /* SYNTH: cap 1 / 2 / 4 (0: not SYNTH) */
    uint8_t amode;                              /* AUDIO: 0 ONCE, 1 LOOP, 2 SONG */
    uint8_t under;                              /* AUDIO: the stream ran dry */
    uint16_t cpu;                               /* per mille, 0xffff = not shown */
    const char *name;                           /* engine or file */
    const char *err;                            /* red instead of the name, 0 = none */
} py_lt;

typedef struct py_lm {
    uint8_t layout;                             /* 0 LANES, 1 ROWS */
    uint8_t n, cursor;                          /* cursor == n: the ADD row */
    uint8_t add_room, add_type, add_ok;
    int8_t cut;                                 /* row armed for removal, -1 */
    uint8_t insert;                             /* insert pressed once */
    int8_t master_db;
    const char *note;                           /* footer: this instead of the hints, 0 = hints */
    uint16_t note_col;
    const char *note2;                          /* (white, after note) */
    const char *add_msg;                        /* why ADD cannot (or a hint), 0 = none */
    py_lt t[PS_TRACKS];
} py_lm;
void py_d_list(const py_lm *m);

/* ------------------------------------------------------------------ the song view */

typedef struct py_sc {                          /* a column */
    uint8_t k, type, mute, solo, starved;
    int8_t slot;                                /* SND / KIT 1..8 */
    const char *file;                           /* AUDIO */
    int16_t head;                               /* the row it plays, -1 */
    int16_t cue;                                /* live: the row it is cued to, -1 */
} py_sc;

typedef struct py_sm {
    const ps_song *s;
    uint8_t n, list[PS_TRACKS];
    uint8_t live, playing, row, col, blink;
    uint8_t *top, *left;                        /* the first row / column shown (kept by the caller) */
    int loop_a, loop_b, loop_n;
    py_sc c[PS_TRACKS];
    const char *note;                           /* footer: instead of the hints, 0 = hints */
    uint16_t note_col;
} py_sm;
void py_d_song(const py_sm *m);

#endif
