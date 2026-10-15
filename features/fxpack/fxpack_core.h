/* FXPACK effect core: seven small effects behind one host FX (spec: README.md). Pure C (no
 * TE dependencies): runs on the OP-1 and in the host unit tests (test_fxpack_core.c).
 *
 * FX rate 44.1 kHz, mono blocks in place; a stereo host runs one state per channel (ch 1 = right:
 * LFOs offset by SPREAD, own noise). Parameters: 8 int16 values 0..32767; p[0..3] page 1, p[4] the
 * effect type (not read here: fp_reset picks the effect), p[5..7] page 2. Per-sample work is 32-bit
 * integer maths; coefficients are set once per <= 64-sample chunk (one divide at most). */
#ifndef FXPACK_CORE_H
#define FXPACK_CORE_H

#include <stdint.h>

enum {
    FP_PHASER = 0,  /* RATE DEPTH FEEDBACK MIX | CENTER STAGES SPREAD */
    FP_FLANGER,     /* RATE DEPTH FEEDBACK(-/+) MIX | DELAY DAMP SPREAD */
    FP_FOLD,        /* DRIVE BIAS TONE MIX | SHAPE(tri..sine) LEVEL - */
    FP_FUZZ,        /* DRIVE TONE CHARACTER MIX | LEVEL BIAS GATE */
    FP_DJ,          /* FILTER(LP..off..HP) RESO DRIVE MIX | SLOPE LFO-RATE LFO-DEPTH */
    FP_TRANSIENT,   /* ATTACK(-/+) SUSTAIN(-/+) OUTPUT MIX | SPEED CLIP - */
    FP_TEXTURE,     /* KIND LEVEL TONE BREATH | RATE DENSITY DUCK */
    FP_TYPES
};

#define FP_DL_LEN       2048                    /* flanger line: 46 ms */
#define FP_KINDS        6                       /* texture kinds */

typedef struct fp_state {
    int type, ch;
    uint32_t lfo, rnd, w;
    int32_t apx[8], apy[8], fb, pa;             /* phaser */
    int16_t dl[FP_DL_LEN];                      /* flanger */
    uint32_t dcur;
    int32_t ic[4];                              /* SVF states (2 stages) */
    int32_t lp, lp2, hpx, hpy;                  /* tone / DC */
    int32_t envf, envs, gain, gate;             /* followers, gains */
    int32_t tx[8];                              /* texture */
    int32_t level;                              /* output peak, decays (display) */
    int16_t scope[64];                          /* last chunk's output (display) */
} fp_state;

void fp_reset(fp_state *s, int type, int ch);
void fp_process(fp_state *s, int16_t *buf, int n, const int16_t *p);

int fp_type_of(int16_t v);                      /* TYPE knob -> effect (0 .. FP_TYPES - 1) */
const char *fp_name(int type);
const char *fp_param_name(int type, int i);     /* i 0..7 (4 = "TYPE") */
const char *fp_choice(int type, int i, int16_t v);  /* text for stepped params, else 0 */

#endif
