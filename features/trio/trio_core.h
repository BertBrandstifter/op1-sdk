/* TRIO voice: three oscillators (triangle, saw, pulse, pitched noise and the AND-combined waves)
 * with ring modulation and hard sync into a gritty multimode filter, in the style of the sound chips
 * of early 8-bit home computers. Pure C (no TE dependencies): runs on the OP-1 and in the host unit
 * tests (test_trio_core.c). Spec: README.md.
 *
 * Renders TE's voice rate (88.2 kHz, 128 samples per block); osc 1 follows TE's per-sample pitch
 * increments, osc 2 / 3 are set per block from it (interval + detune). Parameters are 12 values
 * 0..32767. Output is a raw, full-length note (TE's envelope shapes it afterwards). */
#ifndef TRIO_CORE_H
#define TRIO_CORE_H

#include <stdint.h>

enum {
    TR_WAVE = 0,    /* 16 oscillator sets */
    TR_CUTOFF,      /* 30 Hz .. 12 kHz */
    TR_RES,         /* resonance (gritty at the top) */
    TR_PW,          /* pulse width 50 % .. 3 % */
    TR_INT2,        /* osc 2 interval -24 .. +24 semitones (centre = unison) */
    TR_INT3,        /* osc 3 interval */
    TR_DETUNE,      /* osc 2 up, osc 3 down, 0 .. 50 cents */
    TR_MODE,        /* filter: LP BP HP NOTCH */
    TR_FENV,        /* filter envelope: cutoff up by up to 6 octaves at note on, decaying */
    TR_FDECAY,      /* 5 ms .. 2 s */
    TR_DRIVE,       /* input drive into the filter (0 = clean) */
    TR_NOISE,       /* white noise mixed in */
    TR_PARAMS
};

#define TR_SETS     16
#define TR_MODES    4
#define TR_SR       88200

typedef struct tr_voice {
    uint32_t ph[3];             /* oscillator phases */
    uint32_t lfsr;              /* noise */
    int16_t nz[3];              /* pitched noise: held value per oscillator */
    int32_t lp, bp;             /* filter states */
    int32_t env;                /* filter envelope, Q15 */
} tr_voice;

void tr_note_on(tr_voice *v);
void tr_render(tr_voice *v, int16_t *out, int n, const uint32_t *inc, const int16_t *p);

int tr_set_of(int16_t knob);
int tr_mode_of(int16_t knob);
int tr_semis_of(int16_t knob);          /* -24 .. +24 */
extern const char *const tr_set_names[TR_SETS];
extern const char *const tr_mode_names[TR_MODES];

#endif
