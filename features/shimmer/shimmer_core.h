/* SHIMMER effect core: a reverb whose tail is pitch-shifted (an octave up by default) and fed back
 * into itself, so every pass through the tank rises another interval. Pure C (no TE dependencies):
 * runs on the OP-1 and in the host unit tests (test_shimmer_core.c). Spec: README.md.
 *
 * FX rate 44.1 kHz. Tank: 2 input allpass diffusers into a 4-line feedback delay network
 * (Householder mix, one-pole damping per line, RT60-matched line gains). Shifter: two crossfading
 * taps on a delay line (triangle windows) inside the tank's loop: the mean that Householder feeds
 * back to every line is partly replaced by its shifted copy, so each pass rises an interval.
 * Parameters are 8 int16 values 0..32767. Per-sample work is 32-bit integer maths (no divides, no
 * 64-bit); per-block set-up may use both. */
#ifndef SHIMMER_CORE_H
#define SHIMMER_CORE_H

#include <stdint.h>

enum {
    SH_DECAY = 0,   /* reverb time: ~0.3 s .. ~20 s */
    SH_TONE,        /* damping in the tank: dark .. bright */
    SH_SHIMMER,     /* pitch-shifted feedback: none .. a lot */
    SH_MIX,         /* dry .. wet (0 = dry, bit-exact) */
    SH_INTERVAL,    /* shift per pass: +12 +7 +19 +24 -12 semitones */
    SH_SIZE,        /* tank size: small room .. hall */
    SH_MOD,         /* slow modulation of the line lengths (less metallic, chorused tail) */
    SH_LOWCUT,      /* high-pass on the shimmer feedback: ~40 Hz .. ~1 kHz */
    SH_PARAMS
};

#define SH_LINES        4
#define SH_LINE_LEN     4096                    /* per tank line: longest ~79 ms */
#define SH_PS_LEN       4096                    /* shifter delay line */
#define SH_PS_WIN       2048                    /* shifter window: ~46 ms */
#define SH_AP1_LEN      256
#define SH_AP2_LEN      1024
#define SH_INTERVALS    5

typedef struct sh_state {
    int16_t line[SH_LINES][SH_LINE_LEN];
    int16_t ps[SH_PS_LEN];
    int16_t ap1[SH_AP1_LEN], ap2[SH_AP2_LEN];
    uint32_t wpos;                              /* write position (all buffers) */
    uint32_t delay_q16[SH_LINES];               /* line lengths slewed toward SIZE */
    uint32_t dcur[SH_LINES];                    /* length incl. MOD where the last block ended */
    int32_t lp[SH_LINES];                       /* damping lowpass per line */
    uint32_t ps_ph;                             /* shifter tap phase, 16.16 samples in 0..WIN */
    uint32_t lfo;                               /* MOD phase */
    int32_t pl1, pl2, phx, phy;                 /* shifter lowpass x2, low cut */
    int32_t hp_x, hp_y;                         /* input DC blocker */
    int32_t level;                              /* wet peak, decays (for the display) */
    int sized;                                  /* line lengths set at least once */
} sh_state;

void sh_reset(sh_state *s);

/* Mono block in place (synth / drum FX). */
void sh_process(sh_state *s, int16_t *buf, int n, const int16_t *p);

/* Stereo block in place (master FX): the tank runs on the mid signal, its line taps give a wide
 * stereo wet. */
void sh_process_stereo(sh_state *s, int16_t *l, int16_t *r, int n, const int16_t *p);

/* which interval an INTERVAL value selects (0 .. SH_INTERVALS - 1), and its semitones */
int sh_interval(int16_t knob);
int sh_interval_semis(int i);

#endif
