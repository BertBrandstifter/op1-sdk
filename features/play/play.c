/* play: the PLAY tape screen (README.md, new_pattern_tape.md). TE-facing part; the logic is in
 * py_core.c, py_song.c, py_drum.c, py_synth.c (host-tested) and py_pattern.c.
 *
 *   tracks     one list of up to 8 tracks, each AUDIO (a WAV / AIF of the set folder, looped or
 *              once), SYNTH (NOTE ROLL patterns on a pooled engine instance and a sound slot) or
 *              DRUM (DRUM ROLL patterns on a kit); added from the "+ ADD" row, removed with cut
 *              (twice). The list, the choices, levels, mutes and solos live in SONG.BIN.
 *   set folder /yaffs2/user/play_a|b: the audio files + SET.TXT (tempo), imported from disk mode's
 *              play/ folder (py_import.c). Read at boot, after an import and on orange press.
 *   streaming  every AUDIO track has a ring in TE's sample heap (16384 frames); the main loop
 *              (main_tick) reads ahead with yaffs pread, at most PY_BUDGET bytes per pass.
 *   audio      an "after" entry hook on TE's metronome mix (0x019de548, audio interrupt, every
 *              block): the pattern tracks render (py_pattern.c), py_mix sums everything, and the
 *              sum joins TE's master bus (int16 L 0xff800d34, R 0xff800db4) like the tape does:
 *              TE's master EQ, master FX, the mixer's drive and the volume act on PLAY too.
 *   views      the track list (here), the song view and the chain view (py_view.c): join / orange
 *              press between them; the tracker song itself is py_song.c.
 *   screen     the tape key opens PLAY on its track list (also from the song / chain view); join
 *              opens the song view, from any screen (another screen: PLAY injects a tape key
 *              press, TE switches to the tape screen, PLAY opens there in the song view). PLAY is
 *              drawn over TE's frame and takes every tape-mode key that would move, record or edit
 *              TE's tape; playback keeps running on other screens.
 * Threads: KeyHandler (keys, knobs) edits the song's track settings and sets requests; file I/O
 * and allocation in the main loop; mixing in the audio interrupt; drawing in the GUI thread. */
#include <op1/te246.h>
#include <op1/gfx.h>
#include <op1/audiofile.h>
#include "py_core.h"
#include "py_song.h"
#include "play.h"
#include "py_yaffs.h"
#include "../enginehost/eh_host.h"
#include "py_synth.h"                          /* PY_SYN_CAP_DEFAULT */
#include "py_tev.h"
#include "py_ui.h"
#include "py_slot.h"
#include "py_draw.h"

#define RING_FRAMES     16384                   /* 372 ms */
#define CHUNK_FRAMES    4096                    /* 16 KB stereo per read (~2.3 ms on the device) */
#define PY_BUDGET       (48u << 10)             /* bytes read per main-loop pass at most */
#define PREFILL         (2 * CHUNK_FRAMES)      /* frames per track read before play starts */
#define MAXFILES        16
#define MAX_DRUM        2                       /* CPU */
#define TE_BUS_L        ((int16_t *)0xff800d34)  /* int16[64]: TE's master bus (before EQ / FX) */
#define TE_BUS_R        ((int16_t *)0xff800db4)
#define BUS_SCALE       12409                   /* Q15 0.38: TE's drive takes the bus ~x2.64 up */

/* ------------------------------------------------------------------ state */

static py_set set;                              /* main thread writes, others read */
static py_setfile setfile;
static py_file files[MAXFILES];
static uint8_t file_r[MAXFILES];                /* the file is in the render folder (PY_DIR_R) */
static volatile int nfiles;
static int fds[PY_TRACKS] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static uint8_t *scratch;
py_mixer py_mx;                                 /* bench reads it */
volatile uint32_t py_blocks;                    /* volume-hook calls (bench, debug) */

/* user state (KeyHandler writes, main / GUI read) */
static volatile uint8_t shown = 1;              /* PLAY instead of TE's tape screen */
volatile uint8_t py_view;                       /* PY_V_LIST / PY_V_SONG / PY_V_CHAIN */
static volatile uint8_t cursor;                 /* row: a track (in list order) or the ADD row */
static volatile uint8_t add_type = PS_SYNTH;    /* what the ADD row adds */
static volatile int8_t cut_armed = -1;          /* track whose removal waits for a second cut */
static volatile uint8_t insert_armed;           /* insert pressed once: the second one inserts */
static volatile int8_t master_db;
static volatile const char *note_msg;           /* a short note in the footer (GUI counts it down) */
static volatile uint16_t note_frames;

/* requests to the main thread */
static volatile uint8_t req_load = 1, req_tracks, req_resume, req_play, req_stop, req_bar;
static uint8_t demos_due = 1;                   /* py_demos_install not done yet this boot */
static volatile int8_t req_bar_step;
static volatile uint8_t req_start, req_start_row, req_start_chain, req_segs, req_rescan;
static volatile int8_t req_start_aud;           /* req_start: the track that auditions a chain, -1 */
static volatile uint8_t req_slot, req_slot_n;   /* the songs page: PY_SL_* on slot n */
static volatile uint32_t req_slot_keep;         /* PY_SL_LOAD: the patterns kept (NOTE ROLL bits, DRUM ROLL << 16) */
static volatile uint8_t req_slot_skeep;         /* PY_SL_LOAD: the sounds kept */
static volatile uint8_t req_cue_on[PS_TRACKS], req_cue_row[PS_TRACKS];   /* live: AUDIO tracks' cues */
#define CUE_GUARD       4096                    /* frames a cue's switch lies ahead at least (93 ms) */

volatile uint8_t py_loading;                    /* GUI: "LOADING" */

/* the track list's layout (v0.17): LANES (0) or ROWS (1); tape on the list toggles it, kept as the
 * marker file below (there = ROWS), read at the first main-loop pass, written by the main loop */
#define LAYOUT_FILE     PY_DIR_S "/ROWS.LAY"
volatile uint8_t py_layout;
static uint8_t layout_due = 1;                  /* not read yet this boot */
static volatile uint8_t req_layout;             /* toggled: write the marker */

/* ------------------------------------------------------------------ the track list */

/* rows: the tracks in index order, then the ADD row (if there is room) */
int py_rows(int *list)
{
    int t, n = 0;

    for (t = 0; t < PS_TRACKS; t++)
        if (py_song.trk[t].type != PS_NONE)
            list[n++] = t;
    return n;
}

static int count(int type)
{
    int t, n = 0;

    for (t = 0; t < PS_TRACKS; t++)
        n += py_song.trk[t].type == type;
    return n;
}

static int can_add(int type)
{
    if (count(PS_NONE) == 0)
        return 0;
    return type == PS_DRUM ? count(PS_DRUM) < MAX_DRUM : 1;    /* SYNTH: any number, 3 sound at once */
}

/* ------------------------------------------------------------------ files (main thread) */

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static int up(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static int is_audio_name(const char *n)        /* .WAV / .AIF / .AIFF */
{
    const char *x = 0;

    for (; *n; n++)
        if (*n == '.')
            x = n + 1;
    if (!x)
        return 0;
    return (up(x[0]) == 'W' && up(x[1]) == 'A' && up(x[2]) == 'V' && !x[3])
        || (up(x[0]) == 'A' && up(x[1]) == 'I' && up(x[2]) == 'F' && (!x[3] || (up(x[3]) == 'F' && !x[4])));
}

static int yaffs_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    return PY_PREAD(*(int *)ctx, buf, n, (int32_t)off);
}

static int track_read(void *ctx, int k, uint32_t off, void *buf, uint32_t n)
{
    (void)ctx;
    return fds[k] < 0 ? -1 : PY_PREAD(fds[k], buf, n, (int32_t)off);
}

static const char *set_dir;                     /* the folder the set was loaded from */

static void path_of(char *out, const char *name)
{
    TE_SNPRINTF(out, 64, "%s/%s", set_dir, name);
}

