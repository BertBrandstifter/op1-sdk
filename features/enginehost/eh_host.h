/* Engine host (spec: README.md): several SDK synth engines on TE's drwave engine.
 *
 * A drwave sound belongs to an SDK engine when knob slots 5-8 all hold (tag << 12) | v, the same
 * tag 1..6 in all four (v = 12 bits, that engine's page 2). Every such value is inside drwave's
 * ranges (5-8: 0..32767), so presets pass TE's boot-time patch validator and load on stock
 * firmware as drwave. Stock drwave presets never match (slot 5 = 32000 / 32767, 6-8 = 0).
 * Page 1 lives in slots 1-4 within drwave's ranges: slot 1 0..24575, 2 0..16383, 3 0..16383,
 * 4 0..32767 (engines use 0..EH_PAGE1_MAX in all four).
 *
 * Engines register from their "init" (idempotent table writes; init runs once before the first
 * hook, on whichever thread gets there first). Tags: 2 MONO 3 (mono), 3 STATION 1 (m1), 4 TRIO, 5 WHEEL, 6 WARP. 7 is never used (stock
 * slot-5 values 0x7d00 / 0x7fff carry tag 7). */
#ifndef EH_HOST_H
#define EH_HOST_H

#include <stdint.h>

#define EH_TAGS         7               /* tags 1..6 */
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

/* tag of a sound's knob block (1..6), 0 = not an SDK engine (plain drwave) */
int eh_tag(const volatile int16_t *k);

/* page-2 value v of slot 5-8 with a tag */
static inline int16_t eh_p2(int tag, int v) { return (int16_t)((tag << 12) | (v & EH_P2_MAX)); }

/* page 3 of every hosted engine: the current sound's FX knobs (eh_ui.c). eh_fx_knob: one knob
 * step inside TE's range for the FX type, saved (returns 1: consumed); eh_fx_draw: name + knob
 * names + bars from y (y .. y + 36; shows "FX IS OFF" / "NO FX" instead when there is none) */
int eh_fx_knob(int slot, int knob, int16_t diff);
void eh_fx_draw(int slot, int y);

#endif
