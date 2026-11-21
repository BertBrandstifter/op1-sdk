/* py_core: PLAY's logic without TE calls (README.md, new_pattern_tape.md), host-tested.
 *
 *   set.txt      py_setfile_parse: the tempo
 *   audio        py_set_assign: an audio track plays one file of the set folder, looped or once;
 *                a file that cannot play only affects its own track (with the reason)
 *   positions    one global frame counter from bar 1; every track maps it to its own file frame
 *                (a looped file modulo its length, a once file straight until its end), so all
 *                tracks stay sample-locked by construction
 *   streaming    one ring per track: py_fill (main thread) reads the files ahead of the play
 *                position through a read function (yaffs pread on the device); py_mix (audio
 *                interrupt) consumes 64 frames per block, mixes, adds to TE's master buffers.
 *                A late ring is an underrun: silence for that track, its position still advances.
 * Threads: py_mix only touches the rings' read side and published counters; everything else is
 * main thread. 32-bit counters, single writer each. */
#ifndef PY_CORE_H
#define PY_CORE_H

#include <stdint.h>
#include <op1/audiofile.h>

#define PY_TRACKS       8
#define PY_RATE         44100
#define PY_BLOCK        64                      /* frames per audio block */
#define PY_NAME         12                      /* display name incl. NUL */
#define PY_SEGS         128                     /* segments of a song-driven track (song rows) */
#define PY_BAR_SLACK    64                      /* frames a loop may be off a whole bar */

/* ------------------------------------------------------------------ set.txt */

typedef struct py_setfile {
    uint32_t bpm_x100;                          /* 0 = no bpm line */
    uint16_t bad_line;                          /* first line not understood (1-based), 0 = none */
} py_setfile;

/* lines: "bpm 120" / "bpm 98.5", "# comment", empty; the lines of earlier versions ("audio ...",
 * "pattern ...", "loop ...") are accepted and ignored */
void py_setfile_parse(const char *txt, uint32_t n, py_setfile *s);

/* ------------------------------------------------------------------ audio tracks */

typedef struct py_file {                        /* an audio file in the set folder */
    char name[13];                              /* short name, e.g. "SONG.WAV" */
    uint32_t size;
    aud_info a;                                 /* its parsed header */
} py_file;

typedef struct py_segs {                        /* the segments of a song-driven track */
    uint8_t n, row0;                            /* segments; the song row of seg[0] */
    uint8_t seg[PY_SEGS];                       /* nonzero = the file once (its length), 0 = a bar of silence */
    uint8_t row[PY_SEGS];                       /* the song row of each (row0 + i, or a loop's rows again) */
    uint32_t period;                            /* frames of the whole circle */
    uint32_t origin;                            /* the global frame seg[0] starts at */
} py_segs;

typedef struct py_track {                       /* what audio track k plays */
    uint8_t used, channels, big_endian, loop;
    int8_t file;                                /* index into the files */
    uint32_t data_off, frames;                  /* PCM inside the file */
    char err[24];                               /* why it does not play ("" = it does) */
    /* song-driven (py_set_song): the track plays cur's segments in a circle; loop is ignored
     * then. pending: from global frame sw on it plays nxt instead (py_set_cue, live mode). */
    uint8_t song, pending;
    uint32_t sw;
    py_segs cur, nxt;
} py_track;

typedef struct py_set {
    uint32_t bpm_x100, bar_frames;              /* the clock: set.txt's tempo, else TE's */
    py_track t[PY_TRACKS];
} py_set;

/* tempo from set.txt (sf may be 0) or TE; every track unused */
void py_set_init(py_set *s, const py_setfile *sf, uint32_t te_bpm_x100);

/* audio track k plays file `name` of files (case-insensitive), looped or once; returns 1 if it
 * plays, else 0 with t[k].err set ("NO FILE", "SONG.WAV: NOT 44.1 KHZ", ...) */
int py_set_assign(py_set *s, int k, const py_file *files, int n, const char *name, int loop);

/* audio track k follows the song: n segments (nonzero = the file once, 0 = a bar of silence),
 * repeated; n = 0: silent; n < 0: free again (looped or once, as assigned). Main thread, with the
 * rings re-seeked afterwards (py_seek + py_fill): what they hold was read with the old mapping. */
void py_set_song(py_set *s, int k, const uint8_t *seg, int n, int row0);
/* the same with the song row of each segment (rows: n entries; 0 = row0 + i): a loop section's
 * rows come again (py_song_audio_segs) */
