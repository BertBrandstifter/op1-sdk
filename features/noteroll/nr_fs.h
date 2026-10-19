/* NOTE ROLL <-> TE's synth finger sequencer. Pure C, no TE calls; host-tested by test_nr_fs.c.
 *
 * A finger pattern in TE: up to 32 steps of 1/16, two notes per step ("hands"); a note is stored
 * as 53 + key + 12 x the finger screen's octave (TE adds the synth sound's octave when it plays),
 * -1 = empty; TE releases every note at the next step. NOTE ROLL keeps the stored values as its
 * pitches. */
#ifndef NR_FS_H
#define NR_FS_H

#include <stdint.h>
#include "nr_core.h"

#define NR_FS_STEPS     32              /* kOkMaxLength */
#define NR_FS_HANDS     2

typedef int32_t nr_fs_notes[NR_FS_STEPS][NR_FS_HANDS];

/* TE pattern (notes, length 1..32) -> NOTE ROLL pattern: length len, every note one step long
 * (TE's gate), velocity 100 */
void nr_fs_import(const nr_fs_notes notes, int len, nr_pattern *out);

/* NOTE ROLL pattern -> TE's notes (its compressed view, stock firmware): per step 0..31 inside the
 * pattern the lowest and the highest note (one note: hand 0 only), the rest -1 */
void nr_fs_summary(const nr_pattern *p, nr_fs_notes out);

/* TE changed its copy from `was` to `now` (step record, delete, clear on the finger screen): a
 * removed note goes from that step, an added one joins it (velocity 100, one step); a step TE
 * emptied (both hands, as its delete and clear do) is emptied with all its notes. Returns 1 if p
 * changed. */
int nr_fs_te_edit(nr_pattern *p, const nr_fs_notes was, const nr_fs_notes now);

/* The clock of one playing pattern, driven by TE's step events (one call per audio block): the
 * first event starts it at 0; every second event re-syncs (step in this window: in time; behind by
 * up to 2 blocks: catch up; ahead: hold; further: jump); without events for 3 steps it stops. */
typedef struct nr_fs_clock {
    uint8_t on;
    uint32_t k;                         /* step events since the start */
    uint32_t next;                      /* clock at the start of the next window */
    uint32_t idle;                      /* samples since the last event */
} nr_fs_clock;

int nr_fs_window(nr_fs_clock *c, int event, uint64_t step, uint32_t block, uint32_t *from, uint32_t *to);

#endif
