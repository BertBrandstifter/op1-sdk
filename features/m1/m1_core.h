/* M1 voice: two multisound oscillators (a workstation's "double" mode) into a low-pass filter with
 * its own envelope. Pure C (no TE dependencies): runs on the OP-1 and in the host unit tests
 * (test_m1_core.c). Spec: README.md.
 *
 * Multisounds come from build/m1_data.inc (the sound library, committed with
 * build/NOTICE, not MIT) or, without it and in the simulator benches, from the built-in waves (m1_builtin.inc).
 * Samples are 4-bit block ADPCM or PCM16, played with linear interpolation at TE's voice rate
 * (88.2 kHz, 128 samples per block) from TE's per-sample pitch increments. Output is a raw,
 * full-length note (TE's envelope shapes it afterwards). */
#ifndef M1_CORE_H
#define M1_CORE_H

#include <stdint.h>

enum {
    M1_WAVE_A = 0,  /* multisound of osc A */
    M1_WAVE_B,      /* osc B: off or a multisound */
    M1_MIX,         /* A .. B */
    M1_CUTOFF,      /* 30 Hz .. 16 kHz; top = filter off */
    M1_BPITCH,      /* osc B interval -24 .. +24 semitones */
    M1_DETUNE,      /* A down, B up, 0 .. 25 cents each */
    M1_FENV,        /* filter envelope: cutoff up by up to 6 octaves at note on, decaying */
    M1_FDECAY,      /* 5 ms .. 2 s */
    M1_RES,         /* gentle resonance */
    M1_KEYTRK,      /* cutoff follows the key, 0 .. 100 % around C4 */
    M1_ADECAY,      /* osc A level: 0 = hold, more = faster decay (8 s .. 50 ms) */
    M1_BFADE,       /* osc B level: instant, or fade in 10 ms .. 4 s */
    M1_PARAMS
};

#define M1_SR       88200
#define M1_NOLOOP   0xffffffffu
#define M1_NOMIP    0xffff
#define M1_NAME     12

/* data (gen_m1.py) */
typedef struct m1_sample {
    uint32_t off, len;          /* bytes into the ROM, samples */
    uint32_t loop;              /* loop start (loop end = len), M1_NOLOOP = one-shot */
    uint32_t k;                 /* samples per cycle at the root pitch, 16.16 */
    int16_t h1, h2;             /* ADPCM decoder history at the loop start's block */
    uint8_t fmt;                /* 0 ADPCM (16 samples in 9 bytes), 1 PCM16 */
    uint16_t mip;               /* the same an octave down at half the rate (for high notes), or M1_NOMIP */
} m1_sample;

typedef struct m1_zone {
    uint8_t lo, hi;             /* MIDI keys */
    uint16_t sample;
} m1_zone;

typedef struct m1_multi {
    char name[M1_NAME];
    uint16_t zone;              /* first zone */
    uint8_t nzones;
    uint8_t fixed;              /* 0xff, or the key it always plays at ("NT": no transpose) */
} m1_multi;

typedef struct m1_osc {
    const m1_sample *s;
    uint32_t i;                 /* next sample to decode */
    uint32_t frac;              /* position between x0 and x1, 16 bits */
    int32_t h1, h2;             /* ADPCM history */
    int16_t x0, x1;
    uint8_t hdr, on;
    uint8_t fixed;              /* plays at a fixed key */
} m1_osc;

typedef struct m1_voice {
    m1_osc a, b;
    int key;                    /* MIDI key at note on */
    int32_t lp, bp;             /* filter states */
    int32_t env;                /* filter envelope, Q15 */
    int32_t la, lb;             /* osc levels, Q15 */
    uint32_t fixa, fixb;        /* fixed-key increments */
} m1_voice;

void m1_note_on(m1_voice *v, uint32_t inc, const int16_t *p);
void m1_render(m1_voice *v, int16_t *out, int n, const uint32_t *inc, const int16_t *p);

/* knob 0..32767 -> value */
int m1_wave_of(int16_t knob);           /* 0 .. m1_nwaves() - 1 */
int m1_waveb_of(int16_t knob);          /* -1 = off, else a multisound */
int m1_semis_of(int16_t knob);          /* -24 .. +24 */
int m1_steps_of(int16_t knob);          /* 0 .. 63 (6-bit fields) */
int m1_nwaves(void);
const char *m1_wave_name(int w);
int m1_have_rom(void);                  /* 1: built from a SoundFont, 0: built-in waves */
uint32_t m1_rom_bytes(void);

/* the SoundFont's sample data as a file, loaded at runtime (m1.c): its path ("/yaffs2/..."; 0 when
 * the data is built in), size (whole words) and rotate-xor sum over its little-endian words. Until
 * m1_set_rom hands over the loaded data, m1_rom_ready() is 0 and notes stay silent. */
const char *m1_rom_file(void);
uint32_t m1_rom_file_bytes(void);
uint32_t m1_rom_file_sum(void);
void m1_set_rom(const uint8_t *data);
int m1_rom_ready(void);

/* MIDI key nearest to a per-sample increment at 88.2 kHz, and back */
int m1_key_of_inc(uint32_t inc);
uint32_t m1_inc_of_key(int key);

/* sample of multisound w for key (nearest zone) */
const m1_sample *m1_pick(int w, int key);

/* the sample's octave-down copy that plays inc (x ratio, Q16) at a step <= 1 (PCM16) / 2 (ADPCM), or
 * the last one */
const m1_sample *m1_level(const m1_sample *s, uint32_t inc, uint32_t ratio);

/* low level, for the tests: start an oscillator, decode the next sample */
void m1_osc_start(m1_osc *o, const m1_sample *s);
int16_t m1_osc_next(m1_osc *o, const uint8_t *rom);
const uint8_t *m1_rom_data(void);

#endif
