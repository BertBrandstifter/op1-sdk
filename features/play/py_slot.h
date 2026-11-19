/* py_slot: PLAY's song slots (README "Songs"): a song with its patterns in one file, pure C,
 * host-tested (test_py_slot.c), no TE calls.
 *
 * A slot file holds the whole song (tracks, song grid, chains: py_song_save's PSN3 blob) and the 14
 * NOTE ROLL and 14 DRUM ROLL patterns as PLAY plays them, plus both editors' swing. Patterns equal
 * to the empty pattern are not stored (a mask says which are). Loading gives back the song and
 * all 28 patterns (the ones not stored become empty again).
 *
 *   0   "PSL1"
 *   4   u16 bytes of a NOTE ROLL pattern, u16 bytes of a DRUM ROLL pattern (other sizes: refused)
 *   8   u32 file bytes (with the CRC)
 *   12  u16 NOTE ROLL mask, u16 DRUM ROLL mask (bit p: pattern p stored)
 *   16  types[8] (PS_NONE / PS_AUDIO / PS_SYNTH / PS_DRUM per track), rows used, chains used,
 *       NOTE ROLL swing, DRUM ROLL swing, 4 bytes 0
 *   32  the song (PS_FILE_BYTES)
 *       the stored NOTE ROLL patterns in order, then the stored DRUM ROLL patterns
 *   end u32 CRC-32 of everything before it
 * Numbers little-endian. Save and load stream through callbacks with one pattern of scratch, so a
 * slot never needs the whole file in RAM. */
#ifndef PY_SLOT_H
#define PY_SLOT_H

#include <stdint.h>
#include "py_song.h"

#define PSL_SLOTS   16
#define PSL_PATS    14                          /* patterns of each kind */
#define PSL_HDR     32
#define PSL_NR      0                           /* pattern kinds */
#define PSL_DR      1

enum { PSL_OK = 0, PSL_NOFILE = -1, PSL_BAD = -2, PSL_OTHER = -3, PSL_IO = -4, PSL_BUSY = -5,
       PSL_WAIT = -6 /* (PLAY: the editors have not read their patterns yet) */ };

typedef struct psl_info {
    uint8_t type[PS_TRACKS];                    /* per track, in list order */
    uint8_t rows, chains;                       /* song rows up to the last used one, chains holding rows */
    uint8_t swing[2];
    uint16_t mask[2];                           /* stored patterns per kind */
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
} psl_io;

/* the header's summary of a song (rows / chains / types), no patterns */
void py_slot_summary(const ps_song *s, psl_info *info);

/* write the song and its patterns (songbuf: PS_FILE_BYTES scratch); PSL_OK, PSL_BUSY (a pattern
 * could not be read consistently) or PSL_IO */
int py_slot_save(const ps_song *s, const uint8_t swing[2], psl_io *io, uint8_t *songbuf);

/* the header only (the slot list); PSL_OK, PSL_NOFILE (nothing there), PSL_BAD, PSL_OTHER */
int py_slot_peek(psl_io *io, psl_info *info);

/* the whole file: header, length and CRC (reads it once in pattern-sized chunks) */
int py_slot_check(psl_io *io, psl_info *info);

/* check, then the song into s, the swings and every pattern through put (the ones not stored:
 * the empty pattern); nothing is changed unless the file is whole */
int py_slot_load(ps_song *s, uint8_t swing[2], psl_io *io, uint8_t *songbuf);

#endif
