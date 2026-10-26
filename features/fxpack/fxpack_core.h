/* FXPACK effect core: fifteen small effects behind one host FX (spec: README.md). Pure C (no
 * TE dependencies): runs on the OP-1 and in the host unit tests (test_fxpack_core.c).
 *
 * FX rate 44.1 kHz, mono blocks in place; a stereo host runs one state per channel (ch 1 = right:
 * LFOs offset by SPREAD, own noise). Parameters: 8 int16 values 0..32767; p[0..3] page 1, p[4] the
 * effect type (not read here: fp_reset picks the effect), p[5..7] page 2. Per-sample work is 32-bit
 * integer maths; coefficients are set once per <= 64-sample chunk (a few divides at most).
 *
 * TYPE: the stored value picks one of 32 fixed slots (v >> 10), each slot one effect; every TYPE
 * value ever stored (v0.4's seven, CHORUS, BITCRUSH) keeps its effect, new effects take free
 * slots. */
#ifndef FXPACK_CORE_H
#define FXPACK_CORE_H

#include <stdint.h>

enum {
    FP_PHASER = 0,  /* RATE DEPTH FEEDBACK MIX | CENTER STAGES SPREAD */
    FP_FLANGER,     /* RATE DEPTH FEEDBACK(-/+) MIX | DELAY DAMP SPREAD */
    FP_CHORUS,      /* RATE DEPTH VOICES(1-3) MIX | DELAY TONE SPREAD */
    FP_COMB,        /* FREQ FEEDBACK(-/+) DAMP MIX | DEPTH RATE SPREAD */
    FP_RING,        /* FREQ DEPTH RATE MIX | SHAPE TONE SPREAD */
    FP_TREMOLO,     /* RATE DEPTH SHAPE MIX | SPREAD (180 degrees = auto-pan) - - */
    FP_FOLD,        /* DRIVE BIAS TONE MIX | SHAPE(tri..sine) LEVEL - */
    FP_FUZZ,        /* DRIVE TONE CHARACTER MIX | LEVEL BIAS GATE */
    FP_BITCRUSH,    /* BITS RATE JITTER MIX | TONE DRIVE - */
    FP_DJ,          /* FILTER(LP..off..HP) RESO DRIVE MIX | SLOPE LFO-RATE LFO-DEPTH */
    FP_LADDER,      /* CUTOFF RESO DRIVE MIX | ENV LFO-RATE WOBBLE */
    FP_WAH,         /* SENS RESO FREQ MIX | SPEED MODE(LP BP HP) DIR(UP DOWN) */
    FP_VOWEL,       /* VOWEL(A E I O U) RESO SHIFT MIX | LFO-RATE DEPTH SPREAD */
    FP_TRANSIENT,   /* ATTACK(-/+) SUSTAIN(-/+) OUTPUT MIX | SPEED CLIP - */
    FP_TEXTURE,     /* KIND LEVEL TONE BREATH | RATE DENSITY DUCK */
    FP_TYPES
};

#define FP_DL_LEN       2048                    /* flanger / chorus / comb line: 46 ms */
#define FP_KINDS        6                       /* texture kinds */

typedef struct fp_state {
    int type, ch;
    uint32_t lfo, rnd, w;
    int32_t apx[8], apy[8], fb, pa;             /* phaser (chorus: apx[0..2] voice delays;
                                                 * ladder: apy[0..3] stages, fb last output) */
    int16_t dl[FP_DL_LEN];                      /* flanger */
    uint32_t dcur;
    int32_t ic[4];                              /* SVF states (2 stages; auto-wah: lo, band) */
    int32_t lp, lp2, hpx, hpy;                  /* tone / DC */
    int32_t envf, envs, gain, gate;             /* followers, gains */
    int32_t tx[8];                              /* texture (bitcrush: tx[0] held, tx[1] clock; ring: tx[0]
                                                 * carrier phase; vowel: tx[0..5] formant states) */
    int32_t level;                              /* output peak, decays (display) */
    int16_t scope[64];                          /* last chunk's output (display) */
} fp_state;

void fp_reset(fp_state *s, int type, int ch);
void fp_process(fp_state *s, int16_t *buf, int n, const int16_t *p);

int fp_type_of(int16_t v);                      /* TYPE value -> effect (0 .. FP_TYPES - 1) */
int16_t fp_type_value(int type);                /* effect -> its TYPE value (slot centre / v0.4 value) */
const char *fp_name(int type);
const char *fp_param_name(int type, int i);     /* i 0..7 (4 = "TYPE") */
const char *fp_choice(int type, int i, int16_t v);  /* text for stepped params, else 0 */
int fp_steps(int type, int i);                  /* number of steps of a stepped param, else 0 */
int fp_step(int type, int i, int16_t v);        /* its step 0 .. fp_steps - 1 */
int16_t fp_step_value(int type, int i, int step);   /* a step's value (its middle; TYPE: slot value) */

#endif