static void file_path(char *out, int i)         /* file i of the pool: set folder or render folder */
{
    TE_SNPRINTF(out, 64, "%s/%s", file_r[i] ? PY_DIR_R : set_dir, files[i].name);
}

/* the audio tracks' files closed, rings freed (the interrupt stops reading them first) */
static void close_audio(void)
{
    int k;

    py_mx.playing = 0;
    for (k = 0; k < PY_TRACKS; k++) {
        if (fds[k] >= 0)
            TE_FILE_CLOSE(fds[k]);
        fds[k] = -1;
        if (py_mx.ring[k].buf) {
            int16_t *b = py_mx.ring[k].buf;

            py_mx.ring[k].buf = 0;
            TE_FREE(b);
        }
        set.t[k].used = 0;
    }
}

static uint32_t te_bpm_x100(void)
{
    float b = *TE_BPM;

    return b >= 40.0f && b <= 220.0f ? (uint32_t)(b * 100.0f + 0.5f) : 12000;
}

/* every AUDIO track with song cells follows them from the start row (py_set_song), the others
 * play free; the rings must be re-seeked afterwards */
static uint8_t seg_buf[PS_ROWS], row_buf[PS_ROWS];

static void song_audio(void)
{
    int k;

    for (k = 0; k < PY_TRACKS; k++) {
        req_cue_on[k] = 0;                      /* cues belong to the old segments */
        if (py_song.trk[k].type == PS_AUDIO && set.t[k].used)
            py_set_song_rows(&set, k, seg_buf, row_buf, py_song_audio_segs(&py_song, k, py_pat_start_row(), seg_buf,
                                                                           row_buf), py_pat_start_row());
    }
}

/* live: the AUDIO tracks' cues (py_req_cue): the track changes to the cued row's cells when the
 * segment it plays ends */
static void audio_cues(void)
{
    int k;

    for (k = 0; k < PY_TRACKS; k++) {
        int row = req_cue_row[k], n;

        if (!req_cue_on[k])
            continue;
        req_cue_on[k] = 0;
        if (!py_mx.playing || py_song.trk[k].type != PS_AUDIO || !set.t[k].used || !set.t[k].song)
            continue;
        n = py_song_audio_segs(&py_song, k, row, seg_buf, row_buf);
        if (n >= 0)
            py_set_cue_rows(&set, &py_mx, k, seg_buf, row_buf, n, row, CUE_GUARD);
    }
}

/* every AUDIO track: its file assigned, opened, a ring allocated */
static void open_audio(void)
{
    char path[64];
    int k;

    close_audio();
    py_set_init(&set, 0, te_bpm_x100());       /* the clock: TE's tempo page (set.txt's bpm: no more, v0.14) */
    if (!scratch)
        scratch = TE_HEAP_MALLOC(TE_HEAP_SAMPLES, CHUNK_FRAMES * 4);
    for (k = 0; k < PY_TRACKS; k++) {
        const ps_track *t = &py_song.trk[k];
        py_ring *r = &py_mx.ring[k];

        if (t->type != PS_AUDIO || !py_set_assign(&set, k, files, nfiles, t->file, t->loop))
            continue;
        file_path(path, set.t[k].file);
        fds[k] = TE_FILE_OPEN(path, TE_FILE_RDONLY, TE_FILE_MODE);
        r->ch = set.t[k].channels;
        r->size = RING_FRAMES;
        r->buf = scratch ? TE_HEAP_MALLOC(TE_HEAP_SAMPLES, RING_FRAMES * r->ch * 2u) : 0;
        if (fds[k] < 0 || !r->buf) {
            TE_SNPRINTF(set.t[k].err, sizeof(set.t[k].err), "%s", fds[k] < 0 ? "CANNOT OPEN" : "NO MEMORY");
            if (fds[k] >= 0)
                TE_FILE_CLOSE(fds[k]);
            fds[k] = -1;
            TE_FREE(r->buf);
            r->buf = 0;
            set.t[k].used = 0;
        }
    }
    song_audio();
    py_mx.set = &set;
    py_mx.chunk = CHUNK_FRAMES;
}

/* the audio files of one folder into the pool from index n on (set folder: SET.TXT too; render
 * folder: names the set folder has already are skipped); returns the new count */
static int scan_dir(const char *dir, int render, int n)
{
    static uint32_t st[32];                     /* TE's stat buffer is 0x48 bytes */
    static char txt[4096] __attribute__((aligned(4)));   /* yaffs: even addresses */
    char path[64];
    void *d = PY_OPENDIR(dir);
    uint8_t *de;

    while (d && (de = PY_READDIR(d)) != 0) {
        const char *name = (const char *)de + 11;
        py_file *f = &files[n];
        int fd, i;

        if (name[0] == '.')
            continue;
        TE_SNPRINTF(path, sizeof(path), "%s/%s", dir, name);
        if (PY_STAT(path, st) < 0 || !(st[0x08 / 4] & 0x8000))
            continue;                           /* not a regular file */
        if (!render && str_eq(name, "SET.TXT")) {
            fd = TE_FILE_OPEN(path, TE_FILE_RDONLY, TE_FILE_MODE);
            if (fd >= 0) {
                int r = PY_PREAD(fd, txt, st[0x1c / 4] < sizeof(txt) ? st[0x1c / 4] : sizeof(txt), 0);

                if (r > 0)
                    py_setfile_parse(txt, (uint32_t)r, &setfile);
                TE_FILE_CLOSE(fd);
            }
            continue;
        }
        if (!is_audio_name(name) || n >= MAXFILES)
            continue;
        for (i = 0; i < n && !str_eq(files[i].name, name); i++)
            ;
        if (i < n)
            continue;                           /* the set has a file of that name */
        for (i = 0; name[i] && i < 12; i++)
            f->name[i] = name[i];
        f->name[i] = 0;
        f->size = st[0x1c / 4];
        fd = TE_FILE_OPEN(path, TE_FILE_RDONLY, TE_FILE_MODE);
        if (fd < 0)
            continue;
        aud_parse(yaffs_read, &fd, f->size, &f->a);
        TE_FILE_CLOSE(fd);
        file_r[n] = (uint8_t)render;
        n++;
    }
    if (d)
        PY_CLOSEDIR(d);
    return n;
}

/* read the set folder and the render folder: the audio files' headers, SET.TXT, the song (not
 * when only the files are read again: after a render); then the tracks */
static void load_set(int song)
{
    int n;

    py_loading = 1;
    close_audio();
    set_dir = py_dir();
    py_pat_audition(-1, 0);                     /* a new song: no audition, from song row 00 */
    py_pat_set_start(0);
    if (song)
        py_pat_load(set_dir);
    py_setfile_parse("", 0, &setfile);
    n = scan_dir(set_dir, 0, 0);
    n = scan_dir(PY_DIR_R, 1, n);
    nfiles = n;
    open_audio();
    req_play = req_stop = req_bar = 0;          /* transport requests belong to the old set */
    py_seek(&py_mx, 0);
    py_fill(&py_mx, track_read, 0, PREFILL * 4u * PY_TRACKS, scratch);
    py_loading = 0;
}

static uint32_t clock_bpm(void) { return set.bpm_x100 ? set.bpm_x100 : te_bpm_x100(); }
static uint32_t clock_bar(void) { return set.bar_frames ? set.bar_frames : py_bar_frames(te_bpm_x100()); }

/* TE's tempo page is PLAY's clock (v0.14): a new tempo there, playing or not, retimes the song.
 * The pattern clock and the play position scale in place (py_pat_retime: every track stays at
 * its place in the bar, voices sound on); AUDIO tracks (files are not stretched) get their song
 * segments anew and their rings re-read at the scaled position. While AUDIO tracks play, the
 * tempo must rest for TEMPO_SETTLE passes first (a turn of the BPM knob = one re-read, not one
 * per step). */
#define TEMPO_SETTLE    12                      /* main-loop passes (~0.1 s) */
static uint8_t tempo_wait;

