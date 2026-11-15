/* py_drum: PLAY's drum voices for the drum pattern track P4 (new_pattern_tape.md 2.3), host-tested,
 * no TE calls.
 *
 * Plays a key of a drum kit the way TE's drum sampler does (analysis/notes/re_drum.md), at
 * 44.1 kHz from the kit TE keeps in sample memory (read in place):
 *   key block (8 int16, as TE keeps it per slot and key): W0 pitch (semitones x 512), start =
 *   (W1 << 16) + 2 x W5, end = (W2 << 16) + 2 x W6 (both: frames x 2^31 / 529200), W3 >> 13
 *   playmode (1 one-shot, 2 loop while held, 0 / 3 gate), W4 >> 14 reverse, W7 volume (8192 =
 *   unity, squared law).
 *   rate = 2^((W0 / 512 + semitones) / 12) frames per output sample, linear interpolation.
 *   gate: a hit is held `hold` samples; then a non-FULL hit fades out over 128 samples (DRUM ROLL's
 *   gate); a FULL hit's release lets one-shot keys play to the end and fades the others.
 * Mono output, added into an int32 buffer at int16 scale. */
#ifndef PY_DRUM_H
#define PY_DRUM_H

#include <stdint.h>

#define PY_DVOICES      8
#define PY_DRUM_FRAMES  529200                  /* TE's kit buffer: 12 s */
#define PY_DRUM_FADE    128                     /* DRUM ROLL's gate fade (2.9 ms) */
#define PY_DRUM_CAL     20545                   /* Q15: TE's level at volume 8192, velocity 127
                                                 * (0.627, simulator); calibrate on the device */

typedef struct py_dv {
    uint8_t on, loop, rev, oneshot, released;
    uint16_t fade;                              /* samples left of the fade-out, 0 = none */
    uint32_t delay;                             /* samples until the hit starts (in this block) */
    uint32_t pos, frac;                         /* frames played (from start / end), Q16 fraction */
    uint32_t start, len;                        /* frames */
    uint32_t inc;                               /* Q16 frames per sample */
    int32_t gain;                               /* Q15, at most 65535 */
    uint32_t gate;                              /* samples until the note-off, 0xffffffff = none */
    uint32_t age;
    const int16_t *pcm;
} py_dv;

typedef struct py_drums {
    py_dv v[PY_DVOICES];
    uint32_t ages;
} py_drums;

/* 2^(x / 6144) in Q16 (x in 1/512 semitones), x clamped to +-96 semitones */
uint32_t py_pow2_q16(int32_t x512);

/* frames of a key block position pair (hi = W1 / W2, lo = W5 / W6) */
uint32_t py_drum_frames(uint16_t hi, uint16_t lo);

/* a hit of the key block `key` on pcm (frame 0 of the kit), `semis` semitones on top of the key's
 * pitch, gain Q15 (velocity x lane level, before the kit's volume), held `hold` samples, FULL or
 * not, starting `delay` samples into the next rendered block. Steals the oldest voice. */
void py_drum_hit(py_drums *d, const int16_t *pcm, const int16_t key[8], int semis, int32_t gain,
                 uint32_t hold, int full, uint32_t delay);

/* n samples of every voice added into out[0..n-1] */
void py_drum_render(py_drums *d, int32_t *out, int n);

int py_drum_active(const py_drums *d);          /* voices playing */

#endif
