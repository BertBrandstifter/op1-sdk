/* RESO effect core: four tuned feedback-comb "strings" that resonate at the notes being played.
 * Pure C (no TE dependencies): runs on the OP-1 and in the host unit tests (test_reso_core.c).
 * Spec: README.md.
 *
 * FX rate 44.1 kHz. Strings are tuned from the synth voices' phase increments (TE's voice rate
 * 88.2 kHz, 2^32 = one cycle) through rs_chord, then rs_process runs the strings on a mono block
 * in place. Parameters are 8 int16 values 0..32767. Per-sample work is 32-bit integer maths (no
 * divides, no 64-bit); per-block set-up may use both. */
#ifndef RESO_CORE_H
#define RESO_CORE_H

#include <stdint.h>

enum {
    RS_DECAY = 0,   /* string feedback: short .. ~20 s ring */
    RS_TONE,        /* damping: dark .. bright */
    RS_CHORD,       /* voicing: PLAYED ROOT FIFTH MAJ MIN SUS4 MAJ7 MIN7 */
    RS_MIX,         /* dry .. wet (0 = dry, bit-exact) */
    RS_GLIDE,       /* retune slew: instant .. ~1 s */
    RS_OCTAVE,      /* strings -2 .. +2 octaves (middle = 0) */
    RS_DETUNE,      /* spread the strings up to +-25 cents */
    RS_DRIVE,       /* saturation in the strings */
    RS_PARAMS
};

#define RS_STRINGS  4
#define RS_LEN      2048                        /* samples per string: lowest note ~21.6 Hz */
#define RS_CHORDS   8
#define RS_VOICES   8                           /* most voices rs_chord looks at */

typedef struct rs_state {
    int16_t line[RS_STRINGS][RS_LEN];
    uint32_t wpos;
    uint32_t delay_q16[RS_STRINGS];             /* current (gliding) period, 16.16 samples */
    uint32_t target_q16[RS_STRINGS];            /* period the string glides to */
    int32_t lp[RS_STRINGS];                     /* damping lowpass state */
    int32_t err[RS_STRINGS];                    /* feedback rounding remainder (error feedback) */
    int32_t hp_x, hp_y;                         /* input DC blocker */
    int32_t out_x, out_y;                       /* wet DC blocker */
    int tuned;                                  /* targets set at least once */
} rs_state;

void rs_reset(rs_state *s);

/* Tune the strings from the synth voices: inc[v] = voice phase increment at 88.2 kHz, active[v]
 * nonzero while voice v sounds. With nothing sounding the strings keep their notes (they ring
 * on). Returns the number of sounding voices seen. */
int rs_chord(rs_state *s, const uint32_t *inc, const uint8_t *active, int voices, const int16_t *p);

/* Run n samples of a mono block in place. */
void rs_process(rs_state *s, int16_t *buf, int n, const int16_t *p);

/* which voicing a CHORD value selects (0 .. RS_CHORDS - 1) */
int rs_voicing(int16_t knob);

#endif
