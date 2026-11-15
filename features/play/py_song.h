/* py_song: PLAY's tracker song (new_pattern_tape.md 2.3), host-tested, no TE calls.
 *
 *   TRACKS up to 8, each AUDIO (a file of the set folder, once or looped), SYNTH (NOTE ROLL
 *          patterns on a sound slot) or DRUM (DRUM ROLL patterns on a kit), in any order
 *   SONG   rows x 8 tracks: a SYNTH / DRUM cell = chain 00..7F (or empty), an AUDIO cell = ON / OF
 *   CHAIN  16 rows of pattern 1..14 (stored 0..13) + transpose
 *   heads  every track walks its own song column: a chain row plays its pattern once (the
 *          pattern's own length), then the next chain row, then the next song row; an empty song
 *          cell (or the song's end) = back to the row the song was started from. AUDIO: ON = the
 *          file once (its length), OF = one bar of silence.
 * The pattern / loop lengths come from a callback, so the model knows nothing about NOTE ROLL,
 * DRUM ROLL or audio files. */
#ifndef PY_SONG_H
#define PY_SONG_H

#include <stdint.h>

#define PS_ROWS     128
#define PS_CHAINS   128
#define PS_CROWS    16
#define PS_TRACKS   8
#define PS_PATTERNS 14
#define PS_EMPTY    0xff                        /* empty song cell / chain row */
#define PS_ON       1                           /* AUDIO cells */
#define PS_OF       0
#define PS_NAME     13                          /* a file's short name + NUL */

enum { PS_NONE, PS_AUDIO, PS_SYNTH, PS_DRUM };

typedef struct ps_crow { uint8_t pat; int8_t tsp; } ps_crow;

typedef struct ps_track {
    uint8_t type;                               /* PS_NONE / PS_AUDIO / PS_SYNTH / PS_DRUM */
    uint8_t sound;                              /* SYNTH / DRUM: slot 0..7, PS_EMPTY = TE's current */
    uint8_t loop;                               /* AUDIO: 1 = loop, 0 = once */
    uint8_t mute, solo;
    int8_t level;                               /* dB 0..-60 */
    uint8_t voices;                             /* SYNTH: voices at most, 1 / 2 / 4 (0 = 2) */
    uint8_t eco;                                /* SYNTH: 1 = the engine at 44.1 kHz (half the cost) */
    char file[PS_NAME];                         /* AUDIO: the file in the set folder */
    uint8_t pad2[3];
} ps_track;

typedef struct ps_song {
    uint8_t cell[PS_ROWS][PS_TRACKS];
    ps_crow chain[PS_CHAINS][PS_CROWS];
    ps_track trk[PS_TRACKS];
} ps_song;

/* SYNTH or DRUM: plays patterns */
static inline int py_song_is_pattern(const ps_song *s, int t)
{
    return s->trk[t].type == PS_SYNTH || s->trk[t].type == PS_DRUM;
}

void py_song_clear(ps_song *s);

/* length in frames of what track t plays: pattern pat (0..13) of a SYNTH / DRUM track, or the file
 * of an AUDIO track (pat unused); 0 = nothing to play there */
typedef uint32_t (*ps_len_fn)(void *ctx, int track, int pat);

typedef struct ps_head {
    uint8_t active;                             /* 0 = this track plays nothing */
    uint8_t row, crow, start_row;
    uint8_t pat;                                /* P: pattern 0..13; A: PS_ON / PS_OF */
    int8_t tsp;
    uint32_t start, len;                        /* global frames: [start, start + len) */
} ps_head;

/* start track t at song row `row` at global frame g; returns h->active */
int py_song_start(const ps_song *s, int t, int row, uint32_t g, uint32_t bar, ps_len_fn len, void *ctx,
             ps_head *h);

/* move h on to what follows it (h->start + h->len becomes the new start) */
void py_song_next(const ps_song *s, int t, uint32_t bar, ps_len_fn len, void *ctx, ps_head *h);

/* move h forward until it covers global frame g (at most `limit` steps); returns h->active */
int py_song_seek(const ps_song *s, int t, uint32_t g, uint32_t bar, ps_len_fn len, void *ctx, ps_head *h);

/* ------------------------------------------------------------------ editing */

int py_song_chain_used(const ps_song *s, int c);    /* referenced by a song cell */
int py_song_chain_empty(const ps_song *s, int c);   /* no rows */
int py_song_new_chain(const ps_song *s);            /* first chain neither used nor holding rows; -1 */
int py_song_clone_chain(ps_song *s, int c);         /* a copy under a new number; -1 */

/* ------------------------------------------------------------------ storage */

#define PS_FILE_BYTES (8 + sizeof(ps_song))
/* serialize to / from a byte buffer (magic "PSN2", CRC); 0 = ok (an older file: not loaded) */
void py_song_save(const ps_song *s, uint8_t *buf);
int py_song_load(ps_song *s, const uint8_t *buf, uint32_t n);

#endif