static int audio_used(void)
{
    int k;

    for (k = 0; k < PY_TRACKS; k++)
        if (py_song.trk[k].type == PS_AUDIO && set.t[k].used)
            return 1;
    return 0;
}

static void follow_tempo(void)
{
    uint32_t bpm = te_bpm_x100(), bar;
    int aud, playing;

    if (bpm == set.bpm_x100) {
        tempo_wait = 0;
        return;
    }
    aud = audio_used();
    if (aud && py_mx.playing && ++tempo_wait < TEMPO_SETTLE)
        return;
    tempo_wait = 0;
    bar = py_bar_frames(bpm);
    playing = py_mx.playing;
    if (aud)
        py_mx.playing = 0;                      /* (the rings are re-read below) */
    py_pat_retime(bar, bpm);                    /* (scales py_mx.pos too) */
    set.bpm_x100 = bpm;
    set.bar_frames = bar;
    if (aud) {
        song_audio();
        py_seek(&py_mx, py_mx.pos);
        py_fill(&py_mx, track_read, 0, PREFILL * 4u * PY_TRACKS, scratch);
        py_mx.playing = (uint8_t)playing;
    }
}

/* stop, move to global frame g, read some ahead, resume if it was playing */
static void seek_to(uint32_t g, int play)
{
    py_mx.playing = 0;
    py_seek(&py_mx, g);
    py_fill(&py_mx, track_read, 0, PREFILL * 4u * PY_TRACKS, scratch);
    py_pat_start(g);
    py_mx.playing = (uint8_t)play;
}

/* ------------------------------------------------------------------ gains (any thread) */

static void update_gains(void)
{
    int k, solo = 0, aud = py_pat_audition_track(), render = py_rd_busy();

    for (k = 0; k < PY_TRACKS; k++)
        solo |= py_song.trk[k].type != PS_NONE && py_song.trk[k].solo;
    for (k = 0; k < PY_TRACKS; k++) {
        const ps_track *t = &py_song.trk[k];
        int on = t->type != PS_NONE && (aud >= 0 ? k == aud : solo ? t->solo : !t->mute);

        py_mx.gain[k] = on ? py_db_q15(t->level * 10) : 0;
    }
    py_mx.master = render ? 32767 : py_db_q15(master_db * 10);  /* a render: the master at 0 dB */
}

/* ------------------------------------------------------------------ audio interrupt */

