/* py_slot: PLAY's song slots (README "Songs"): a song with its patterns in one file, pure C,
 * host-tested (test_py_slot.c), no TE calls.
 *
 * A slot file holds the whole song (tracks, song grid, chains: py_song_save's PSN3 blob) and the 14
 * NOTE ROLL and 14 DRUM ROLL patterns as PLAY plays them, plus both editors' swing and the synth
 * sounds the song plays (engine type, octave, knobs, envelope of each sound slot a SYNTH track or
 * one of its chain rows names). Patterns equal to the empty pattern are not stored (a mask says
 * which are). Loading gives back the song and the 28 patterns (the ones not stored become empty
 * again) and the sounds the caller can take back (io->sok), but for those the caller keeps:
 * py_slot_diff says which patterns and sounds differ from now, the header which patterns the song
 * plays. The file is the sharing format too (disk mode's play_s/ folder, README "Songs").
 *
 *   0   "PSL2" ("PSL1": v0.10 / v0.11, a 32-byte header, no sounds)
 *   4   u16 bytes of a NOTE ROLL pattern, u16 bytes of a DRUM ROLL pattern (other sizes: refused)
 *   8   u32 file bytes (with the CRC)
 *   12  u16 NOTE ROLL mask, u16 DRUM ROLL mask (bit p: pattern p stored)
 *   16  types[8] (PS_NONE / PS_AUDIO / PS_SYNTH / PS_DRUM per track), rows used, chains used,
 *       NOTE ROLL swing, DRUM ROLL swing,
 *   28  u16 NOTE ROLL patterns the song plays | 0x8000, u16 DRUM ROLL ones | 0x8000 (0: a file
 *       of v0.10.0, read as "the stored ones")
 *   32  u8 sounds stored (bit s: synth sound slot s), 7 bytes 0
 *   40  the song (PS_FILE_BYTES)
 *       the stored NOTE ROLL patterns in order, then the stored DRUM ROLL patterns, then the
 *       stored sounds in slot order (PSL_SND bytes each, opaque here: the caller's sget / sput)
 *   end u32 CRC-32 of everything before it
 * Numbers little-endian. Save and load stream through callbacks with one pattern of scratch, so a
 * slot never needs the whole file in RAM. */
#ifndef PY_SLOT_H
#define PY_SLOT_H

#include <stdint.h>
#include "py_song.h"

#define PSL_SLOTS   16
#define PSL_PATS    14                          /* patterns of each kind */
#define PSL_HDR     40                          /* (PSL1: PSL_HDR1) */
#define PSL_HDR1    32
#define PSL_SOUNDS  8                           /* synth sound slots */
#define PSL_SND     36                          /* bytes of a stored sound */
#define PSL_NR      0                           /* pattern kinds */
#define PSL_DR      1

enum { PSL_OK = 0, PSL_NOFILE = -1, PSL_BAD = -2, PSL_OTHER = -3, PSL_IO = -4, PSL_BUSY = -5,
       PSL_WAIT = -6 /* (PLAY: the editors have not read their patterns yet) */ };

typedef struct psl_info {
    uint8_t type[PS_TRACKS];                    /* per track, in list order */
    uint8_t rows, chains;                       /* song rows up to the last used one, chains holding rows */
    uint8_t swing[2];
    uint16_t mask[2];                           /* stored patterns per kind */
    uint16_t used[2];                           /* patterns the song plays (its SYNTH / DRUM chains) */
    uint8_t smask;                              /* sounds stored (summary: the sounds the song plays) */
    uint8_t sbad;                               /* py_slot_diff: stored sounds io->sok refuses now */
    uint8_t hdr;                                /* header bytes (PSL_HDR, PSL1: PSL_HDR1) */
    uint32_t bytes;
} psl_info;

typedef struct psl_io {
    void *ctx;
    uint32_t size[2];                           /* bytes of one pattern of each kind */
    const void *empty[2];                       /* the empty pattern of each kind */
    void *pat;                                  /* scratch: one pattern of the larger kind (4-byte aligned) */
    /* save: pattern p of a kind as it plays now, into out; 0 = ok */
    int (*get)(void *ctx, int kind, int p, void *out);
    /* load: pattern p of a kind becomes `in` */
    void (*put)(void *ctx, int kind, int p, const void *in);
    int (*write)(void *ctx, const void *buf, uint32_t n);          /* 0 = ok (appends) */
    int (*read)(void *ctx, uint32_t off, void *buf, uint32_t n);   /* bytes read */
    int cur_sound;                              /* save: TE's current synth sound (a track on CUR plays it) */
    /* save / diff: synth sound slot s as it is now, PSL_SND bytes into rec; 0 = ok */
    int (*sget)(void *ctx, int s, uint8_t *rec);
    /* load: sound slot s becomes rec (only where sok said yes) */
    void (*sput)(void *ctx, int s, const uint8_t *rec);
    /* rec can become sound slot s now (its engine is there, the slot can take it): 1 */
    int (*sok)(void *ctx, int s, const uint8_t *rec);
} psl_io;

/* the header's summary of a song (rows / chains / types / patterns played, the sounds it plays in
 * smask: cur = the slot of tracks on CUR), no pattern masks */
void py_slot_summary(const ps_song *s, psl_info *info, int cur);

/* write the song and its patterns (songbuf: PS_FILE_BYTES scratch); PSL_OK, PSL_BUSY (a pattern
 * could not be read consistently) or PSL_IO */
int py_slot_save(const ps_song *s, const uint8_t swing[2], psl_io *io, uint8_t *songbuf);

/* the header only (the slot list); PSL_OK, PSL_NOFILE (nothing there), PSL_BAD, PSL_OTHER */
int py_slot_peek(psl_io *io, psl_info *info);

/* the whole file: header, length and CRC (reads it once in pattern-sized chunks) */
int py_slot_check(psl_io *io, psl_info *info);

/* check, then which patterns differ from the ones playing now (get): bit p of diff[kind]; a
 * pattern not stored differs if the one now is not empty. diff[2]: bit s = stored sound s differs
 * from the slot now (sget); info->sbad: the stored sounds sok refuses. PSL_OK, PSL_BUSY or as check */
int py_slot_diff(psl_io *io, psl_info *info, uint16_t diff[3]);

/* check, then the song into s, the swings, every pattern through put (the ones not stored: the
 * empty pattern) and every stored sound sok takes through sput, but those in keep ([0] / [1]
 * patterns, [2] sounds; 0: none kept); nothing is changed unless the file is whole */
int py_slot_load(ps_song *s, uint8_t swing[2], psl_io *io, uint8_t *songbuf, const uint16_t *keep);

/* a shared slot file being imported (disk mode's play_s/): the slot it goes into. name "SONGnn.PSL"
 * (nn 01..16) with slot nn free: that one; else the first free slot. taken: bit n = slot n holds
 * a file; same: bit n = slot n holds this very file (then -2: nothing to do). -1: no free slot */
int py_slot_pick(const char *name, uint16_t taken, uint16_t same);

#endif