void py_set_song_rows(py_set *s, int k, const uint8_t *seg, const uint8_t *rows, int n, int row0);

/* the song row (row0 + segment) a song-driven track k plays at global frame g; -1 = none (free,
 * silent, unused) */
int py_set_row(const py_set *s, int k, uint32_t g);

/* frames per 4/4 bar at bpm x 100 (rounded) */
uint32_t py_bar_frames(uint32_t bpm_x100);

/* the file frame track k plays at global frame g, and how many frames from there are contiguous;
 * -1 = silent from here (a once track past its end, an unused track) */
int32_t py_map(const py_set *s, int k, uint32_t g, uint32_t *run);

/* ------------------------------------------------------------------ streaming + mix */

typedef struct py_ring {
    int16_t *buf;                               /* size frames x channels, native int16 */
    uint32_t size;                              /* frames, a power of two */
    volatile uint32_t wr;                       /* global frame up to which buf is filled */
    uint8_t ch;
} py_ring;

typedef struct py_mixer {
    const py_set *set;
    py_ring ring[PY_TRACKS];
    volatile uint32_t pos;                      /* global frame of the next block (audio thread) */
    volatile uint8_t playing;                   /* audio thread consumes while set */
    volatile uint8_t ended;                     /* unused since v0.2 (no set end any more) */
    volatile int16_t gain[PY_TRACKS];           /* Q15 after mute / solo, per track (any type) */
    volatile int16_t master;                    /* Q15 */
    volatile int16_t te_volume;                 /* Q15 on our sum (32767: TE applies its volume after) */
    volatile uint16_t peak[PY_TRACKS];          /* per-track peak since the GUI last cleared it */
    volatile uint16_t peak_out;
    volatile uint32_t underruns[PY_TRACKS];
    volatile uint32_t clips;
    /* pattern tracks (SYNTH / DRUM): mono blocks their voices rendered (int16 scale) at the
     * track's index, mixed in py_mix with the track's gain into both channels, cleared there */
    int32_t pin[PY_TRACKS][PY_BLOCK];
    volatile uint8_t pin_used;                  /* bit k: pin[k] holds this block */
    /* SYNTH tracks: their sum after each track's gain (one shared decimator), mono at unity */
    int32_t ext[PY_BLOCK];
    volatile uint8_t ext_used;
    uint32_t chunk;                             /* frames per read (main thread) */
    uint8_t next;                               /* round-robin start for py_fill */
    uint32_t read_errors;
} py_mixer;

/* read n bytes at off of track k's file into buf: returns n, anything else = error */
typedef int (*py_read_fn)(void *ctx, int k, uint32_t off, void *buf, uint32_t n);

/* main thread: (re)start every ring at global frame g (playing must be 0) */
void py_seek(py_mixer *m, uint32_t g);

/* main thread: read ahead until every ring is full or budget bytes were read; scratch holds one
 * chunk (chunk x 2 channels x 2 bytes). Returns bytes read. */
uint32_t py_fill(py_mixer *m, py_read_fn rd, void *ctx, uint32_t budget, uint8_t *scratch);

/* frames ring k holds ahead of the play position */
uint32_t py_ahead(const py_mixer *m, int k);

/* audio interrupt: one block (64 frames) added into TE's 32-bit words l[i * stride], r[i * stride]
 * (sample in the top bits), saturating; advances pos. Nothing when not playing. TE's master
 * buffers before its volume (0xff800e34 / 0xff800f34): stride 1; interleaved tx: l = tx, r = tx + 1,
 * stride 2. */
void py_mix(py_mixer *m, int32_t *l, int32_t *r, int stride);

/* live: song-driven track k changes to other segments (n = 0: silence) when the segment it plays
 * `guard` frames from now ends (a silent track: at the next bar); the ring is taken back to that
 * frame so the read-ahead fills it anew. A second cue before the switch replaces the first.
 * Main thread, while playing; guard must cover one main-loop pass. Returns the switch frame. */
uint32_t py_set_cue(py_set *s, py_mixer *m, int k, const uint8_t *seg, int n, int row0, uint32_t guard);
uint32_t py_set_cue_rows(py_set *s, py_mixer *m, int k, const uint8_t *seg, const uint8_t *rows, int n, int row0,
                         uint32_t guard);

/* the song row a pending cue of track k goes to; -1 = none pending at global frame g */
int py_set_cued(const py_set *s, int k, uint32_t g);

/* Q15 gain of dB x 10 (-600 .. 0; above 0 = unity, below -60 dB = 0) */
int16_t py_db_q15(int db10);

#endif
