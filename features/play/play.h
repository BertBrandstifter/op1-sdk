/* play: shared declarations of play.c and py_import.c / py_te.c. */
#ifndef PLAY_H
#define PLAY_H

#include <stdint.h>

#define PY_DIR_A "/yaffs2/user/play_a"          /* the imported set: two folders take turns */
#define PY_DIR_B "/yaffs2/user/play_b"
#define PY_DIR_R "/yaffs2/user/play_r"          /* rendered files (py_render.c): kept across imports */

const char *py_dir(void);                       /* the active set folder (py_import.c) */
int py_active(void);
void py_clear_dir(const char *dir);

/* py_import.c. TE's disk sync, before TE deletes play/: copy play/ from the image into the
 * inactive set folder and make it active; 1 = a play/ folder was there */
int py_import_stage(void);
/* main thread: a new set became active */
int py_import_pending(void);
/* main thread, the old set closed: empty the folder that is no longer active */
void py_import_run(void);

/* py_pattern.c: the pattern tracks */
struct ps_song;
struct ps_head;
extern struct ps_song py_song;
void py_pat_load(const char *dir);              /* main: SONG.BIN of the set folder (or empty) */
void py_pat_main(uint32_t bar, uint32_t bpm_x100);  /* main, every pass */
void py_pat_start(uint32_t g);                  /* main, not playing: heads at global frame g */
void py_pat_set_start(int row);                 /* main, not playing: the song row play starts from */
int py_pat_start_row(void);
void py_pat_audition(int t, int chain);         /* main, not playing: track t plays that chain alone (t < 0: off) */
int py_pat_audition_track(void);                /* -1 = no audition */
void py_pat_cue(int t, int row);                /* live: pattern track t jumps to that song row at its chain's end */
int py_pat_cued(int t);                         /* the row it is cued to, -1 = none */
void py_pat_block(uint32_t pos);                /* audio interrupt, playing: one block */
void py_pat_step(uint32_t pos, int step);       /* the same in steps: 0..7 a track, 8 the buses */
void py_pat_sound_set(int t);                   /* the track's sound was set by hand: it wins over the chain row's */
int py_pat_slot(int t);                         /* the sound / kit slot track t plays now */
void py_pat_hd(int on);                         /* main, not playing: SYNTH tracks without ECO, all voices */
uint32_t py_pat_song_frames(int row);           /* the longest pattern column played once from that row */
void py_pat_idle(void);                         /* audio interrupt, not playing */
void py_pat_dirty(void);                        /* the song was edited */
void py_pat_tracks_changed(void);               /* track types changed: engine instances anew */
int py_pat_inst(int t);                         /* SYNTH track t's engine instance (0 = none) */
int py_pat_any(void);                           /* the song plays any P track */
const struct ps_head *py_pat_head(int t);

#endif
