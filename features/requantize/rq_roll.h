/* Requantize piano roll core (spec: requantize-roll.md): a take's log as a note list, the
 * render that quantizes the AUTO notes and keeps the FIXED ones, and the roll's edit operations.
 * Pure C: runs on the OP-1 and in the host tests (test_rq_roll.c). */
#ifndef RQ_ROLL_H
#define RQ_ROLL_H

#include <stdint.h>
#include "rq_core.h"

#define RN_MAX      1024
#define RN_FINE     220                 /* fine move / length step: 5 ms */

enum { RN_FIXED = 1, RN_DEL = 2, RN_SEL = 4, RN_OPEN = 8, RN_ADDED = 16 };   /* ADDED: in the roll */

typedef struct rq_note {
    uint32_t played, played_len;        /* as played (tape samples) */
    uint32_t start, len;                /* shown and played: FIXED = the user's, AUTO = last render */
    uint8_t data[6];                    /* replay data as logged (see requant.c log_event) */
    uint8_t key;                        /* pitch now (edited) */
    uint8_t key0;                       /* pitch as played: AUTO notes are quantized with it */
    uint8_t flags;
} rq_note;

/* Note list from logged events (any order, sorted in place). Note-on paired with the first open
 * note of its key; note-offs without note-on are dropped; notes still open end at `end`.
 * Returns the number of notes (at most max). Shown start/len = played until the first render. */
int rq_notes_build(rq_event *ev, int n, uint32_t end, rq_note *nt, int max);

/* Quantize the AUTO, not deleted notes (rq_quantize with the given settings, played positions,
 * played pitches) into their start/len, then write the replay events of every not deleted note
 * into out (sorted, note-offs first at equal positions). tmp holds 2 * n events.
 * Returns the number of events written. */
int rq_render(rq_note *nt, int n, rq_event *tmp, rq_event *out, int max, qz_state *s,
              uint64_t step_q16, int mode, uint32_t window, uint32_t chord);

/* Edits on the targets: the selected notes, or note `cur` when none is selected (cur -1: an empty
 * cursor, the selection only). */
enum {
    RE_MOVE,                            /* start += amount samples (FIXED) */
    RE_MOVE_STEP,                       /* start by amount grid steps (FIXED) */
    RE_LEN,                             /* length += amount samples (FIXED) */
    RE_LEN_STEP,                        /* end by amount grid steps (FIXED) */
    RE_PITCH,                           /* key += amount (0..127) */
    RE_SNAP,                            /* as RQ_CLEAN: start to the nearest grid point, length
                                         * whole grid steps (rounded, >= 1) (FIXED) */
    RE_PLAYED,                          /* all targets AUTO: FIXED as played; else back to AUTO */
    RE_DELETE,                          /* all targets deleted: restore; else delete */
    RE_SELECT,                          /* toggle the selection of `cur` only */
    RE_SELECT_ALL,                      /* any selected: clear the selection; else select all */
    RE_AUTO_ALL,                        /* every note back to AUTO (the whole take quantized) */
    RE_VEL,                             /* velocity += amount (1..127) */
};
/* Returns 1 if anything changed. */
int rq_edit(rq_note *nt, int n, int cur, int op, int32_t amount, uint64_t step_q16);

/* a note added in the roll: FIXED + ADDED at start, len (>= QZ_MIN_LEN), pitch key, replay data (as
 * a logged note's). Returns the new count (n when full). */
int rq_note_add(rq_note *nt, int n, int max, uint32_t start, uint32_t len, int key, const uint8_t data[6]);
/* note i out of the list (the later ones move down one); returns the new count */
int rq_note_remove(rq_note *nt, int n, int i);

/* ord[0..n) = note indices by start, then pitch, then index */
void rq_order(const rq_note *nt, int n, int *ord);

/* Grid point k from tape position 0 / index of the grid point at or before pos / the nearest
 * grid point */
uint32_t rq_grid_pt(uint64_t step_q16, uint32_t k);
uint32_t rq_grid_floor(uint64_t step_q16, uint32_t pos);
uint32_t rq_grid_nearest(uint64_t step_q16, uint32_t pos);

#endif
