/* CHORD+ core: chord-keyboard theory. Pure C (no TE dependencies): runs on the OP-1 and in the
 * host unit tests (test_cp_core.c). Spec: README.md.
 *
 * White keys play the seven chords of the chosen key and scale (C = I, D = ii, ... B = vii);
 * held black keys modify the next chord. Lower octave: C# flips major/minor, D# adds the 7th, F#
 * turns the third into a fourth (sus4), G# adds the 9th, A# inverts once. Upper octave: C# power
 * chord, D# sus2, F# OPEN voicing, G# WIDE voicing, A# inverts twice (both A#: three times). */
#ifndef CP_CORE_H
#define CP_CORE_H

#include <stdint.h>

enum { CP_MAJOR = 0, CP_MINOR, CP_DORIAN, CP_MIXO, CP_HARMONIC, CP_PHRYGIAN, CP_LYDIAN, CP_SCALES };
enum { CP_CLOSE = 0, CP_LEAD, CP_OPEN, CP_BASS, CP_VOICINGS };

/* modifier bits (held black keys) */
#define CP_FLIP     0x01        /* C#: major <-> minor third */
#define CP_SEVEN    0x02        /* D#: + diatonic 7th */
#define CP_SUS4     0x04        /* F#: third -> perfect fourth */
#define CP_NINE     0x08        /* G#: + diatonic 9th */
#define CP_INV1     0x10        /* A# (lower): one inversion */
#define CP_INV2     0x20        /* A# (upper): two inversions (with CP_INV1: three) */
#define CP_POWER    0x40        /* C# (upper): root, fifth, octave (third / 7th / 9th left out) */
#define CP_SUS2     0x80        /* D# (upper): third -> second (CP_SUS4 wins) */
#define CP_VOPEN    0x100       /* F# (upper): OPEN voicing (drop 2) while held */
#define CP_VWIDE    0x200       /* G# (upper): WIDE voicing (lowest note down, next note up an
                                 * octave) while held */

#define CP_MAX_NOTES 6

typedef struct cp_settings {
    uint8_t key;                /* tonic 0..11 (C..B) */
    uint8_t scale;              /* CP_MAJOR .. */
    uint8_t voicing;            /* CP_CLOSE .. */
} cp_settings;

/* 1 if MIDI note n is a black key (C# D# F# G# A#) */
int cp_is_black(int n);

/* modifier bit a black MIDI note stands for (C# -> CP_FLIP, ..., A# -> CP_INV1); 0 for white */
int cp_black_mod(int n);

/* modifier bit of a black key by its place on the keyboard: 0..4 the lower octave's F# G# A# C#
 * D#, 5..9 the upper octave's (-1: none) */
int cp_black_mod_at(int place);

/* the chord's root pitch class (0..11) for white MIDI note n; -1 for a black note */
int cp_root_pc(const cp_settings *s, int n);

/* build the chord for white MIDI note n (its degree and octave) with the held modifiers; prev =
 * the previous chord's average pitch x 16 for CP_LEAD (0 = none; CP_VOPEN / CP_VWIDE held: no
 * voice leading). Writes up to CP_MAX_NOTES MIDI notes (0..127, ascending, no duplicates) and
 * returns the count (0 for a black note). */
int cp_chord(const cp_settings *s, int n, int mods, int prev, uint8_t *out);

/* average pitch x 16 of a chord (for CP_LEAD) */
int cp_center(const uint8_t *notes, int count);

/* chord name, e.g. "DM7" "F#SUS4" "BDIM" "CMAJ7" "AM9" (root = lowest chord tone before inversion
 * = notes as built; buf >= 12) */
void cp_name(const cp_settings *s, int n, int mods, char *buf);

extern const char *const cp_key_names[12];
extern const char *const cp_scale_names[CP_SCALES];
extern const char *const cp_voicing_names[CP_VOICINGS];

#endif
