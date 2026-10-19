/* NOTE ROLL core (spec: README.md): the synth finger sequencer's patterns as DRUM ROLL-style step
 * patterns with chords: every step holds up to NR_VOICES notes (pitch, velocity, length), and its
 * own NUDGE, PROB and RATCH. Pure C, no TE calls; host-tested by test_nr_core.c.
 *
 * Time is the sequencer clock in samples at 44.1 kHz from the pattern's start; a step is a 1/16. */
#ifndef NR_CORE_H
#define NR_CORE_H

#include <stdint.h>

#define NR_STEPS        64
#define NR_VOICES       6               /* notes per step (a chord) */
#define NR_PATTERNS     14              /* the finger sequencer's patterns */
#define NR_NONE         0xff            /* empty voice */
#define NR_LENS         14
#define NR_LEN_DEF      4               /* 1 step */
#define NR_SR           44100
#define NR_SHAPES       12

typedef struct nr_note {
    uint8_t key;                        /* MIDI note 0..127, NR_NONE = empty */
    uint8_t vel;                        /* 1..100 */
    uint8_t len;                        /* index into nr_len8 */
    uint8_t pad;
} nr_note;

typedef struct nr_step {
    nr_note n[NR_VOICES];               /* sorted by pitch, empty voices last */
    int8_t nudge;                       /* -50..50 % of a step */
    uint8_t prob;                       /* 0..100 */
    uint8_t ratch;                      /* 1..4 */
    uint8_t pad;
} nr_step;

typedef struct nr_pattern {
    uint8_t len;                        /* steps 1..64 */
    uint8_t pad[3];
    nr_step s[NR_STEPS];
} nr_pattern;

extern const uint8_t nr_len8[NR_LENS];  /* held length in eighths of a step */
extern const char *const nr_len_name[NR_LENS];
extern const char *const nr_shape_name[NR_SHAPES];

void nr_step_init(nr_step *s);
void nr_pattern_init(nr_pattern *p);    /* 16 steps, empty */
int nr_voices(const nr_step *s);        /* notes in the step */
void nr_sort(nr_step *s);               /* by pitch, empty last, duplicates of a pitch dropped */

/* add a note (keeps the order; full or the pitch already there: nothing). Returns its voice or -1 */
int nr_add(nr_step *s, int key, int vel, int len);
void nr_remove(nr_step *s, int voice);

/* the voice's note to key (clamped 0..127; a pitch that is already in the step: unchanged).
 * Returns the note's voice after sorting. */
int nr_set_key(nr_step *s, int voice, int key);

/* chord shape sh (0 = single note) on the step's lowest note: the other notes are replaced by the
 * shape's, with the root's velocity and length. Returns 1 if the step changed. */
int nr_chord(nr_step *s, int sh);
int nr_shape_of(const nr_step *s);      /* the shape the notes form (0 single, -1 none) */
/* inversion: up = the lowest note an octave up, down = the highest an octave down */
int nr_invert(nr_step *s, int up);

/* samples per 1/16 (16.16) at bpm_x100 / 100 BPM; 0 if the tempo is 0 */
uint64_t nr_step_q16(uint32_t bpm_x100);

typedef struct nr_hit {
    int step, voice, hit;               /* hit 0 .. RATCH-1 */
    uint32_t k;                         /* step count since clock 0 */
    uint32_t at;                        /* clock samples */
    uint32_t hold;                      /* samples until the note-off */
    const nr_note *n;
} nr_hit;

typedef void (*nr_emit_fn)(void *ctx, const nr_hit *h);

/* every hit of p with from <= at < from + n (probability by a hash of seed and k: all notes of a
 * step agree). Returns the number of hits. */
int nr_schedule(const nr_pattern *p, uint32_t from, uint32_t n, uint64_t step, int swing,
                uint32_t seed, nr_emit_fn emit, void *ctx);

int nr_playhead(const nr_pattern *p, uint32_t t, uint64_t step);    /* step at clock t, -1 none */

#endif
