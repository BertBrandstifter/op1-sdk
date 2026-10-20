/* MONO voice: a classic 70s mono synth voice: three oscillators (range, wave, level each; osc 2 / 3 tuned against osc 1),
 * noise, a linear 4-pole ladder (self-oscillation at the top of EMPHASIS, its level limited) with
 * its contour, the loudness contour, glide. Pure C (no TE dependencies): runs on the OP-1 and in
 * the host unit tests (test_mo_core.c). Spec: README.md.
 *
 * Renders TE's voice rate (88.2 kHz, 128 samples per block) from TE's per-sample pitch increments
 * (their block mean). Output is the note through its own loudness contour; TE's envelope shapes it
 * afterwards as for every engine. */
#ifndef MO_CORE_H
#define MO_CORE_H

#include <stdint.h>

#define MO_SR       88200
#define MO_RANGES   6           /* LO (64') 32' 16' 8' 4' 2'; 8' = the pitch TE plays */
#define MO_WAVES    6           /* triangle, shark (osc 3: reverse saw), saw, square, wide, narrow */
#define MO_FREQS    31          /* osc 2 / 3 frequency steps, MO_UNISON = osc 1's pitch */
#define MO_UNISON   15
#define MO_LEVELS   16          /* mixer levels 0..15 */
#define MO_TIMES    32          /* contour times 1 ms .. 10 s */
#define MO_GLIDES   16          /* off, 10 ms .. 2 s */
#define MO_KBDS     4           /* filter keyboard control: off, 1/3, 2/3, full */

enum { MO_TRI, MO_SHARK, MO_SAW, MO_SQUARE, MO_WIDE, MO_NARROW };

typedef struct mo_params {
    int32_t cutoff;             /* 0..32767: 41 Hz .. (12.3 kHz), 1 octave per 3277 */
    int32_t emph;               /* 0..32767: emphasis, self-oscillates from ~0.91 */
    int32_t contour;            /* 0..32767: contour amount, up to +4.95 kHz at its peak */
    uint8_t glide;              /* 0..MO_GLIDES - 1 */
    uint8_t range[3];           /* 0..MO_RANGES - 1 */
    uint8_t wave[3];            /* 0..MO_WAVES - 1 */
    uint8_t freq[2];            /* osc 2 / 3: 0..MO_FREQS - 1 */
    uint8_t vol[4];             /* osc 1, 2, 3, noise: 0..MO_LEVELS - 1 */
    uint8_t fatt, fdec, fsus;   /* filter contour: attack, decay 0..MO_TIMES - 1, sustain 0..15 */
    uint8_t latt, ldec, lsus;   /* loudness contour */
    uint8_t pink;               /* noise colour: 0 white, 1 pink */
    uint8_t kbd;                /* 0..MO_KBDS - 1 */
    uint8_t release;            /* 0: key up leaves the contours to TE's envelope; 1: release with the DECAY times */
} mo_params;

typedef struct mo_env {
    int32_t e;                  /* level, Q15 */
    int32_t r;                  /* stage ramp 0 .. 1 << 24 */
    int32_t from;               /* level the stage started at */
    uint8_t stage;              /* 0 off, 1 attack, 2 decay / sustain, 3 release */
} mo_env;

typedef struct mo_voice {
    uint32_t ph[3];             /* oscillator phases */
    uint32_t lfsr;              /* noise */
    int32_t pink;               /* pink noise: white through a one-pole low-pass */
    int32_t lad[6];             /* ladder: stages 1-4, stage 4 before, the averaged output */
    mo_env fenv, lenv;          /* filter / loudness contour */
    int32_t glide, gstep;       /* pitch offset still to glide away / per block, octaves Q16 */
    int32_t cur;                /* log2 of the last effective increment, Q16 (where a glide starts) */
    int32_t lgain;              /* loudness at the end of the last block (per-sample ramp) */
    int32_t key;                /* MIDI key of the note (screen), from its pitch */
    uint8_t sounding;           /* a note has been played (a glide has a start) */
} mo_voice;

void mo_voice_init(mo_voice *v);
/* a new note (inc = its pitch): glide from v->cur; both contours start (from where they are) */
void mo_note(mo_voice *v, uint32_t inc, const mo_params *p);
/* the note's key is up: RELEASE on = both contours release with their DECAY times; off = they go on
 * (TE's envelope ends the note, as for notes without a key) */
void mo_release(mo_voice *v, const mo_params *p);
void mo_render(mo_voice *v, int16_t *out, int n, const uint32_t *inc, const mo_params *p);
/* the filter + loudness part of mo_render on a given mixer signal (tests feed noise); key = the
 * note's position for keyboard control, MIDI key Q16 */
void mo_filter(mo_voice *v, const int32_t *acc, int16_t *out, int n, int32_t key, const mo_params *p);

/* screen / bank helpers */
int mo_cents(int freq);                         /* osc 2 / 3 step -> cents */
uint32_t mo_time_10us(int t);                   /* contour time step -> 0.1 ms */
int mo_glide_ms(int g);                         /* glide step -> ms (0 = off) */
int mo_level_permille(int l);                   /* mixer level -> 0..1000 */

/* log2 of a 32-bit value / 2^x, both Q16 */
int32_t mo_log2(uint32_t x);
uint32_t mo_exp2(int32_t l);

#endif
