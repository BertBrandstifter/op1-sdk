/* DRUM ROLL <-> TE's finger sequencer (spec: README.md "How it works"). Pure C, no TE calls;
 * host-tested by test_dr_fs.c.
 *
 * A finger pattern in TE: up to 32 steps of 1/16, two notes per step ("hands"), a note is a MIDI
 * note (drum: 53 + key, +24 per octave offset) or -1. DRUM ROLL shows such a pattern as an import,
 * keeps a summary of its own patterns in TE's copy, and follows TE's step events with a clock. */
#ifndef DR_FS_H
#define DR_FS_H

#include <stdint.h>
#include "dr_core.h"

#define DR_FS_PATTERNS  14              /* kOkNumPatterns */
#define DR_FS_STEPS     32              /* kOkMaxLength */
#define DR_FS_HANDS     2               /* kOkNumHands */
#define DR_FS_NOTE0     53              /* drum key 0 */

typedef int32_t dr_fs_notes[DR_FS_STEPS][DR_FS_HANDS];

/* drum key 0..23 and octave offset of a TE drum note; -1 if it is none (n < 5 or empty) */
int dr_fs_key(int32_t note, int *octave);

/* TE pattern (notes, length 1..32) -> DRUM ROLL pattern: one lane per key in order of first use
 * (the first 8), the other lanes on the lowest unused keys; every lane length len, rate 1/16; a note
 * = step on with the default parameters and TONE = 12 x its octave offset */
void dr_fs_import(const dr_fs_notes notes, int len, dr_pattern *out);

/* DRUM ROLL pattern -> TE's notes: per 1/16 step 0..31 the first two hits starting in that step
 * (lanes in order, muted lanes skipped, one note per key), as 53 + key; the rest -1 */
void dr_fs_summary(const dr_pattern *p, dr_fs_notes out);

/* TE wrote note `now` over `was` at 1/16 step `step` of a DRUM ROLL pattern (te_len: TE's length):
 * a removed note turns that step off in the first 1/16 lane on its key; an added note turns it on in
 * the first 1/16 lane on its key, else in the first lane without any step on (it gets the key, rate
 * 1/16 and length >= te_len). Returns 1 if p changed. */
int dr_fs_te_edit(dr_pattern *p, int step, int32_t was, int32_t now, int te_len);

/* TE emptied 1/16 step `step` (both hands -1: TE's delete / clear): that step off in every 1/16 lane.
 * Returns 1 if p changed. */
int dr_fs_te_clear_step(dr_pattern *p, int step);

/* The pattern length in 1/16 steps (1..DR_STEPS): every lane plays the same time span. A lane at
 * rate r gets round(len16 x its steps per 1/16) steps (1..DR_STEPS); the pattern's length is its
 * longest 1/16 or 1/32 lane in 1/16 steps (those hold any length exactly; none: the longest lane,
 * rounded), 1..DR_STEPS. set returns 1 if p changed. Steps past a lane's length are kept (shorten
 * then lengthen: they come back). */
int dr_fs_lane_len(int rate, int len16);
int dr_fs_length(const dr_pattern *p);
int dr_fs_set_length(dr_pattern *p, int len16);

/* The clock of one playing pattern, driven by TE's step events (one call per audio block):
 * the first event starts it at 0; every second event re-syncs (behind by <= 2 blocks: catch up,
 * ahead: hold, further: jump); without events for 3 steps it stops. */
typedef struct dr_fs_clock {
    uint8_t on;
    uint32_t k;                         /* step events since the start */
    uint32_t next;                      /* clock at the start of the next window */
    uint32_t idle;                      /* samples since the last event */
} dr_fs_clock;

/* this block's window [*from, *to) (may be empty); returns 0 when the clock is (or just went) off.
 * step = samples per 1/16 (16.16), block = samples per call */
int dr_fs_window(dr_fs_clock *c, int event, uint64_t step, uint32_t block, uint32_t *from, uint32_t *to);

#endif
