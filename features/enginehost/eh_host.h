/* Engine host (spec: README.md): several SDK synth engines on TE's drwave engine.
 *
 * A drwave sound belongs to an SDK engine when knob slots 5-8 all hold (tag << 12) | v, the same
 * tag 1..7 in all four (v = 12 bits, that engine's page 2). Extended tags 8..13 (v1.4, tags 1..7
 * were full): slots 5-7 carry 7, slot 8 carries tag - 7 (1..6); eh_p2s builds them. Every such value is inside drwave's
 * ranges (5-8: 0..32767), so presets pass TE's boot-time patch validator and load on stock
 * firmware as drwave. Stock drwave presets never match (slot 5 = 32000 / 32767, 6-8 = 0: tag 7 in
 * slot 5 alone is not enough, all four must carry it).
 * Page 1 lives in slots 1-4 within drwave's ranges: slot 1 0..24575, 2 0..16383, 3 0..16383,
 * 4 0..32767 (engines use 0..EH_PAGE1_MAX in all four).
 *
 * Engines register from their "init" (idempotent table writes; init runs once before the first
 * hook, on whichever thread gets there first). Tags: 1 PARTY (PSS-11) (pss11), 2 MONO 3 (mono),
 * 3 STATION 1 (m1), 4 TRIO, 5 WHEEL, 6 WARP, 7 MALLET (stock slot-5 values 0x7d00 / 0x7fff carry
 * tag 7, but their slots 6-8 are 0: checked on all 16 factory drwave presets); extended: 8 ACID 3
 * (acid), 9 PALETTE (palette), 10..13 free. */
#ifndef EH_HOST_H
#define EH_HOST_H

#include <stdint.h>

#define EH_TAGS         14              /* tags 1..7, extended 8..13 */
#define EH_PAGE1_MAX    16383           /* page 1 in slots 1-4 */
#define EH_P2_MAX       0x0fff          /* page 2 in slots 5-8, below the tag */
#define EH_FRESH        0x01            /* render flag: first block of a new note on this voice */
#define EH_BLOCK        0x02            /* render flag: first voice rendered in this audio block */
#define EH_VOICES       6

/* render one voice: vi = voice 0..5, out = 128 samples at 88.2 kHz, knobs = the sound's 8 stored
 * values (+ TE's LFO on slots 1-4), inc = TE's per-sample pitch increments, flags = EH_* */
typedef void (*eh_render_fn)(int vi, int16_t *out, const int16_t *knobs, const uint32_t *inc,
                             int flags);

void eh_register(int tag, eh_render_fn render);

/* tag of a sound's knob block (1..13), 0 = not an SDK engine (plain drwave) */
int eh_tag(const volatile int16_t *k);

/* page-2 value v of slot 5-8 with a tag */
static inline int16_t eh_p2(int tag, int v) { return (int16_t)((tag << 12) | (v & EH_P2_MAX)); }

/* the same for any tag (1..13): s = 0..3 for knob slots 5..8 (extended tags: 7 7 7 tag-7) */
static inline int16_t eh_p2s(int tag, int s, int v)
{
    return eh_p2(tag < 8 ? tag : s < 3 ? 7 : tag - 7, v);
}

/* page 3 of every hosted engine: the current sound's FX knobs (eh_ui.c). eh_fx_knob: one knob
 * step inside TE's range for the FX type, saved (returns 1: consumed); eh_fx_draw: name + knob
 * names + bars from y (y .. y + 36; shows "FX IS OFF" / "NO FX" instead when there is none) */
int eh_fx_knob(int slot, int knob, int16_t diff);
void eh_fx_draw(int slot, int y);

/* the engine screens' dot-matrix displays (eh_dots.c): cells of 5 x 7 dots (font: eh_font57.h), unlit
 * dots drawn in `off`. A cell is 4 * pitch + dot pixels wide, 6 * pitch + dot high; dot 3 = a round
 * lamp (corners dark). eh_lcd_blank: every cell of rows 0 .. rows-1 unlit; eh_lcd_text: s into row
 * `row` from cell `col` (cells past cols cut off), lit dots in `on`, returns the cell after it;
 * eh_lcd_w: the cells' width in pixels */
typedef struct eh_lcd {
    int16_t x, y;                       /* the first cell's top-left dot */
    uint8_t cols;                       /* cells per row */
    uint8_t pitch, dot;                 /* dot spacing, dot size (px) */
    uint8_t adv, line;                  /* cell advance, row advance (px) */
    uint16_t on, off;                   /* lit (eh_lcd_blank), unlit dots */
} eh_lcd;

void eh_lcd_blank(const eh_lcd *d, int rows);
int eh_lcd_text(const eh_lcd *d, int row, int col, const char *s, uint16_t on);
int eh_lcd_w(const eh_lcd *d);
/* rounded shapes: eh_rrect filled (radius r), eh_frame a t-pixel frame in c around fill */
void eh_rrect(int x, int y, int w, int h, int r, uint16_t c);
void eh_frame(int x, int y, int w, int h, int r, int t, uint16_t c, uint16_t fill);

/* antialiased shapes (eh_aa.c): edges blended into the frame by their coverage. eh_aa_begin after
 * gfx_begin (the same buffer); eh_aa_dy is added to every y. eh_aa_line: round ends, half width hw;
 * eh_aa_ellipse: radii a, b, a ring of half width hw or filled (hw < 0), steps s0 .. s1 of 128 a
 * turn (0 = right, 32 = down; s1 - s0 <= 64, or >= 128 = whole), dashed (on 2 .. 6 of every 8):
 * both in 1/16 px, kept and played back while drawn the same again (eh_aa_forget drops them).
 * eh_aa_frame: outline t px wide, corner radius r; eh_aa_box: filled (both whole px).
 * eh_aa_plot: one pixel at coverage a (0..256) */
extern int eh_aa_dy;
void eh_aa_begin(uint16_t *buf);
void eh_aa_plot(int x, int y, uint16_t c, int a);
void eh_aa_line(int x0, int y0, int x1, int y1, int hw, uint16_t c);
void eh_aa_ellipse(int cx, int cy, int a, int b, int hw, int s0, int s1, int dash, uint16_t c);
void eh_aa_frame(int x, int y, int w, int h, int r, int t, uint16_t c);
void eh_aa_box(int x, int y, int w, int h, int r, uint16_t c);
void eh_aa_forget(void);

#endif