static int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* entry hook "after" on 0x019de548 (TE's metronome mix into the master bus, once per block) */
/* CPU: CYCLES PLAY spends per block, over windows of CPU_WINDOW blocks, as a share of the time
 * between blocks (permille); per pattern track (py_pattern.c counts) and in all */
#define CPU_WINDOW      172                     /* ~0.25 s */
extern volatile uint32_t py_trk_cyc[PS_TRACKS], py_dec_cyc;
volatile uint16_t py_cpu_trk[PS_TRACKS], py_cpu_all;   /* permille, GUI reads */

static inline uint32_t cycles(void)
{
    uint32_t c;

    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

static uint16_t permille(uint32_t w, uint32_t p)
{
    while (w > 4000000u) {                      /* keep w * 1000 in 32 bits */
        w >>= 1;
        p >>= 1;
    }
    return (uint16_t)(p ? w * 1000u / p : 0);
}

static void cpu_window(uint32_t own, uint32_t now)
{
    static uint32_t last, period, work;
    static uint16_t n;
    static uint8_t ccen;
    int t;

    if (!ccen) {                                /* the cycle counter runs (SYSCFG.CCEN) */
        uint32_t sc;

        __asm__ volatile("%0 = SYSCFG;" : "=d"(sc));
        if (!(sc & 2))
            __asm__ volatile("SYSCFG = %0; CSYNC;" : : "d"(sc | 2));
        ccen = 1;
        last = cycles();
        return;
    }
    period += now - last;
    last = now;
    work += own;
    if (++n < CPU_WINDOW)
        return;
    for (t = 0; t < PS_TRACKS; t++) {
        py_cpu_trk[t] = permille(py_trk_cyc[t], period);
        py_trk_cyc[t] = 0;
    }
    py_cpu_all = permille(work, period);
    py_dec_cyc = 0;
    period = work = 0;
    n = 0;
}

void py_bus_hook(void)
{
    static int32_t l[PY_BLOCK], r[PY_BLOCK];
    uint32_t c0 = cycles();
    int i;

    py_blocks++;
    if (py_rd_audio()) {                        /* a render: into its file, not the bus */
        py_pat_idle();
        cpu_window(0, c0);
        return;
    }
    if (!py_mx.playing) {
        py_pat_idle();                          /* the editors' playheads: not PLAY's */
        if (py_cpu_all) {
            for (i = 0; i < PS_TRACKS; i++)
                py_cpu_trk[i] = 0;
            py_cpu_all = 0;
        }
        cpu_window(0, c0);
        return;
    }
    py_pat_block(py_mx.pos);
    for (i = 0; i < PY_BLOCK; i++)
        l[i] = r[i] = 0;
    py_mix(&py_mx, l, r, 1);
    for (i = 0; i < PY_BLOCK; i++) {
        TE_BUS_L[i] = sat16(TE_BUS_L[i] + (((l[i] >> 16) * BUS_SCALE) >> 15));
        TE_BUS_R[i] = sat16(TE_BUS_R[i] + (((r[i] >> 16) * BUS_SCALE) >> 15));
    }
    cpu_window(cycles() - c0, c0);
}

/* ------------------------------------------------------------------ main loop */

/* for py_render.c (main thread) */
void py_song_audio(void) { song_audio(); }
void py_update_gains(void) { update_gains(); }
void py_seek_to(uint32_t g, int play) { seek_to(g, play); }
uint32_t py_clock_bar(void) { return clock_bar(); }
void py_req_rescan(void) { req_rescan = 1; }

/* the songs page's request: scan the slots' headers, save, load (stops, the song and its
 * patterns replaced, the AUDIO tracks reopened, bar 1, song row 00) or delete a slot */
/* ------------------------------------------------------------------ the render folder (files page) */
py_rfile py_rfiles[PY_RFILES];
volatile uint8_t py_nrfiles;

static int name_lt(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (uint8_t)*a < (uint8_t)*b;
}

/* main: every file of the render folder with its header, sorted by name */
static void scan_renders(void)
{
    static uint32_t st[32];                     /* TE's stat buffer is 0x48 bytes */
    char path[64];
    void *d = PY_OPENDIR(PY_DIR_R);
    uint8_t *de;
    int n = 0, i, j, fd;

    while (d && n < PY_RFILES && (de = PY_READDIR(d)) != 0) {
        const char *name = (const char *)de + 11;
        py_rfile f;
        aud_info a;

        if (name[0] == '.')
            continue;
        for (i = 0; name[i] && i < 12; i++)
            f.name[i] = name[i];
        f.name[i] = 0;
        if (name[i])
            continue;                           /* (not a short name: not PLAY's) */
        TE_SNPRINTF(path, sizeof(path), "%s/%s", PY_DIR_R, f.name);
        if (PY_STAT(path, st) < 0 || !(st[0x08 / 4] & 0x8000))
            continue;
        f.size = st[0x1c / 4];
        f.ch = 0;
        f.frames = f.rate = 0;
        fd = TE_FILE_OPEN(path, TE_FILE_RDONLY, TE_FILE_MODE);
        if (fd >= 0) {
            if (aud_parse(yaffs_read, &fd, f.size, &a) == AUD_OK) {
                f.ch = a.channels;
                f.frames = a.frames;
                f.rate = a.rate;
            }
            TE_FILE_CLOSE(fd);
        }
        for (j = n; j > 0 && name_lt(f.name, py_rfiles[j - 1].name); j--)
            py_rfiles[j] = py_rfiles[j - 1];
        py_rfiles[j] = f;
        n++;
    }
    if (d)
        PY_CLOSEDIR(d);
    py_nrfiles = (uint8_t)n;
}

/* main: render n deleted (playback stops; an AUDIO track playing it shows GONE) */
static int delete_render(int n)
{
    char path[64];
    int r;

    if (n >= py_nrfiles)
        return -1;
    TE_SNPRINTF(path, sizeof(path), "%s/%s", PY_DIR_R, py_rfiles[n].name);
    close_audio();                              /* (stops; the file may be open) */
    r = PY_UNLINK(path) < 0 ? -1 : 0;
    load_set(0);                                /* the pool without it */
    update_gains();
    return r;
}

static void slot_op(void)
{
    int op = req_slot, n = req_slot_n, r = 0;
    uint32_t keep = req_slot_keep;
    uint8_t skeep = req_slot_skeep;

    req_slot = 0;
    switch (op) {
    case PY_SL_SCAN:
        py_v_slot_scan(-1);
        return;
    case PY_SL_SAVE:
        r = py_pat_slot_save(n);
        break;
    case PY_SL_DIFF: {                          /* before a load: patterns / sounds that would change */
        uint16_t diff[3], used[2];
        uint8_t sbad;

        r = py_pat_slot_diff(n, diff, used, &sbad);
        if (r != PSL_OK || diff[0] || diff[1] || diff[2]) {
            py_v_slot_ask(n, r, diff, used, sbad);  /* the page asks (or says what went wrong) */
            return;
        }
        op = PY_SL_LOAD;                        /* nothing would change: loaded at once */
        keep = 0;
        skeep = 0;
    }   /* fall through */
    case PY_SL_LOAD: {
        uint16_t k[3];

        k[0] = (uint16_t)keep;
        k[1] = (uint16_t)(keep >> 16);
        k[2] = skeep;
        close_audio();                          /* (stops) */
        py_pat_audition(-1, 0);
        py_pat_set_start(0);
        r = py_pat_slot_load(n, k);
        req_play = req_stop = req_bar = 0;
        open_audio();
        update_gains();
        seek_to(0, 0);
        break;
    }
    case PY_SL_DELETE:
        r = py_pat_slot_delete(n);
        break;
    case PY_SL_FSCAN:
    case PY_SL_FDELETE:
        r = op == PY_SL_FDELETE ? delete_render(n) : 0;
        scan_renders();
        py_v_files_done(op, n, r);
        return;
    default:
        return;
    }
    py_v_slot_scan(n);
    py_v_slot_done(op, n, r);
}

void py_main(void)
{
    if (py_rd_main()) {                         /* a render runs: only its read-ahead and the patterns */
        py_pat_main(clock_bar(), clock_bpm());
        py_fill(&py_mx, track_read, 0, PY_BUDGET, scratch);
        return;
    }
    if (req_rescan) {                           /* a rendered file is new in the pool */
        req_rescan = 0;
        load_set(0);
        update_gains();
    }
    if (py_import_pending()) {                  /* disk mode left with a play/ folder */
        close_audio();
        py_import_run();
        req_load = 1;
    }
    if (demos_due && !req_load && py_demos_install() >= 0)
        demos_due = 0;                          /* the image's demo songs into free slots (once) */
    if (layout_due) {                           /* the layout chosen before (v0.17) */
        static uint32_t st[32];

        layout_due = 0;
        if (!req_layout)
            py_layout = PY_STAT(LAYOUT_FILE, st) >= 0;
    }
    if (req_layout) {
        req_layout = 0;
        if (py_layout) {
            int fd;

            PY_MKDIR(PY_DIR_S, 0);
            fd = TE_FILE_OPEN(LAYOUT_FILE, TE_FILE_CREATE, TE_FILE_CREATE_MODE);
            if (fd >= 0)
                TE_FILE_CLOSE(fd);
        } else {
            PY_UNLINK(LAYOUT_FILE);
        }
    }
    if (req_load) {
        req_load = 0;
        req_tracks = 0;
        py_mx.te_volume = 32767;                /* TE applies its volume after PLAY's mix */
        load_set(1);
        update_gains();
    }
    if (req_slot)
        slot_op();
    if (req_tracks) {                           /* a track added, removed or changed */
        int playing = py_mx.playing || req_resume;

        req_tracks = 0;
        req_resume = 0;
        open_audio();
        update_gains();
        seek_to(py_mx.pos, playing);
    }
    follow_tempo();
    py_pat_main(clock_bar(), clock_bpm());
    if (req_segs) {                             /* AUDIO song cells changed: the segments, the rings anew */
        int playing = py_mx.playing;

        req_segs = 0;
        py_mx.playing = 0;
        song_audio();
        seek_to(py_mx.pos, playing);
    }
    if (req_start) {                            /* song view: play from a row; chain view: audition */
        req_start = 0;
        req_play = 0;
        py_mx.playing = 0;
        py_pat_audition(req_start_aud, req_start_chain);
        py_pat_set_start(req_start_aud >= 0 ? py_pat_start_row() : req_start_row);
        song_audio();
        update_gains();
        seek_to(0, 1);
    }
    if (req_stop) {
        req_stop = 0;
        if (py_pat_audition_track() >= 0) {     /* an audition ends: stopped at bar 1 */
            py_mx.playing = 0;
            py_pat_audition(-1, 0);
            update_gains();
            seek_to(0, 0);
        } else if (py_mx.playing) {
            py_mx.playing = 0;                  /* first stop: pause where it is */
        } else {                                /* second stop: back to bar 1, song row 00 */
            py_pat_set_start(0);
            song_audio();                       /* (also: live cues are over) */
            seek_to(0, 0);
        }
    }
    if (req_play) {
        req_play = 0;
        if (py_mx.playing) {
            py_mx.playing = 0;
        } else {
            py_pat_start(py_mx.pos);            /* the song may have changed while stopped */
            py_mx.playing = 1;
        }
    }
    if (req_bar) {
        uint32_t bf = clock_bar(), bar = py_mx.pos / bf;
        int b = (int)bar + req_bar_step;

        req_bar = 0;
        if (py_mx.pos % bf > bf / 8 && req_bar_step < 0)
            b++;                                /* back: first to the start of this bar */
        seek_to((uint32_t)(b < 0 ? 0 : b) * bf, py_mx.playing);
    }
    audio_cues();
    py_fill(&py_mx, track_read, 0, PY_BUDGET, scratch);
}

/* ------------------------------------------------------------------ keys + knobs (KeyHandler) */

/* every key whose press PLAY took: its release is taken too, even after PLAY closed (a stray
 * release reaches TE's tape: rec ends a punch-in, T resets the solo gains, ...) */
static volatile uint32_t taken[2];

static int took(int key)
{
    return key >= 0 && key < 64 && (taken[key >> 5] >> (key & 31)) & 1;
}

static int take(int key)
{
    if (key >= 0 && key < 64)
        taken[key >> 5] |= 1u << (key & 31);
    return 1;
}

static volatile uint8_t join_due;               /* join on another screen: the tape key comes, then the song view */

/* play / stop on every other screen are PLAY's transport too (v0.13.1), except where TE needs
 * them: the album screen (album playback), shift held, rec held (rec + play still records TE's
 * tape), TE's tape running (its stop) and REQUANTIZE at work (it drives TE's tape with injected
 * play / stop presses). */
extern volatile int rq_state __attribute__((weak));     /* REQUANTIZE, when in the image */
static volatile uint8_t rec_held;

static int transport_anywhere(void)
{
    return *TE_EDIT_MODE != TE_EDIT_MODE_ALBUM && !*TE_SHIFT_HELD && !rec_held
        && *TE_TAPE_STATE == 0 && !(&rq_state && rq_state);
}

static int on_screen(void)
{
    return shown && *TE_EDIT_MODE == TE_EDIT_MODE_TAPE;
}

/* tape-mode keys PLAY keeps from TE while it is shown (RE of TE's tape controller 0x01911b14):
 * T1-T4 6-9, cut / insert / join 10-12 (edit clips), keys 1-8 16-20 + 45-47 (loop points,
 * reverse, one-beat loop, FX tricks), rec / play / stop / back / forward 33-37, knob pushes
 * 59-62. Mode keys, shift and the note keys still go to TE. */
static int tape_key(int key)
{
    return (key >= 6 && key <= 12) || (key >= 16 && key <= 20) || (key >= 45 && key <= 47)
        || (key >= 33 && key <= 37) || (key >= 59 && key <= 62);
}

/* the track under the cursor, -1 = the ADD row */
static int cursor_track(void)
{
    int list[PS_TRACKS], n = py_rows(list);

    return cursor < n ? list[cursor] : -1;
}

/* the song was edited: 0 = a setting or a cell, 1 = an AUDIO track's file / loop (reopen),
 * 2 = AUDIO song cells or song rows (the segments anew) */
void py_changed(int audio)
{
    py_pat_dirty();
    update_gains();
    if (audio == 1)
        req_tracks = 1;
    else if (audio)
        req_segs = 1;
}

#define changed py_changed

/* ------------------------------------------------------------------ for the views (py_view.c) */

void py_note(const char *msg)                   /* a short note in the footer (~2 s) */
{
    note_msg = msg;
    note_frames = 60;
}

const char *py_note_now(void)                   /* GUI: the note to draw this frame, 0 = none */
{
    if (!note_frames)
        return 0;
    note_frames--;
    return (const char *)note_msg;
}

void py_req_slot(int op, int n, uint32_t keep, int skeep)
{
    req_slot_n = (uint8_t)n;
    req_slot_keep = keep;
    req_slot_skeep = (uint8_t)skeep;
    req_slot = (uint8_t)op;
}

void py_req_pause(void) { req_play = 1; }
void py_req_stop(void) { req_stop = 1; }

void py_req_bar(int step)
{
    req_bar_step = (int8_t)step;
    req_bar = 1;
}

void py_req_start(int row, int aud_track, int chain)
{
    req_start_row = (uint8_t)row;
    req_start_aud = (int8_t)aud_track;
    req_start_chain = (uint8_t)chain;
    req_start = 1;
}

/* live: track t jumps to song row `row` when what it plays now ends (a chain; an AUDIO segment) */
void py_req_cue(int t, int row)
{
    if (py_song_is_pattern(&py_song, t)) {
        py_pat_cue(t, row);
    } else if (py_song.trk[t].type == PS_AUDIO) {
        req_cue_row[t] = (uint8_t)row;
        req_cue_on[t] = 1;
    }
}

/* the song row track t is cued to; -1 = none */
int py_cue_row(int t)
{
    if (py_song_is_pattern(&py_song, t))
        return py_pat_cued(t);
    if (py_song.trk[t].type != PS_AUDIO)
        return -1;
    return req_cue_on[t] ? req_cue_row[t] : py_set_cued(&set, t, py_mx.pos);
}

/* bars a render from song row `row` takes by default: the longest column played once (pattern
 * columns to their loop point, AUDIO columns their cells), at least one bar */
int py_song_bars(int row)
{
    static uint8_t seg[PS_ROWS];
    uint32_t max = py_pat_song_frames(row), bar = clock_bar();
    int k, i, n;

    for (k = 0; k < PY_TRACKS; k++) {
        uint32_t sum = 0;

        if (py_song.trk[k].type != PS_AUDIO || !set.t[k].used)
            continue;
        n = py_song_audio_segs(&py_song, k, row, seg, 0);
        for (i = 0; i < n; i++)
            sum += seg[i] ? set.t[k].frames : bar;
        if (sum > max)
            max = sum;
    }
    n = bar ? (int)((max + bar - 1) / bar) : 1;
    return n < 1 ? 1 : n > 999 ? 999 : n;
}

void py_master_step(int d)
{
    int v = master_db + d;

    master_db = (int8_t)(v < -60 ? -60 : v > 0 ? 0 : v);
    update_gains();
    {                                           /* (v0.17: no footer row any more) */
        static char msg[16] = "MASTER ";
        int i = 7, m = master_db < 0 ? -master_db : master_db;

        if (master_db < 0)
            msg[i++] = '-';
        if (m >= 10)
            msg[i++] = (char)('0' + m / 10);
        msg[i++] = (char)('0' + m % 10);
        msg[i++] = ' ';
        msg[i++] = 'D';
        msg[i++] = 'B';
        msg[i] = 0;
        py_note(msg);
    }
}

/* the tracks in list order with track `from` moved to list position `to`, written back to
 * indexes 0..n-1 (track settings and song columns move together); playing pauses meanwhile and
 * the main loop restarts the heads (req_tracks) */
static void move_track(int from, int to)
{
    static ps_track trk[PS_TRACKS];
    static uint8_t cell[PS_ROWS][PS_TRACKS];
    int list[PS_TRACKS], order[PS_TRACKS], n = py_rows(list), m = 0, i, r;

    for (i = 0; i < n; i++)
        if (list[i] != from) {
            if (m == to)
                order[m++] = from;
            order[m++] = list[i];
        }
    if (m == to)
        order[m++] = from;
    req_resume = py_mx.playing;
    py_mx.playing = 0;
    for (i = 0; i < PS_TRACKS; i++) {
        trk[i] = py_song.trk[i];
        for (r = 0; r < PS_ROWS; r++)
            cell[r][i] = py_song.cell[r][i];
    }
    for (i = 0; i < PS_TRACKS; i++) {
        int src = i < m ? order[i] : -1;

        if (src >= 0) {
            py_song.trk[i] = trk[src];
            for (r = 0; r < PS_ROWS; r++)
                py_song.cell[r][i] = cell[r][src];
        } else {
            py_song.trk[i].type = PS_NONE;
            for (r = 0; r < PS_ROWS; r++)
                py_song.cell[r][i] = PS_EMPTY;
        }
    }
}

static void step_pattern(int t, int d);

/* a new track of add_type at list position `pos` (-1 = at the end) */
static void add_track(int pos)
{
    int t;
    ps_track *k;

    if (!can_add(add_type))
        return;
    for (t = 0; t < PS_TRACKS && py_song.trk[t].type != PS_NONE; t++)
        ;
    k = &py_song.trk[t];
    k->type = add_type;
    k->sound = PS_EMPTY;
    k->loop = 1;
    k->mute = k->solo = 0;
    k->level = -9;
    k->voices = k->eco = 0;
    k->file[0] = 0;
    {
        int r;

        for (r = 0; r < PS_ROWS; r++)
            py_song.cell[r][t] = PS_EMPTY;      /* a fresh column */
    }
    if (add_type != PS_AUDIO) {                 /* TE's current sound / kit and pattern 1: it plays */
        k->sound = (uint8_t)((add_type == PS_SYNTH ? *TE_SYNTH_SLOT : *TE_DRUM_SLOT) & 7);
        step_pattern(t, 1);
    }
    if (add_type == PS_AUDIO && nfiles > 0) {   /* the first file not used yet, else the first */
        int f, u, pick = 0;

        for (f = 0; f < nfiles; f++) {
            for (u = 0; u < PS_TRACKS; u++)
                if (u != t && py_song.trk[u].type == PS_AUDIO && str_eq(py_song.trk[u].file, files[f].name))
                    break;
            if (u == PS_TRACKS) {
                pick = f;
                break;
            }
        }
        TE_SNPRINTF(k->file, sizeof(k->file), "%s", files[pick].name);
    }
    if (pos >= 0)
        move_track(t, pos);
    req_tracks = 1;
    changed(1);
}

static void remove_track(int t)
{
    py_song.trk[t].type = PS_NONE;
    changed(1);
}

/* the pattern of song row 00's chain, first row, of a SYNTH / DRUM track; -1 = none */
static int pattern_of(int t)
{
    int c = py_song.cell[0][t];

    return c < PS_CHAINS && py_song.chain[c][0].pat < PS_PATTERNS ? py_song.chain[c][0].pat : -1;
}

/* the pattern the list shows for the track: the one it plays now (playing or paused), else
 * song row 00's */
static int pattern_now(int t)
{
    const ps_head *h = py_pat_head(t);

    if ((py_mx.playing || py_mx.pos) && h->active && h->solo == PS_EMPTY && h->pat < PS_PATTERNS)
        return h->pat;
    return pattern_of(t);
}

/* the pattern the list row shows: a one-pattern track (orange turn) the one chosen, at once (the
 * head only takes it when the pattern playing ends: until then PY_PAT_PENDING, drawn amber; v0.9.1:
 * before, the row kept the old number while playing or paused); else what plays now */
int py_list_pattern(int t)
{
    int now = pattern_now(t), set;

    if (!py_song_quick(&py_song, t))
        return now;
    set = pattern_of(t);
    return set != now ? set | PY_PAT_PENDING : set;
}

/* orange turn: that pattern, --, 1..14 (the track gets a new unused chain the first time); only
 * while the track's song is that one pattern (py_song_quick), else the song view edits it */
static void step_pattern(int t, int d)
{
    ps_song *sg = &py_song;
    int v = pattern_of(t), c = sg->cell[0][t];

    if (!py_song_quick(sg, t))
        return;
    v += d;                                     /* 1..14, held at the ends: no "--" on the list (an */
    v = v < 0 ? 0 : v >= PS_PATTERNS ? PS_PATTERNS - 1 : v;    /* empty column stopped the track) */
    if (c >= PS_CHAINS) {
        c = py_song_new_chain(sg);
        if (c < 0)
            return;
    }
    sg->chain[c][0].pat = (uint8_t)v;
    sg->chain[c][1].pat = PS_EMPTY;             /* one row */
    sg->cell[0][t] = (uint8_t)c;
    changed(0);
}

/* keys 1-8 as a slot 0..7; -1 = another key */
int py_sound_key(int key)
{
    static const uint8_t keys[8] = { TE_KEY_SOUND1, TE_KEY_SOUND2, TE_KEY_SOUND3, TE_KEY_SOUND4,
                                     TE_KEY_SOUND5, TE_KEY_SOUND6, TE_KEY_SOUND7, TE_KEY_SOUND8 };
    int i;

    for (i = 0; i < 8; i++)
        if (keys[i] == key)
            return i;
    return -1;
}

/* SYNTH / DRUM track t on sound / kit slot 0..7, at once (it wins over the playing chain row's) */
void py_set_sound(int t, int slot)
{
    if (t < 0 || !py_song_is_pattern(&py_song, t))
        return;
    py_song.trk[t].sound = (uint8_t)slot;
    py_pat_sound_set(t);
    changed(0);
}

/* white turn: the file of an AUDIO track */
static void step_file(int t, int d)
{
    int i, cur = -1, n = nfiles;

    if (!n)
        return;
    for (i = 0; i < n; i++)
        if (str_eq(files[i].name, py_song.trk[t].file))
            cur = i;
    cur = cur < 0 ? 0 : (cur + d + n) % n;
    TE_SNPRINTF(py_song.trk[t].file, sizeof(py_song.trk[t].file), "%s", files[cur].name);
    changed(1);
}

/* orange turn: sound slot of a SYNTH / DRUM track: CUR (TE's current), 1..8 */
static void step_sound(int t, int d)
{
    int v = py_song.trk[t].sound >= 8 ? -1 : py_song.trk[t].sound;

    v += d;                                     /* 1..8, held at the ends (CUR of older songs: to 1) */
    v = v < 0 ? 0 : v > 7 ? 7 : v;
    py_set_sound(t, v);
}

/* DRUM ROLL / NOTE ROLL: their editors open over PLAY (shift + blue press on a track) */
extern void dr_open_from_play(int p), nr_open_from_play(int p);
extern volatile uint8_t dr_open, dr_via_play, nr_open, nr_via_play;

static int editor_up(void)                      /* an editor opened from PLAY is on screen */
{
    return (dr_open && dr_via_play) || (nr_open && nr_via_play);
}

/* shift + blue press on a SYNTH / DRUM track: its pattern in NOTE ROLL / DRUM ROLL, drawn over
 * PLAY on the tape screen (shift + blue press there closes it again: the editor's own key).
 * Runs before the other features' key handlers (REQUANTIZE's shift + blue press keeps working
 * on the other rows and screens). */
int py_key_first(int key, int down)
{
    int t, pat;

    if (key != TE_KEY_BLUE_PUSH)
        return 0;
    if (!down) {
        if (took(key)) {
            taken[key >> 5] &= ~(1u << (key & 31));
            return 1;
        }
        return 0;
    }
    if (!*TE_SHIFT_HELD || !on_screen() || editor_up() || py_help_on())
        return 0;
    if (py_view != PY_V_LIST) {                 /* song / chain view: the cursor's pattern */
        pat = py_v_pattern(&t);
        if (pat < 0)
            py_note("NO PATTERN HERE");
    } else {
        t = cursor_track();
        if (t < 0 || !py_song_is_pattern(&py_song, t))
            return 0;
        if (pattern_now(t) < 0)
            step_pattern(t, 1);                 /* no pattern yet: pattern 1 */
        pat = py_list_pattern(t);               /* the one the row shows */
        pat = pat < 0 ? pat : pat & ~PY_PAT_PENDING;
    }
    if (pat < 0)
        return take(key);
    if (py_song.trk[t].type == PS_SYNTH)
        nr_open_from_play(pat);
    else
        dr_open_from_play(pat);
    return take(key);
}

int py_key(int key, int down)
{
    int t;

    if (key == TE_KEY_REC)
        rec_held = (uint8_t)down;
    if (!down) {
        if (took(key)) {
            taken[key >> 5] &= ~(1u << (key & 31));
            return 1;
        }
        return 0;
    }
    if (key == TE_KEY_TAPE) {
        if (*TE_EDIT_MODE != TE_EDIT_MODE_TAPE) {
            shown = 1;                          /* TE switches to the tape screen: PLAY first */
            if (!py_rd_busy()) {                /* the list (join from another screen: the song view) */
                if (join_due)
                    py_v_enter();
                else
                    py_v_list();
            }
            join_due = 0;
            return 0;
        }
        /* PLAY is the tape screen of this image: no way to TE's own tape screen (v0.6; before,
         * tape again switched to it) nor to its erase screen (shift + tape). Tape here: the list;
         * on the list (v0.17): its layout, LANES <-> ROWS */
        if (!editor_up() && !py_rd_busy()) {
            if (py_view == PY_V_LIST && !py_help_on() && !*TE_SHIFT_HELD) {
                py_layout ^= 1;
                req_layout = 1;
            }
            py_v_list();
        }
        return take(key);
    }
    if (key == TE_KEY_JOIN && *TE_EDIT_MODE != TE_EDIT_MODE_TAPE && !*TE_SHIFT_HELD) {
        join_due = 1;                           /* another screen: to the tape screen, the song view */
        TE_KEY_PUSH((uint16_t)(TE_KEY_TAPE | 1 << 8));
        TE_KEY_PUSH((uint16_t)TE_KEY_TAPE);
        TE_KEY_WAKE();
        return take(key);
    }
    if (on_screen() && tape_key(key) && py_rd_busy()) {     /* a render: stop or rec cancels it */
        if (key == TE_KEY_STOP || key == TE_KEY_REC)
            py_rd_cancel();
        return take(key);
    }
    if (on_screen() && !editor_up() && (tape_key(key) || (key == TE_KEY_SPEECH && !*TE_SHIFT_HELD))
        && py_help_key(key))
        return take(key);                       /* speech: the help page; any key closes it */
    if (!on_screen() && (key == TE_KEY_PLAY || key == TE_KEY_STOP) && transport_anywhere()) {
        if (py_rd_busy()) {                     /* a render: stop cancels it */
            if (key == TE_KEY_STOP)
                py_rd_cancel();
        } else if (key == TE_KEY_PLAY) {
            req_play = 1;
        } else {
            req_stop = 1;
        }
        return take(key);
    }
    if (!on_screen() || !tape_key(key))
        return 0;
    if (editor_up()) {                          /* the editor did not want it: transport only */
        if (key == TE_KEY_PLAY)
            req_play = 1;
        else if (key == TE_KEY_STOP)
            req_stop = 1;
        return take(key);
    }
    if (py_view != PY_V_LIST) {
        py_v_key(key);
        return take(key);
    }
    t = cursor_track();
    if (key != TE_KEY_CUT)
        cut_armed = -1;
    if (key != TE_KEY_INSERT)
        insert_armed = 0;
    switch (key) {
    case TE_KEY_PLAY:
        req_play = 1;
        break;
    case TE_KEY_STOP:
        req_stop = 1;
        break;
    case TE_KEY_BACK:
    case TE_KEY_FORWARD:
        req_bar_step = key == TE_KEY_BACK ? -1 : 1;
        req_bar = 1;
        break;
    case TE_KEY_INSERT:                         /* twice: a new track here (before the cursor's) */
        if (!insert_armed) {
            insert_armed = 1;
        } else {
            insert_armed = 0;
            if (can_add(add_type))
                add_track(t >= 0 ? cursor : -1);
        }
        break;
    case TE_KEY_CUT:                            /* twice on a track: remove it */
        if (t >= 0) {
            if (cut_armed == t) {
                remove_track(t);
                cut_armed = -1;
            } else {
                cut_armed = (int8_t)t;
            }
        }
        break;
    case TE_KEY_T1: case TE_KEY_T2: case TE_KEY_T3: case TE_KEY_T4: {
        int list[PS_TRACKS], n = py_rows(list), r = key - TE_KEY_T1;

        if (r < n) {
            if (*TE_SHIFT_HELD) {
                py_song.trk[list[r]].mute ^= 1;
                changed(0);
            } else {
                cursor = (uint8_t)r;
            }
        }
        break;
    }
    case TE_KEY_BLUE_PUSH:                      /* mute */
        if (t >= 0) {
            py_song.trk[t].mute ^= 1;
            changed(0);
        }
        break;
    case TE_KEY_GREEN_PUSH:                     /* solo */
        if (t >= 0) {
            py_song.trk[t].solo ^= 1;
            changed(0);
        }
        break;
    case TE_KEY_WHITE_PUSH:     /* ADD row: add; AUDIO: loop / once; SYNTH: voices, shift: ECO */
        if (t < 0) {
            add_track(-1);
        } else if (py_song.trk[t].type == PS_AUDIO) {
            if (!py_song_col_used(&py_song, t)) {   /* with song cells: the song decides */
                py_song.trk[t].loop ^= 1;
                changed(1);
            }
        } else if (py_song.trk[t].type == PS_SYNTH) {
            ps_track *tr = &py_song.trk[t];

            if (*TE_SHIFT_HELD) {
                tr->eco ^= 1;
            } else {
                int v = tr->voices ? tr->voices : PY_SYN_CAP_DEFAULT;

                tr->voices = (uint8_t)(v == 1 ? 2 : v == 2 ? 4 : 1);   /* 2 -> 4 -> 1 -> 2 */
            }
            changed(0);
        }
        break;
    case TE_KEY_ORANGE_PUSH:
        req_load = 1;                           /* re-read the set folder (stops first) */
        break;
    case TE_KEY_JOIN:                           /* the song view; shift: the songs page */
        if (*TE_SHIFT_HELD)
            py_v_slots();
        else
            py_v_enter();
        break;
    default:                                    /* keys 1-8: the track's sound / kit; rec: nothing */
        if (py_sound_key(key) >= 0)
            py_set_sound(t, py_sound_key(key));
        break;
    }
    return take(key);
}

int py_knob(int knob, int16_t *diff)
{
    int d, t, list[PS_TRACKS], n;

    if (!on_screen())
        return 0;
    if (editor_up())
        return 1;                               /* the editor did not want it: never TE's tape */
    d = *diff > 0 ? 1 : *diff < 0 ? -1 : 0;
    if (py_rd_busy())
        return 1;                               /* a render: nothing changes meanwhile */
    if (py_help_on()) {
        py_help_knob(knob, d);
        return 1;
    }
    if (py_view != PY_V_LIST) {
        py_v_knob(knob, d);
        return 1;
    }
    n = py_rows(list);
    t = cursor < n ? list[cursor] : -1;
    cut_armed = -1;
    if (insert_armed && knob == 2) {            /* armed insert: white turn = the new track's type */
        int v = add_type + d;

        add_type = (uint8_t)(v < PS_AUDIO ? PS_DRUM : v > PS_DRUM ? PS_AUDIO : v);
        return 1;
    }
    insert_armed = 0;
    switch (knob) {
    case 0:                                     /* blue: cursor (tracks, then ADD if there is room) */
    {
        int last = count(PS_NONE) ? n : n - 1;
        int c = cursor + d;

        cursor = (uint8_t)(c < 0 ? 0 : c > last ? last : c);
        break;
    }
    case 1:                                     /* green: level */
        if (t >= 0) {
            int v = py_song.trk[t].level + d;

            py_song.trk[t].level = (int8_t)(v < -60 ? -60 : v > 0 ? 0 : v);
            changed(0);
        }
        break;
    case 2:                                     /* white: the sound / kit or the file; ADD row: the type */
        if (t < 0) {
            int v = add_type + d;

            add_type = (uint8_t)(v < PS_AUDIO ? PS_DRUM : v > PS_DRUM ? PS_AUDIO : v);
        } else if (py_song.trk[t].type == PS_AUDIO) {
            step_file(t, d);
        } else {
            step_sound(t, d);
        }
        break;
    default:                                    /* orange: the pattern; shift: master */
        if (*TE_SHIFT_HELD) {
            py_master_step(d);
        } else if (t >= 0 && py_song.trk[t].type != PS_AUDIO) {
            step_pattern(t, d);
        }
        break;
    }
    return 1;
}

/* ------------------------------------------------------------------ screen (GUI thread) */

/* the header every view shares (py_draw.c): tempo, bar.beat, transport; h->ctx etc. for the caller */
void py_hdr_fill(py_hm *h, const char *title, int num, uint16_t col)
{
    uint32_t bf = clock_bar(), bar = 0, beat = 0;

    if (bf) {
        bar = py_mx.pos / bf;
        beat = (py_mx.pos % bf) / (bf / 4);
    }
    h->title = title;
    h->title_col = col;
    h->num = num;
    h->ctx = py_loading ? "LOADING" : 0;
    h->bpm = (int)((clock_bpm() + 50) / 100);
    h->bar = (int)bar + 1;
    h->beat = (int)beat + 1;
    h->row = -1;
    h->loop_a = h->loop_b = h->loop_n = 0;
    h->state = (uint8_t)(py_rd_busy() ? 2 : py_mx.playing ? 1 : 0);
    h->bare = 0;
    h->nobar = 0;
}

/* "CPU 31" while playing (PLAY's share of the audio interrupt's time) after `pre` */
const char *py_cpu_ctx(const char *pre)
{
    static char t[24];
    int i = 0, v;

    for (; *pre; pre++)
        t[i++] = *pre;
    if (py_mx.playing) {
        if (i) {
            t[i++] = ' ';
            t[i++] = '\1';
            t[i++] = ' ';
        }
        t[i++] = 'C';
        t[i++] = 'P';
        t[i++] = 'U';
        t[i++] = ' ';
        v = (py_cpu_all + 5) / 10;
        if (v >= 100)
            t[i++] = (char)('0' + v / 100 % 10);
        if (v >= 10)
            t[i++] = (char)('0' + v / 10 % 10);
        t[i++] = (char)('0' + v % 10);
    }
    t[i] = 0;
    return t;
}

/* the header of the other views (chain, songs, files): the title (num >= 0: two hex digits after it) */
void py_draw_header(const char *title, int num, uint16_t col)
{
    py_hm h;

    py_hdr_fill(&h, title, num, col);
    if (!h.ctx)
        h.ctx = py_cpu_ctx("");
    py_d_header(&h);
}

/* an engine's name: an SDK engine by its tag, TE's by its type */
static const char *engine_of(int slot)
{
    static const char *const sdk[] = { "DR WAVE", "PARTY", "MONO 3", "STATION 1", "TRIO", "WHEEL", "WARP",
                                       "MALLET", "ACID 3", "PALETTE" };
    static const char *const te[] = { "FM", "DIGITAL", "DR WAVE", "STRING", "CLUSTER", "PULSE", "SAMPLER",
                                      "PHASE", "DRUM", "DBOX", "LS", "DNA", "DSYNTH", "VOLTAGE", "ITER" };
    int type = TE_SYNTH_TYPE[slot];

    if (type == TE_INSTR_DRWAVE) {
        int tag = eh_tag(TE_SYNTH_KNOBS + slot * 8);

        return tag >= 0 && tag < (int)(sizeof(sdk) / sizeof(sdk[0])) ? sdk[tag] : "SDK ENGINE";
    }
    return type >= 0 && type < (int)(sizeof(te) / sizeof(te[0])) ? te[type] : "ENGINE";
}

/* the list as py_draw.c shows it: one track */
static void list_track(py_lt *o, int k)
{
    const ps_track *t = &py_song.trk[k];

    o->k = (uint8_t)k;
    o->type = t->type;
    o->mute = t->mute;
    o->solo = t->solo;
    o->level = t->level;
    o->cpu = py_mx.playing && !t->mute && t->type != PS_AUDIO ? py_cpu_trk[k] : 0xffff;
    o->err = 0;
    o->name = 0;
    o->voices = o->eco = o->busy = o->under = o->pend = o->slot_alt = 0;
    o->seq = 0;
    o->seq_song = 0;
    o->slot = 0;
    o->amode = 0;
    if (t->type == PS_AUDIO) {
        o->name = t->file[0] ? t->file : "NO FILE";
        o->amode = (uint8_t)(py_song_col_used(&py_song, k) ? 2 : t->loop ? 1 : 0);
        if (!set.t[k].used && !py_loading)
            o->err = set.t[k].err;
        o->under = py_mx.underruns[k] != 0;
        return;
    }
    {
        int pat = py_list_pattern(k), slot = py_pat_slot(k);

        o->pend = (uint8_t)(pat >= 0 && (pat & PY_PAT_PENDING));
        pat = pat < 0 ? pat : pat & ~PY_PAT_PENDING;
        o->seq = (int8_t)(pat >= 0 && pat < PS_PATTERNS ? pat + 1 : 0);
        o->seq_song = (uint8_t)!py_song_quick(&py_song, k);
        o->slot = (int8_t)(slot + 1);
        o->slot_alt = (uint8_t)(py_mx.playing && slot != t->sound && t->sound < 8);
        if (t->type == PS_DRUM) {
            o->name = "DRUM";
            return;
        }
        o->name = engine_of(slot);
        if (TE_SYNTH_TYPE[slot] == TE_INSTR_DRWAVE ? !eh_multi_ok(TE_SYNTH_KNOBS + slot * 8)
                                                  : py_tev_why(slot) != 0) {
            int why = TE_SYNTH_TYPE[slot] == TE_INSTR_DRWAVE ? 0 : py_tev_why(slot);

            o->err = why == PY_TEV_NO_SAMPLE ? "NO SAMPLE" : why ? "NOT IN PLAY"
                   : eh_tag(TE_SYNTH_KNOBS + slot * 8) ? "NOT IN PLAY" : "NOT AN ENGINE";
        }
        o->busy = (uint8_t)(py_pat_starved(k) != 0);
        o->voices = t->voices ? t->voices : PY_SYN_CAP_DEFAULT;
        o->eco = t->eco;
    }
}

static void list_frame(void)
{
    static py_lm m;
    int list[PS_TRACKS], n, i;
    const char *note;
    py_hm h;

    n = py_rows(list);
    m.layout = py_layout;
    m.n = (uint8_t)n;
    m.cursor = cursor;
    m.add_room = count(PS_NONE) != 0;
    m.add_type = add_type;
    m.add_ok = (uint8_t)can_add(add_type);
    m.insert = insert_armed;
    m.master_db = master_db;
    m.cut = -1;
    for (i = 0; i < n; i++) {
        list_track(&m.t[i], list[i]);
        if (cut_armed == list[i])
            m.cut = (int8_t)i;
    }
    m.note = 0;
    m.note2 = 0;
    note = py_note_now();
    if (note) {
        m.note = note;
        m.note_col = D_AMBER;
    } else if (cut_armed >= 0) {
        m.note = "CUT AGAIN TO REMOVE THE TRACK";
        m.note_col = D_RED;
    } else if (insert_armed) {
        m.note = can_add(add_type) ? "INSERT AGAIN: ADD" : "CANNOT ADD";
        m.note_col = D_GREEN;
        m.note2 = add_type == PS_AUDIO ? "AUDIO" : add_type == PS_SYNTH ? "SYNTH" : "DRUM";
    } else if (cursor >= n && !m.add_ok) {
        m.note = "MAX 2 DRUM TRACKS";
        m.note_col = D_MUTED;
    } else if (cursor >= n && add_type == PS_AUDIO && !nfiles) {
        m.note = "NO FILES: PUT WAV / AIF IN PLAY/";
        m.note_col = D_MUTED;
    }
    py_hdr_fill(&h, "PLAY", -1, C_WHITE);
    h.nobar = 1;                                /* v0.17: no BAR on the list */
    if (!h.ctx) {
        static char trk[8];

        trk[0] = (char)('0' + n);
        trk[1] = ' ';
        trk[2] = 'T';
        trk[3] = 'R';
        trk[4] = 'K';
        trk[5] = 0;
        h.ctx = py_cpu_ctx(trk);
    }
    py_d_header(&h);
    py_d_list(&m);
}

/* a render runs: the footer of whatever view is shown says so */
static void render_footer(void)
{
    int x, pc = py_rd_percent();

    if (py_view == PY_V_LIST || py_view == PY_V_SONG) {     /* the views with the one-line footer */
        py_d_footer_render(py_rd_name(), pc);
        return;
    }
    gfx_rect(0, PY_FOOT_LINE + 1, gfx_w, gfx_h - PY_FOOT_LINE - 1, GFX_BLACK);
    x = gfx_text7(4, PY_FOOT_Y1, "RENDERING ", C_RED);
    x = gfx_text7(x, PY_FOOT_Y1, py_rd_name(), C_WHITE);
    x = gfx_dec7(x + 12, PY_FOOT_Y1, pc, C_WHITE);
    gfx_text7(x, PY_FOOT_Y1, "%", C_WHITE);
    gfx_rect(4, PY_FOOT_Y2 + 1, 200, 5, C_LINE);
    gfx_rect(4, PY_FOOT_Y2 + 1, pc > 100 ? 200 : pc * 2, 5, C_RED);
    gfx_text7(226, PY_FOOT_Y2, "STOP: CANCEL", C_GREY);
}

void py_frame(uint16_t *buf)
{
    static uint32_t last_blocks;
    uint32_t blocks = py_blocks - last_blocks;
    int t;

    if (!on_screen() || editor_up())
        return;                                 /* an editor opened from PLAY draws instead */
    last_blocks += blocks;
    if (!py_mx.playing && !py_rd_busy())
        blocks = blocks ? blocks : 20;          /* (stopped, the hook may not run: still fall) */
    for (t = 0; t < PS_TRACKS; t++) {           /* the meters: this frame's peaks, the time since */
        uint32_t pk = py_mx.peak[t];

        py_mx.peak[t] = 0;
        py_d_meter_feed(t, pk, blocks);
    }
    gfx_begin(buf);
    eh_aa_begin(buf);
    eh_aa_dy = 0;
    eh_aa_gamma = 1;                            /* (the engine screens draw without) */
    gfx_rect(0, 0, gfx_w, gfx_h, GFX_BLACK);
    if (py_help_on())
        py_help_frame();
    else if (py_view != PY_V_LIST)
        py_v_frame();
    else
        list_frame();
    if (py_rd_busy() && !py_help_on())
        render_footer();
    eh_aa_gamma = 0;
}
