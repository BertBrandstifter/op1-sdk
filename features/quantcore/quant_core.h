/* Input quantizer core: grid maths, timing rule, pending queue. Pure C, no TE dependencies,
 * so it builds for the OP-1 and natively for host unit tests (test_quant_core.c).
 * Spec: features/quantize/README.md. Time is an audio-sample position (uint32, wraps; all comparisons use
 * signed differences). */
#ifndef QUANT_CORE_H
#define QUANT_CORE_H

#include <stdint.h>

#define QZ_QUEUE    64          /* note-ons may use at most half, so their note-offs always fit */
#define QZ_KEYS     256
#define QZ_MIN_LEN  64          /* shortest note in samples (one audio block) */

enum { QZ_ON = 1, QZ_OFF = 2 };
enum { QZ_NOW = 0, QZ_HELD = 1 };

typedef struct qz_ev {
    uint8_t kind;               /* QZ_ON / QZ_OFF */
    uint8_t key;                /* identity for ordering: note on/off of the same key stay in order */
    uint8_t data[6];            /* opaque to the core: what the glue needs to replay the event */
} qz_ev;

typedef struct qz_slot {
    qz_ev ev;
    uint32_t due;
    uint32_t seq;               /* insertion order, keeps equal due times stable */
} qz_slot;

typedef struct qz_state {
    uint64_t step_q16;          /* grid step in samples, 16.16 fixed point (0 = no grid) */
    uint32_t origin;            /* sample position of one grid point (e.g. a bar start) */
    uint32_t window;            /* late window in samples (capped at step/4 when used) */
    int queue;                  /* queue mode: each note-on takes the next free grid slot */
    uint32_t chord;             /* queue mode: note-ons this close to the previous one share its slot */
    int have_on;
    uint32_t last_on_in;        /* input time of the first note-on of the current chord (queue mode) */
    uint32_t last_on_due;       /* when it plays */
    uint32_t last_on_slot;      /* the grid point it belongs to */
    int32_t shift[QZ_KEYS];     /* per key: how far its last note-on was moved (samples) */
    uint32_t on_due[QZ_KEYS];   /* per key: when that note-on plays */
    uint8_t on_valid[QZ_KEYS];
    uint32_t idle;              /* phrase start after this much silence (0 = off) */
    uint32_t last_input;        /* time of the last note-on/off offered */
    int anchored;               /* grid follows `anchor` instead of `origin` */
    uint32_t anchor;            /* grid point set by the first note of the current phrase */
    uint32_t emit_due;          /* while an emit callback runs: the due time of that event */
    uint32_t seq;
    int count;
    qz_slot q[QZ_QUEUE];
} qz_state;

typedef void (*qz_emit_fn)(const qz_ev *ev, void *ctx);

void qz_init(qz_state *s);
void qz_set_grid(qz_state *s, uint64_t step_q16, uint32_t origin);
void qz_set_window(qz_state *s, uint32_t samples);

/* Queue mode: notes played faster than the grid are not stacked on one grid point but played one
 * per grid step in the order played; note-ons within `chord` samples of the first note-on of the
 * current chord share its slot (chords stay chords, fast runs still split). Note-offs keep the
 * usual rules. */
void qz_set_queue(qz_state *s, int on, uint32_t chord);
/* change the chord window only (keeps the queue position) */
void qz_set_chord(qz_state *s, uint32_t chord);

/* Phrase start: a note-on that comes after at least `samples` without any note-on/off input and
 * with nothing queued plays immediately and becomes a grid point: the grid of this phrase counts
 * from it, until the next phrase start. 0 = off (grid always follows qz_set_grid's origin). */
void qz_set_idle(qz_state *s, uint32_t samples);

/* Grid point at or before t (prev) and the one after it (next). Requires step_q16 != 0. */
void qz_grid_around(const qz_state *s, uint32_t t, uint32_t *prev, uint32_t *next);

/* Offer an event at time now. QZ_NOW: play it immediately yourself. QZ_HELD: it is queued and
 * comes back through qz_poll/qz_flush. Note-ons go to the grid; a note-off is moved by exactly
 * as much as its note-on was, so every note keeps the length it was held (>= QZ_MIN_LEN).
 * With no grid (step 0) or a full queue: QZ_NOW (but never ahead of a queued event of the
 * same key). */
int qz_input(qz_state *s, uint32_t now, const qz_ev *ev);

/* Emit every queued event with due <= now, in due order (stable). Returns the number emitted.
 * During each callback s->emit_due holds that event's due time. */
int qz_poll(qz_state *s, uint32_t now, qz_emit_fn emit, void *ctx);

/* Emit everything queued, in due order (quantizer turned off). */
int qz_flush(qz_state *s, qz_emit_fn emit, void *ctx);

/* Grid step in 16.16 samples for a note value: beats_num/beats_den of a quarter note,
 * e.g. 1/16 = (1, 4), 1/8T = (1, 3), at bpm_x100 (120 BPM = 12000) and sample rate sr. */
uint64_t qz_step_q16(uint32_t bpm_x100, uint32_t sr, uint32_t beats_num, uint32_t beats_den);

#endif
