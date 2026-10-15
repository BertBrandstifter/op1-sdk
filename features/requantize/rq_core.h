/* Requantize core: a logged take (note events with tape positions) -> the same take quantized to
 * the tape grid, using the live quantizer's core (quant_core: SNAP/QUEUE, late window, chords,
 * held lengths). Pure C: runs on the OP-1 and in the host tests (test_rq_core.c). */
#ifndef RQ_CORE_H
#define RQ_CORE_H

#include <stdint.h>
#include "../quantcore/quant_core.h"

typedef struct rq_event {
    uint32_t pos;               /* tape position in samples */
    qz_ev ev;                   /* kind, key (note), data: whatever replay needs */
} rq_event;

enum { RQ_SNAP = 0, RQ_QUEUE = 1, RQ_STRICT = 2 };

/* Quantize in[0..n) (any order; sorted by pos, stable, first) into out (max entries), sorted by
 * position. Grid: step_q16 (16.16 samples) from tape position 0 = TE's bar grid. window: late
 * window; chord: note-ons this close to a chord's first note-on belong to it (QUEUE, STRICT).
 * mode RQ_SNAP / RQ_QUEUE: the live quantizer's rules. RQ_STRICT: the first chord goes to its
 * grid point (late-window rule), every following note or chord to the next grid step after the
 * previous one, pauses or not; each note keeps its held length, cut at the next chord's step
 * (min. QZ_MIN_LEN), the last chord keeps its full length. Returns the number of events written.
 * s is scratch (the quantizer state is large). */
int rq_quantize(rq_event *in, int n, rq_event *out, int max, qz_state *s,
                uint64_t step_q16, int mode, uint32_t window, uint32_t chord);

/* Stable insertion sort by position (takes are short and nearly sorted); offs_first: at equal
 * positions note-offs go before note-ons (a note cut exactly where the same key starts again). */
void rq_sort(rq_event *e, int n, int offs_first);

#endif
