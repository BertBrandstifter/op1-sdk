/* play: shared declarations of play.c and py_import.c / py_te.c. */
#ifndef PLAY_H
#define PLAY_H

#include <stdint.h>

#define PY_DIR_A "/yaffs2/user/play_a"          /* the imported set: two folders take turns */
#define PY_DIR_B "/yaffs2/user/play_b"
#define PY_DIR_R "/yaffs2/user/play_r"          /* rendered files (py_render.c): kept across imports */
#define PY_DIR_S "/yaffs2/user/play_s"          /* song slots SONG01..16.BIN (py_slot.c): kept across imports */

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
void py_pat_retime(uint32_t bar, uint32_t bpm_x100);    /* main: the tempo changed, playing or not */
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
int py_pat_starved(int t);
/* song slots (main thread; py_slot.h codes): a slot's header, save the song + patterns there,
 * load it (not playing: the caller reopens the tracks), delete it; GUI: the current slot */
struct psl_info;
int py_pat_slot_peek(int n, struct psl_info *info);
int py_pat_slot_save(int n);
/* patterns / sounds ([2]) differing from now, patterns played, sounds that cannot come back */
int py_pat_slot_diff(int n, uint16_t diff[3], uint16_t used[2], uint8_t *sbad);
int py_pat_slot_load(int n, const uint16_t keep[3]);                /* keep: patterns / sounds left as they are */
enum { PY_SND_OK, PY_SND_MISSING /* its SDK engine is not in this firmware (or not in PLAY) */,
       PY_SND_OTHER /* the slot is another TE engine now */, PY_SND_TE /* TE's own engine: PLAY does not play it */ };
extern uint8_t py_snd_why[8], py_snd_tag[8];    /* per sound slot, after py_pat_slot_diff */
int py_slots_import(void);                      /* disk mode's play_s/ -> free slots (py_import.c) */
int py_pat_slot_busy(void);                     /* a slot file is open (the main loop saves / loads) */
void py_pat_slot_path(char *out, int n);        /* slot n's file (64 bytes) */
void py_pat_slot_sizes(uint32_t size[2]);       /* the pattern sizes a slot file must have */
int py_pat_slot_delete(int n);
int py_pat_slot_now(int *changed);

#define PY_PAT_PENDING  0x100                   /* py_list_pattern: chosen, starts when the playing one ends */
int py_list_pattern(int t);                     /* the list row's pattern (0..13, -1 none) | PY_PAT_PENDING */                      /* a note of SYNTH track t found no engine instance (~1 s) */
int py_pat_any(void);                           /* the song plays any P track */
const struct ps_head *py_pat_head(int t);

#endif
