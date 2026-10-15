/* LOFI synth voice: chiptune waves and pitch-tracking bytebeat. Pure C (no TE dependencies): runs
 * on the OP-1 and in the host unit tests (test_lofi_core.c). Spec: README.md.
 *
 * Runs at TE's voice rate, LF_SR = 88.2 kHz (voices are 2x oversampled, 128 samples per 64-frame
 * audio block). Pitch comes in as TE's per-sample phase increments (2^32 = one cycle; glide, bend,
 * LFO and tune already applied). Parameters are 8 int16 knob values 0..32767 (preset knob slots
 * 1-8). Output is a raw, full-length note (TE's envelope, velocity and volume shape it afterwards). Per-sample work uses 32-bit integer maths only (no divides, no
 * 64-bit), per-block set-up may use 64-bit. */
#ifndef LOFI_CORE_H
#define LOFI_CORE_H

#include <stdint.h>

enum {
    LF_WAVE = 0,    /* PULSE TRI SAW NOISE METAL WAVE A-D BYTE 1-16 */
    LF_SHAPE,       /* pulse duty / tri+saw steps / noise clock / wave morph / byte VAR */
    LF_CRUSH,       /* sample-and-hold 1..128 at 88.2 kHz (0 = off) */
    LF_BITS,        /* bit depth 16..1 (0 = off) */
    LF_BEND,        /* phase distortion; byte: t ^ t >> s (0 = off) */
    LF_LOOP,        /* chip: arpeggio pattern; byte: loop window 2^17..2^5 (0 = off) */
    LF_SWEEP,       /* pitch sweep down from note start (0 = off) */
    LF_TONE,        /* lowpass (0 = open) */
    LF_PARAMS
};

enum { LF_W_PULSE = 0, LF_W_TRI, LF_W_SAW, LF_W_NOISE, LF_W_METAL, LF_W_TABLE, LF_W_BYTE = 9 };
#define LF_TABLES   4
#define LF_BYTES    16
#define LF_WAVES    (LF_W_BYTE + LF_BYTES)      /* 25 positions on the WAVE knob */

#define LF_SR       88200                       /* TE's voice render rate */
#define LF_LEVEL    24576                       /* peak output (TE's voltage peaks ~28.7k) */
#define LF_FRAME    128                         /* samples per arpeggio/sweep frame (1.45 ms) */

typedef struct lf_voice {
    uint32_t phase;             /* oscillator phase, 2^32 = one cycle */
    uint32_t t, tfrac;          /* bytebeat time (t steps) and its 16-bit fraction */
    uint32_t nacc;              /* noise clock accumulator, 16.16 clocks */
    uint16_t lfsr;              /* noise shift register (15 bit, never 0) */
    int16_t hold;               /* crush: held sample */
    uint8_t count;              /* crush: samples left until the next one is taken */
    uint8_t arp;                /* arpeggio step */
    uint16_t frame;             /* frames since note on (sweep, arpeggio) */
    uint16_t fpos;              /* samples into the current frame */
    uint32_t noise_inc;         /* noise clocks per sample, 16.16 (set per frame) */
    int32_t smooth, tone;       /* bytebeat smoothing / TONE lowpass state */
    uint32_t sweep_q16;         /* pitch factor of the sweep, 16.16 (1.0 at note on) */
} lf_voice;

/* start a note: resets phase, bytebeat time, sweep and arpeggio (every note sounds the same) */
void lf_note_on(lf_voice *v);

/* render n samples; inc[i] = phase increment of sample i (any n; arpeggio/sweep advance every
 * LF_FRAME samples) */
void lf_render(lf_voice *v, int16_t *out, int n, const uint32_t *inc, const int16_t *p);

/* which wave a WAVE knob value selects (0 .. LF_WAVES - 1) */
int lf_wave(int16_t knob);

#endif
