/* op1-sdk overlay graphics in UI coordinates (320 x 156 on the stock FE/94 panel).
 *
 * TE double-buffers the screen (0x02000000 / 0x03000000) and clears the back buffer before
 * every frame, so drawing straight into TE_FRAMEBUFFER does not stick. Draw from a hook that
 * runs right after TE has rendered a frame and before TE flushes the cache and presents it:
 *
 *   "call_sites": [{"site": "0x019ab8b2", "expect": "0x019b2674",
 *                   "c": "my_overlay", "after": true}],  "libs": ["gfx"]
 *
 *   void my_overlay(uint16_t *buf) { gfx_begin(buf); gfx_text(2, 2, "HELLO", GFX_WHITE, 2); }
 *
 * Mapping (GUI render thread 0x019ab880, canvas 0x01a2361c, text drawer 0x019c5c9a):
 * UI (x, y) -> buf[(GFX_W - 1 - x) * 256 + y], i.e. panel line = 319 - x, panel column = y.
 * Pixels are RGB565. Mapping, colour order and the frame hook were verified on a real OP-1
 * (examples/key_probe, 2026-10-11). */
#ifndef OP1_GFX_H
#define OP1_GFX_H

#include <stdint.h>

#define GFX_STRIDE  256                 /* pixels per panel line (512 bytes) */
#define GFX_RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define GFX_BLACK   GFX_RGB(0, 0, 0)
#define GFX_WHITE   GFX_RGB(255, 255, 255)
#define GFX_RED     GFX_RGB(255, 0, 0)
#define GFX_GREEN   GFX_RGB(0, 255, 0)
#define GFX_BLUE    GFX_RGB(0, 0, 255)

extern int gfx_w, gfx_h;                /* UI size, read from TE's canvas setup in gfx_begin */

void gfx_begin(uint16_t *buf);          /* target the frame TE just rendered */
void gfx_pixel(int x, int y, uint16_t c);
void gfx_rect(int x, int y, int w, int h, uint16_t c);
void gfx_disc(int cx, int cy, int r, uint16_t c);      /* filled circle */
/* 3x5 font (digits, A-Z, space - : . = / + ? * # '; lower case drawn as upper case), scale >= 1.
 * Returns the x after the last character (advance 4 * scale per character). */
int gfx_text(int x, int y, const char *s, uint16_t c, int scale);
int gfx_hex(int x, int y, uint32_t v, int digits, uint16_t c, int scale);
int gfx_dec(int x, int y, int32_t v, uint16_t c, int scale);
/* the shift key's arrow (5x5, filled, pointing up) at the text's size; returns the x after it */
int gfx_shift(int x, int y, uint16_t c, int scale);

#endif
