/* NOTE ROLL punch-in effects (spec: README.md "Punch-in effects"): three banks of 10 on the black
 * keys of TE's synth finger screen (green press picks A / B / C), on NOTE ROLL's playing patterns:
 * A the PO-20 set for the synth, B arcade effects, C time + solos. Pure C, host-tested by
 * test_nr_punch.c. All note effects (NOTE ROLL has no hook into the synth voices' audio). */
#ifndef NR_PUNCH_H
#define NR_PUNCH_H

#include <stdint.h>

enum {
    NR_PX_FILL, NR_PX_RETRIG, NR_PX_RETRIGF, NR_PX_GLITCH, NR_PX_ARP,           /* A */
    NR_PX_OCTUP, NR_PX_OCTDN, NR_PX_STAC, NR_PX_BLINDS, NR_PX_RAND,
    NR_PX_CHIPUP, NR_PX_CHIPDN, NR_PX_POWERUP, NR_PX_POWERDN, NR_PX_COIN,       /* B: arcade */
    NR_PX_TRILL, NR_PX_ECHO, NR_PX_REVERSE, NR_PX_OCTARP, NR_PX_LASER,
    NR_PX_HALF, NR_PX_DOUBLE, NR_PX_TRIPLET, NR_PX_LOOPBEAT, NR_PX_BUILD,       /* C: time + solos */
    NR_PX_SCHORDS, NR_PX_SLEAD, NR_PX_SBASS, NR_PX_SMELODY, NR_PX_FADE,
    NR_PX_KEYS
};
#define NR_PX_BANK      10
#define NR_PX_BANKS     3

extern const char *const nr_px_name[NR_PX_KEYS];
extern const uint8_t nr_px_icon[NR_PX_KEYS][8];     /* 8 x 8 pictograms, bit 7 = left */

/* REPEAT / GLITCH / REVERSE time mappings (as DRUM ROLL's): see dr_punch.h */
typedef struct nr_px_seg { uint32_t src, n, out; } nr_px_seg;
int nr_px_repeat(uint32_t a, uint32_t n, uint32_t t0, uint32_t len, nr_px_seg seg[3]);
int nr_px_glitch(uint32_t a, uint32_t n, uint32_t g, uint32_t span, uint32_t seed, nr_px_seg seg[2]);
int nr_px_reverse(uint32_t a, uint32_t n, uint32_t t0, uint32_t step, uint32_t steps, nr_px_seg seg[3]);

/* one note (voice of voices, stored note key, velocity vel 1..100) of a step with the held effects
 * (bit i = effect i): the notes it plays, at most max: stored note, delay after the step's time,
 * hold, velocity. Returns how many (0: a SOLO or FADE leaves it out).
 * SOLO CHORDS: steps of 2+ notes, LEAD: the highest note, BASS: the lowest, MELODY: steps of one
 * note (several held: any of them). OCT UP / DOWN +-12 (both: none; out of range: unchanged),
 * RANDOM (by r): +12 30 %, -12 20 %, +7 20 %. FADE: velocity x fade / 256.
 * One of (in this order): CHIP ARP UP / DOWN: the step's notes in turn every half span while the
 * note lasts (a single note: with its octave), TRILL: the note and a whole tone up in turn every
 * half span, RISING ARP: the step's notes one after another within span; else the note itself,
 * STACCATO: at most a quarter span, BLINDS: an eighth-span pulse every quarter span while it lasts.
 * Then: COIN an octave-up grace note a quarter span later, OCT ARP the octave half a span later,
 * ECHO the note again 3 and 6 spans later (60 %, 30 %). Holds at least 64 samples. */
typedef struct nr_px_pulse { int key, vel; uint32_t delay, hold; } nr_px_pulse;
int nr_px_notes(uint32_t held, int key, int vel, int voice, int voices, uint32_t span, uint32_t hold,
                uint32_t r, int fade, nr_px_pulse *o, int max);

#endif
