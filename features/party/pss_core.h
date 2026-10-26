/* PSS voice: two multisound oscillators (a workstation's "double" mode) into a low-pass filter with
 * its own envelope. Pure C (no TE dependencies): runs on the OP-1 and in the host unit tests
 * (test_pss_core.c). Spec: README.md.
 *
 * Multisounds come from build/pss_data.inc (the sound library, committed with
 * build/NOTICE, not MIT) or, without it and in the simulator benches, from the built-in waves (pss_builtin.inc).
 * Samples are 4-bit block ADPCM or PCM16 (sample sets, built-in waves) or the PSS-11's own AWM formats
 * (8-bit, 12-bit: the GEW7 ROM's samples as they are), played with linear interpolation at TE's
 * voice rate (88.2 kHz, 128 samples per block) from TE's per-sample pitch increments. Every sample can
 * carry the GEW7's envelope (attack, decay 1, decay level, decay 2, key scaling) and LFO (vibrato,
 * tremolo); the ROM's samples always do. TE's envelope shapes the note afterwards (its release ends
 * it: the engine host passes no key-up). */
#ifndef PSS_CORE_H
#define PSS_CORE_H

#include <stdint.h>

enum {
    PSS_WAVE_A = 0,  /* multisound of osc A */
    PSS_WAVE_B,      /* osc B: off or a multisound */
    PSS_MIX,         /* A .. B */
    PSS_CUTOFF,      /* 30 Hz .. 16 kHz; top = filter off */
    PSS_BPITCH,      /* osc B interval -24 .. +24 semitones */
    PSS_DETUNE,      /* A down, B up, 0 .. 25 cents each */
    PSS_FENV,        /* filter envelope: cutoff up by up to 6 octaves at note on, decaying */
    PSS_FDECAY,      /* 5 ms .. 2 s */
    PSS_RES,         /* gentle resonance */
    PSS_KEYTRK,      /* cutoff follows the key, 0 .. 100 % around C4 */
    PSS_ADECAY,      /* osc A level: 0 = hold, more = faster decay (8 s .. 50 ms) */
    PSS_BFADE,       /* osc B level: instant, or fade in 10 ms .. 4 s */
    PSS_PARAMS
};

#define PSS_SR       88200
#define PSS_NOLOOP   0xffffffffu
#define PSS_NOMIP    0xffff
#define PSS_NAME     12

/* data (gen_pss.py) */
typedef struct pss_sample {
    uint32_t off, len;          /* bytes into the ROM, samples */
    uint32_t loop;              /* loop start (loop end = len), PSS_NOLOOP = one-shot */
    uint32_t k;                 /* samples per cycle at the root pitch, 16.16 */
    int16_t h1, h2;             /* ADPCM decoder history at the loop start's block */
    uint8_t fmt;                /* 0 ADPCM (16 samples in 9 bytes), 1 PCM16, 2 GEW 8-bit, 3 GEW 12-bit */
    uint16_t mip;               /* the same an octave down at half the rate (for high notes), or PSS_NOMIP */
    uint8_t eg[3];              /* GEW7 table bytes 9..11: AR | D1R, D2R | RR, KRS | DL; ADPCM / PCM16:
                                   all 0 = no envelope */
    uint8_t lfo[2];             /* bytes 7, 8: vibrato | tremolo << 3 | reverse << 7; LFO speed (& 7) */
} pss_sample;

typedef struct pss_zone {
    uint8_t lo, hi;             /* MIDI keys */
    uint16_t sample;
} pss_zone;

typedef struct pss_multi {
    char name[PSS_NAME];
    uint16_t zone;              /* first zone */
    uint8_t nzones;
    uint8_t fixed;              /* 0xff, or the key it always plays at ("NT": no transpose) */
} pss_multi;

