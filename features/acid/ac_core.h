/* ACID voice: a TB-303-type bassline voice after Open303 (Robin Schmidt, MIT, see NOTICE), in fixed
 * point, with the TD-3-MO / Devil-Fish-style mods on top. Pure C (no TE dependencies): runs on the
 * OP-1 and in the host unit tests (test_ac_core.c). Spec: README.md.
 *
 * Open303's model: the 303 saw / tanh-shaped square, a 44.5 Hz high-pass, the TB_303 diode-ladder
 * filter (linear, coupled stages, 150 Hz high-pass in the feedback, gain compensation), the main
 * envelope (MEG, exponential decay) smoothed by rc1 (the attack) for the cutoff and by rc2 (the
 * accent sweep circuit) for the accent, the measured cutoff / env-mod mapping, the amp envelope
 * (instant attack, 1230 ms decay to 0, 0.5 / 50 ms release) plus MEG x (0.45 + 4 x accent) while the
 * gate is on, a 200 Hz de-click on the amp, the pitch slew (slide), a 24 Hz high-pass after.
 * Oscillator + filter run at 176.4 kHz (2 x TE's 88.2 kHz, averaged down); controls every 4 samples.
 *
 * Mods: SUB (square an octave down), OVERDRIVE (tanh before the filter), TRACKING, FILTER FM (the
 * output on the cutoff, audio rate), SWEEP OFF / NORM / HIGH (no sweep circuit / Open303 / more
 * accent and resonance), SPEED (rc2 30 / 15 / 5 ms), MUFFLER (soft clip after the VCA), LEVEL. */
#ifndef AC_CORE_H
#define AC_CORE_H

#include <stdint.h>

#define AC_SR       88200

enum { AC_SAW, AC_SQUARE };
enum { AC_SWEEP_OFF, AC_SWEEP_NORM, AC_SWEEP_HIGH };
enum { AC_MUFF_OFF, AC_MUFF_SOFT, AC_MUFF_HARD };

typedef struct ac_params {
    int32_t cutoff;             /* 0..255: 314 .. 2394 Hz (the 303's range, exp) */
    int32_t res;                /* 0..255: resonance, skewed as Open303 */
    int32_t envmod;             /* 0..255: 0 .. 100 % (measured mapping) */
    int32_t decay;              /* 0..255: MEG time constant of normal notes, 30 .. 3000 ms */
    uint8_t accent;             /* 0..63: accent 0 .. 100 % */
    uint8_t accdec;             /* 0..31: MEG time constant of accented notes, 30 .. 3000 ms */
    uint8_t sweep;              /* AC_SWEEP_* */
    uint8_t speed;              /* 0..2: SLOW NORM FAST */
    uint8_t wave;               /* AC_SAW / AC_SQUARE */
    uint8_t sub;                /* 0..3: off, lo, mid, hi */
    int8_t tune;                /* -12..12 semitones */
    uint8_t slide;              /* 0..31: 10 .. 360 ms */
    uint8_t soft;               /* 0..31: rc1 (filter envelope attack) 0.3 .. 30 ms */
    uint8_t track;              /* 0..31: filter tracking 0 .. 1 oct / oct */
    uint8_t fm;                 /* 0..31: filter FM from the output */
    uint8_t drive;              /* 0..31: overdrive, input gain 1 .. 8 into a tanh (0 = linear) */
    uint8_t muff;               /* AC_MUFF_* */
    uint8_t level;              /* 0..15: output, 10 = unity */
} ac_params;

typedef struct ac_voice {
    uint32_t ph, sph;           /* oscillator, sub phases (176.4 kHz) */
    uint32_t cur;               /* pitch now: TE increment (88.2 kHz) after the slew, tune included */
    int32_t y[4];               /* filter stages, Q20 */
    int32_t hx, hy;             /* feedback high-pass state */
    int32_t px, py;             /* pre-filter high-pass state */
    int32_t qx, qy;             /* post-filter high-pass state */
    int32_t fy;                 /* output before the muffler, Q14 (filter FM) */
    int32_t meg;                /* main envelope, Q30 */
    int32_t rc1, rc2;           /* the MEG through rc1 (attack) and rc2 (accent), Q30 */
    int32_t amp;                /* amp envelope, Q30 */
    int32_t dc1, dc2;           /* de-click low-passes on the amp, Q26 */
    int32_t gain;               /* amp at the last control update, Q26 */
    int32_t key;                /* MIDI key now (screen / tracking), Q16 */
    uint8_t on;                 /* a note has been played */
    uint8_t gate;               /* the note's key is down (or no key-up has come) */
    uint8_t accent;             /* the note is accented */
    uint8_t snap;               /* the next block starts at the note's pitch */
} ac_voice;

void ac_init(ac_voice *v);
/* a new note: slide = the pitch slews there and the envelopes run on (Open303's slideToNote); else
 * the envelopes restart (triggerNote) */
void ac_note(ac_voice *v, int slide, int accent);
/* the gate is off (the last key up): the amp releases (0.5 ms, accented notes 50 ms) */
void ac_release(ac_voice *v);
void ac_render(ac_voice *v, int16_t *out, int n, const uint32_t *inc, const ac_params *p);

/* screen helpers */
int ac_decay_ms(int c);                         /* DECAY 0..255 */
int ac_accdec_ms(int s);                        /* ACC DECAY 0..31 */
int ac_slide_ms(int s);
int ac_soft_10us(int s);                        /* 0.1 ms */

int32_t ac_log2(uint32_t x);
uint32_t ac_exp2(int32_t l);

#endif
