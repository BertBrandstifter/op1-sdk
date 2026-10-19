/* DRUM ROLL punch-in effects (spec: README.md "Punch-in effects"): the PO-20 arcade's 16 effects,
 * held on the black keys of TE's drum finger screen (10 plain, 6 with shift). Pure C, host-tested
 * by test_dr_punch.c.
 *
 * v2.0: three banks of 10 on the black keys (green press on TE's finger screen picks A / B / C):
 * A the PO-20 set, B arcade effects, C time + mix. Time effects (DRUM ROLL's playing patterns, the
 * last one pressed plays): RETRIG, RETRIG FAST, GLITCH, RISE, LASER, FALL, TRIPLET, LOOP BEAT,
 * BUILD, HALF TIME, DOUBLE TIME, REVERSE. Hit effects (stack): FILL, COIN, RND PITCH, OCT UP / DOWN,
 * ECHO, SHUFFLE, SOLOs, NO KICK, FADE, CUT. Audio effects (every drum voice, after TE's render and
 * DRUM ROLL's own post-process): LP sweep, HP sweep, BLINDS (1/16 gate), 8-BIT. */
#ifndef DR_PUNCH_H
#define DR_PUNCH_H

#include <stdint.h>
#include "dr_dsp.h"

enum {
    DR_PX_FILL, DR_PX_RETRIG, DR_PX_RETRIGF, DR_PX_GLITCH, DR_PX_RISE,          /* A: lower octave */
    DR_PX_LPSWEEP, DR_PX_HPSWEEP, DR_PX_BLINDS, DR_PX_SOLO1, DR_PX_SOLO12,      /*    upper octave */
    DR_PX_BIT8, DR_PX_LASER, DR_PX_FALL, DR_PX_COIN, DR_PX_RNDPITCH,            /* B: arcade */
    DR_PX_OCTUP, DR_PX_OCTDN, DR_PX_ECHO, DR_PX_REVERSE, DR_PX_SHUFFLE,
    DR_PX_HALF, DR_PX_DOUBLE, DR_PX_TRIPLET, DR_PX_LOOPBEAT, DR_PX_BUILD,       /* C: time + mix */
    DR_PX_SOLO14, DR_PX_SOLO58, DR_PX_NOKICK, DR_PX_FADE, DR_PX_CUT,
    DR_PX_KEYS
};
#define DR_PX_BANK      10              /* effects per bank (the 10 black keys) */
#define DR_PX_BANKS     3


extern const char *const dr_px_name[DR_PX_KEYS];
extern const uint8_t dr_px_icon[DR_PX_KEYS][8];     /* 8 x 8 pictograms, bit 7 = left */

/* REPEAT: clock window [a, a + n) -> the pattern times to play: before t0 as they are, from t0 on
 * the slice [t0, t0 + len) over and over. seg[i]: source start, length, offset in the window.
 * Returns the number of segments (1..3; len >= n). */
typedef struct dr_px_seg { uint32_t src, n, out; } dr_px_seg;
int dr_px_repeat(uint32_t a, uint32_t n, uint32_t t0, uint32_t len, dr_px_seg seg[3]);

/* GLITCH: the window cut into pieces of g samples (g >= n) on the clock's grid; a piece plays as it
 * is (1 in 4) or the piece g samples long from a random place on the g grid of its span (a bar):
 * the same piece index always the same place for one seed. Returns the segments (1..2). */
int dr_px_glitch(uint32_t a, uint32_t n, uint32_t g, uint32_t span, uint32_t seed, dr_px_seg seg[2]);

/* REVERSE: from t0 on, the steps (step samples each, step >= n) of every `steps` long bar in
 * reverse order (inside a step time runs forward); before t0 as it is. Returns the segments (1..3). */
int dr_px_reverse(uint32_t a, uint32_t n, uint32_t t0, uint32_t step, uint32_t steps, dr_px_seg seg[3]);

/* the audio stage of one drum voice */
typedef struct dr_px_voice {
    dr_voice lp, hp;                    /* the sweep filters (unity gain, no gate) */
    int32_t gg;                         /* gate gain Q15 (slewed) */
    int16_t hold;                       /* 8-BIT: the held sample */
    uint8_t hn;
} dr_px_voice;

/* per block, the same for every voice (Seq thread) */
typedef struct dr_px_ctl {
    dr_coef lp, hp;                     /* type DR_F_OFF: that filter is off */
    uint8_t gate, crush;
    uint32_t gpos, gper;                /* BLINDS: voice samples into its period at the block start,
                                         * the period (a 1/16 at the voice rate) */
} dr_px_ctl;

#define DR_PX_GATE_SLEW     512         /* Q15 per sample: 64 samples from open to shut */
#define DR_PX_CRUSH_MASK    (~0x7ff)    /* 8-BIT: 5 bits left */
#define DR_PX_CRUSH_HOLD    8           /* samples held (88.2 kHz / 8 = 11 kHz) */

void dr_px_start(dr_px_voice *v);       /* a new note on the voice */
void dr_px_process(dr_px_voice *v, int16_t *out, int n, const dr_px_ctl *c);    /* in place */

#endif