/* GEW7 envelope + LFO of one oscillator (pss_core.c) */
typedef struct pss_eg {
    int32_t v;                  /* level << 16 (0 .. 0x3ff << 16) */
    uint8_t st;                 /* 0 attack, 1 decay 1, 2 decay 2, 3 none (full level) */
    uint8_t ar, d1, d2;         /* rate indices 0..63 */
    int32_t th;                 /* decay 1 ends below this level */
    int32_t g;                  /* gain at the end of the last 32 samples, Q16 */
    uint16_t ph;                /* LFO phase (256 steps << 8), free running as the chip's */
    uint8_t lf, vib, trem;
} pss_eg;

typedef struct pss_osc {
    const pss_sample *s;
    uint32_t i;                 /* next sample to decode */
    uint32_t frac;              /* position between x0 and x1, 16 bits */
    int32_t h1, h2;             /* ADPCM history */
    int16_t x0, x1;
    uint8_t hdr, on;
    uint8_t fixed;              /* plays at a fixed key */
    uint8_t rev;                /* GEW: plays backwards until the first loop */
    uint8_t direct;             /* GEW at a high step: i = the sample at the position (osc_gew) */
    pss_eg eg;
} pss_osc;

typedef struct pss_voice {
    pss_osc a, b;
    int key;                    /* MIDI key at note on */
    int32_t lp, bp;             /* filter states */
    int32_t env;                /* filter envelope, Q15 */
    int32_t la, lb;             /* osc levels, Q15 */
    uint32_t fixa, fixb;        /* fixed-key increments */
} pss_voice;

void pss_note_on(pss_voice *v, uint32_t inc, const int16_t *p);
void pss_render(pss_voice *v, int16_t *out, int n, const uint32_t *inc, const int16_t *p);

/* knob 0..32767 -> value */
int pss_wave_of(int16_t knob);           /* 0 .. pss_nwaves() - 1 */
int pss_waveb_of(int16_t knob);          /* -1 = off, else a multisound */
int pss_semis_of(int16_t knob);          /* -24 .. +24 */
int pss_steps_of(int16_t knob);          /* 0 .. 63 (6-bit fields) */
int pss_nwaves(void);
const char *pss_wave_name(int w);
int pss_have_rom(void);                  /* 1: built from a sample set, 0: built-in waves */
uint32_t pss_rom_bytes(void);

/* the sample data as a file, loaded at runtime (pss.c): its path ("/yaffs2/..."; 0 when
 * the data is built in), size (whole words) and rotate-xor sum over its little-endian words. Until
 * pss_set_rom hands over the loaded data, pss_rom_ready() is 0 and notes stay silent. */
const char *pss_rom_file(void);
uint32_t pss_rom_file_bytes(void);
uint32_t pss_rom_file_sum(void);
void pss_set_rom(const uint8_t *data);
int pss_rom_ready(void);

/* MIDI key nearest to a per-sample increment at 88.2 kHz, and back */
int pss_key_of_inc(uint32_t inc);
uint32_t pss_inc_of_key(int key);

/* sample of multisound w for key (nearest zone) */
const pss_sample *pss_pick(int w, int key);

/* the sample's octave-down copy that plays inc (x ratio, Q16) at a step <= 1 (PCPSS6) / 2 (ADPCM), or
 * the last one */
const pss_sample *pss_level(const pss_sample *s, uint32_t inc, uint32_t ratio);

/* GEW7 envelope: start (eg = table bytes 9..11, st = 16.16 source samples per output sample at
 * 88.2 kHz, for the key scaling), then once per 32 output samples: the level index 0..1023 */
void pss_eg_start(pss_eg *e, const uint8_t *eg, const uint8_t *lfo, int on, uint32_t st);
int pss_eg_next(pss_eg *e);

/* low level, for the tests: start an oscillator, decode the next sample */
void pss_osc_start(pss_osc *o, const pss_sample *s);
int16_t pss_osc_next(pss_osc *o, const uint8_t *rom);
const uint8_t *pss_rom_data(void);

#endif
