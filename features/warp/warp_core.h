/* WARP voice: a single-cycle wave read through one of 16 phase / output warps. Pure C (no TE
 * dependencies): runs on the OP-1 and in the host unit tests (test_warp_core.c). Spec:
 * README.md.
 *
 * Renders TE's voice rate (88.2 kHz, 128 samples per block). Pitch comes in as TE's per-sample
 * phase increments (2^32 = one cycle). Parameters are 8 values 0..32767. Output is a raw,
 * full-length note (TE's envelope, velocity and volume shape it afterwards). Per-sample work is
 * 16/32-bit integer maths; warp constants are set every 16 samples. */
#ifndef WARP_CORE_H
#define WARP_CORE_H

#include <stdint.h>

enum {
    WP_WAVE = 0,    /* SINE TRI SAW SQUARE PULSE HALF ORGAN VOX */
    WP_MODE,        /* 16 warps (below) */
    WP_AMOUNT,      /* warp amount (0 = the plain wave) */
    WP_COLOR,       /* lowpass (0 = open) */
    WP_SWEEP,       /* amount added at note on, decaying (0 = none) */
    WP_DECAY,       /* sweep decay 10 ms .. 4 s */
    WP_MOTION,      /* LFO on the amount: faster and deeper (0 = off) */
    WP_UNISON,      /* second oscillator, detuned up to ~20 cents (0 = off) */
    WP_PARAMS
};

enum {
    WP_BEND = 0, WP_SYNC, WP_MIRROR, WP_FOLD, WP_QUANT, WP_SQUEEZE, WP_PINCH, WP_WRAP,
    WP_FM, WP_GAP, WP_BITREV, WP_CHAOS, WP_STAIR, WP_HARM, WP_RING, WP_XOR, WP_MODES
};

#define WP_SR       88200

typedef struct wp_voice {
    uint32_t phase, phase2;     /* oscillator, unison oscillator */
    uint32_t lfo;               /* motion LFO phase */
    int32_t env;                /* sweep envelope, Q15 */
    int32_t lp, lp2;            /* colour filter states */
    int32_t a_prev;             /* last sub-block's effective amount (for the scope) */
} wp_voice;

/* start a note: phases restart, sweep envelope at full */
void wp_note_on(wp_voice *v);

/* render n samples (n <= 128); inc[i] = phase increment of sample i */
void wp_render(wp_voice *v, int16_t *out, int n, const uint32_t *inc, const int16_t *p);

/* one cycle of the current wave + warp (n points, amount a 0..32767) for the screen */
void wp_cycle(int wave, int mode, int a, int16_t *out, int n);

int wp_wave_of(int16_t knob);
int wp_mode_of(int16_t knob);
extern const char *const wp_wave_names[];
extern const char *const wp_mode_names[WP_MODES];

#endif
