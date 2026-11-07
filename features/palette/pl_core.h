/* PALETTE voice: up to four tones A-D per note, each a wave generator (WG) with pitch envelope, a
 * state-variable filter (TVF: LPF 24 dB = two in a row, BPF, HPF) with its envelope, an amplifier
 * (TVA) with its envelope, and the patch's two LFOs on pitch / cutoff / level: a layered digital
 * synth in the style of the JD-800 / JD-08. Pure C (no TE dependencies): runs on the OP-1 and in
 * the host unit tests (test_pl_core.c). Spec: README.md.
 *
 * Renders TE's voice rate (88.2 kHz, 128 samples per block) from TE's per-sample pitch increments
 * (their block mean). Envelopes, LFOs and filter coefficients run per block, the TVA ramps per
 * sample. The patches (bank) live here too, so the tests can play all of them. */
#ifndef PL_CORE_H
#define PL_CORE_H

#include <stdint.h>

#define PL_SR       88200
#define PL_TONES    4
#define PL_LEVELS   32      /* palette LEVEL 0..31 */
#define PL_PITCHES  32      /* palette PITCH steps (pl_pitch_semis) */
#define PL_PITCH0   15      /* the step of 0 semitones */
#define PL_CUTS     16      /* palette CUTOFF -35 .. +40 in steps of 5 */
#define PL_CUT0     7
#define PL_TIMES    8       /* palette TIME x1/4 .. x4 */
#define PL_TIME0    3

enum {                      /* waves */
    PL_SAW, PL_FATSAW, PL_SQUARE, PL_PULSE25, PL_PULSE12, PL_PWM,
    PL_TRI, PL_SINE, PL_DIGI1, PL_DIGI2, PL_WIRE, PL_VOXA, PL_VOXO, PL_ORGAN, PL_EPIANO, PL_CLAV,
    PL_SYNBASS, PL_GLASS, PL_REED, PL_BRASS, PL_SOFTPAD, PL_SPECTRAL,   /* PL_TRI .. : tables */
    PL_WHITE, PL_PINK,
    PL_WAVES
};
enum { PL_FOFF, PL_LPF, PL_BPF, PL_HPF };                   /* TVF modes */
enum { PL_LTRI, PL_LSAW, PL_LSQR, PL_LSH, PL_LRND };         /* LFO waves */

/* one tone of a patch. Times 0..100 (0 ms .. 20 s), levels as the JD's panel */
typedef struct pl_tone_def {
    uint8_t wave;           /* PL_* */
    uint8_t level;          /* 0..100 (palette LEVEL's start) */
    uint8_t pitch;          /* palette PITCH step (PL_PITCH0 = unison) */
    int8_t fine;            /* cents */
    uint8_t mode;           /* PL_FOFF .. PL_HPF */
    uint8_t cutoff, reso;   /* 0..100 */
    int8_t f_depth;         /* TVF envelope depth -50..50 */
    uint8_t ft[4];          /* TVF envelope T1..T4 */
    int8_t fl[4];           /* TVF envelope L1 L2 SUS L4, -50..50 */
    uint8_t at[4];          /* TVA envelope T1..T4 */
    uint8_t al[3];          /* TVA envelope L1 L2 SUS, 0..100 */
    int16_t f_kf;           /* TVF key follow % around C4 (-100..150) */
    int8_t p_lfo, f_lfo, a_lfo;     /* LFO depths -50..50 on pitch / TVF / TVA */
    uint8_t lfo_sel;        /* bit 0: pitch from LFO 2, bit 1: TVF from LFO 2, bit 2: TVA from LFO 2 */
    int16_t pe_l0, pe_l1;   /* pitch envelope levels, cents (starts at L0, T1 -> L1, T2 -> 0) */
    uint8_t pe_t1, pe_t2;
    int16_t p_kf;           /* pitch key follow - 100 (0 = normal keyboard) */
} pl_tone_def;

typedef struct pl_lfo_def {
    uint8_t rate, delay;    /* 0..100 */
    int8_t fade;            /* -50..50: > 0 fades in, < 0 fades out (over |fade| x 2 as a time) */
    uint8_t wave;           /* PL_L* */
    uint8_t ktrig;          /* 1: the cycle starts at the note */
    int8_t offset;          /* -1, 0, +1 */
} pl_lfo_def;

typedef struct pl_patch {
    const char *name;
    pl_tone_def t[PL_TONES];
    pl_lfo_def lfo[2];
} pl_patch;

extern const pl_patch pl_patches[];
extern const int pl_npatches;
extern const pl_patch *pl_test_patch;   /* host tests only: played in place of the bank's patch */
extern const char *const pl_wave_names[PL_WAVES];      /* up to 8 characters */
extern const char *const pl_wave_short[PL_WAVES];      /* up to 4 characters */

/* the sound: patch + the palette (per tone A-D) */
typedef struct pl_params {
    uint8_t patch;
    uint8_t level[PL_TONES];        /* 0..31 */
    uint8_t wave[PL_TONES];         /* 0..PL_WAVES - 1 */
    uint8_t pitch[PL_TONES];        /* 0..31 */
    uint8_t cut[PL_TONES];          /* 0..15 (PL_CUT0 = the patch's cutoff) */
    uint8_t time[PL_TONES];         /* 0..7 (PL_TIME0 = the patch's times) */
} pl_params;

typedef struct pl_env {
    int32_t e;              /* level now */
    int32_t from, to;
    uint32_t r, rate;       /* ramp 0 .. 1 << 24, per block */
    uint8_t stage;          /* 0 off; TVA / TVF 1..3 segments, 4 sustain, 5 release, 6 end */
} pl_env;

typedef struct pl_tone_st {
    uint32_t ph, ph2;
    int32_t lp, bp, lp2, bp2;       /* filter states */
    int32_t pink;
    int32_t pw;                     /* PWM: the width at the end of the last block */
    int32_t gain;                   /* TVA gain at the end of the last block, Q15 */
    pl_env pe, fe, ae;
} pl_tone_st;

typedef struct pl_voice {
    pl_tone_st t[PL_TONES];
    uint32_t lph[2];                /* LFO phases */
    int32_t lnow[2], lold[2];       /* S&H / random: the value now, the one before */
    uint32_t blocks;                /* since the note */
    uint32_t lfsr;
    int32_t key;                    /* MIDI key of the note (Q8), from its pitch */
    uint8_t on;
} pl_voice;

void pl_voice_init(pl_voice *v);
/* a new note (inc = its pitch): every tone's envelopes start from 0 */
void pl_note(pl_voice *v, uint32_t inc, const pl_params *p);
/* the note's key is up: TVA / TVF envelopes release (T4) */
void pl_release(pl_voice *v, const pl_params *p);
void pl_render(pl_voice *v, int16_t *out, int n, const uint32_t *inc, const pl_params *p);
/* any tone still sounding (TVA not at its end) */
int pl_sounding(const pl_voice *v);

/* a patch's palette as a bank pick sets it */
void pl_patch_params(int patch, pl_params *p);

/* screen / bank helpers */
int pl_pitch_semis(int step);
int pl_level_pct(int level);            /* 0..31 -> 0..100 */
int pl_cut_offset(int cut);             /* -35 .. +40 */
int pl_time_x100(int t);                /* x 100: 25 .. 400 */

int32_t pl_log2(uint32_t x);
uint32_t pl_exp2(int32_t l);

#endif
