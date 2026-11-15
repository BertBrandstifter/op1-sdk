/* play: the PLAY tape screen (README.md, new_pattern_tape.md). TE-facing part; the logic is in
 * py_core.c, py_song.c, py_drum.c, py_synth.c (host-tested) and py_pattern.c.
 *
 *   tracks     one list of up to 8 tracks, each AUDIO (a WAV / AIF of the set folder, looped or
 *              once), SYNTH (NOTE ROLL patterns on its own engine instance and a sound slot) or
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
 *   screen     the tape key opens PLAY (TE's tape: tape again); PLAY is drawn over TE's frame and
 *              takes every tape-mode key that would move, record or edit TE's tape; playback keeps
 *              running on other screens.
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

#define RING_FRAMES     16384                   /* 372 ms */
#define CHUNK_FRAMES    4096                    /* 16 KB stereo per read (~2.3 ms on the device) */
#define PY_BUDGET       (48u << 10)             /* bytes read per main-loop pass at most */
#define PREFILL         (2 * CHUNK_FRAMES)      /* frames per track read before play starts */
#define MAXFILES        16
#define MAX_SYNTH       3                       /* engine instances */
#define MAX_DRUM        2                       /* CPU */
#define TE_BUS_L        ((int16_t *)0xff800d34)  /* int16[64]: TE's master bus (before EQ / FX) */
#define TE_BUS_R        ((int16_t *)0xff800db4)
#define BUS_SCALE       12409                   /* Q15 0.38: TE's drive takes the bus ~x2.64 up */

/* ------------------------------------------------------------------ state */

static py_set set;                              /* main thread writes, others read */
static py_setfile setfile;
static py_file files[MAXFILES];
static volatile int nfiles;
static int fds[PY_TRACKS] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static uint8_t *scratch;
py_mixer py_mx;                                 /* bench reads it */
volatile uint32_t py_blocks;                    /* volume-hook calls (bench, debug) */

/* user state (KeyHandler writes, main / GUI read) */
static volatile uint8_t shown = 1;              /* PLAY instead of TE's tape screen */
static volatile uint8_t cursor;                 /* row: a track (in list order) or the ADD row */
static volatile uint8_t add_type = PS_SYNTH;    /* what the ADD row adds */
static volatile int8_t cut_armed = -1;          /* track whose removal waits for a second cut */
static volatile uint8_t insert_armed;           /* insert pressed once: the second one inserts */
static volatile int8_t master_db;
static volatile const char *note_msg;           /* a short note in the footer (GUI counts it down) */
static volatile uint16_t note_frames;

/* requests to the main thread */
static volatile uint8_t req_load = 1, req_tracks, req_resume, req_play, req_stop, req_bar;
static volatile int8_t req_bar_step;

volatile uint8_t py_loading;                    /* GUI: "LOADING" */

/* ------------------------------------------------------------------ the track list */

/* rows: the tracks in index order, then the ADD row (if there is room) */
static int rows(int *list)
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
    return type == PS_SYNTH ? count(PS_SYNTH) < MAX_SYNTH : type == PS_DRUM ? count(PS_DRUM) < MAX_DRUM : 1;
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

/* every AUDIO track: its file assigned, opened, a ring allocated */
static void open_audio(void)
{
    char path[64];
    int k;

    close_audio();
    py_set_init(&set, &setfile, te_bpm_x100());
    if (!scratch)
        scratch = TE_HEAP_MALLOC(TE_HEAP_SAMPLES, CHUNK_FRAMES * 4);
    for (k = 0; k < PY_TRACKS; k++) {
        const ps_track *t = &py_song.trk[k];
        py_ring *r = &py_mx.ring[k];

        if (t->type != PS_AUDIO || !py_set_assign(&set, k, files, nfiles, t->file, t->loop))
            continue;
        path_of(path, files[(int)set.t[k].file].name);
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
    py_mx.set = &set;
    py_mx.chunk = CHUNK_FRAMES;
}

/* read the set folder: the audio files' headers, SET.TXT, the song; then the tracks */
static void load_set(void)
{
    static uint32_t st[32];                     /* TE's stat buffer is 0x48 bytes */
    static char txt[4096] __attribute__((aligned(4)));   /* yaffs: even addresses */
    char path[64];
    void *dir;
    uint8_t *de;
    int n = 0;

    py_loading = 1;
    close_audio();
    set_dir = py_dir();
    py_pat_load(set_dir);
    py_setfile_parse("", 0, &setfile);
    dir = PY_OPENDIR(set_dir);
    while (dir && (de = PY_READDIR(dir)) != 0) {
        const char *name = (const char *)de + 11;
        py_file *f = &files[n];
        int fd, i;

        if (name[0] == '.')
            continue;
        path_of(path, name);
        if (PY_STAT(path, st) < 0 || !(st[0x08 / 4] & 0x8000))
            continue;                           /* not a regular file */
        if (str_eq(name, "SET.TXT")) {
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
        for (i = 0; name[i] && i < 12; i++)
            f->name[i] = name[i];
        f->name[i] = 0;
        f->size = st[0x1c / 4];
        fd = TE_FILE_OPEN(path, TE_FILE_RDONLY, TE_FILE_MODE);
        if (fd < 0)
            continue;
        aud_parse(yaffs_read, &fd, f->size, &f->a);
        TE_FILE_CLOSE(fd);
        n++;
    }
    if (dir)
        PY_CLOSEDIR(dir);
    nfiles = n;
    open_audio();
    req_play = req_stop = req_bar = 0;          /* transport requests belong to the old set */
    py_seek(&py_mx, 0);
    py_fill(&py_mx, track_read, 0, PREFILL * 4u * PY_TRACKS, scratch);
    py_loading = 0;
}

static uint32_t clock_bpm(void) { return set.bpm_x100 ? set.bpm_x100 : te_bpm_x100(); }
static uint32_t clock_bar(void) { return set.bar_frames ? set.bar_frames : py_bar_frames(te_bpm_x100()); }

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
    int k, solo = 0;

    for (k = 0; k < PY_TRACKS; k++)
        solo |= py_song.trk[k].type != PS_NONE && py_song.trk[k].solo;
    for (k = 0; k < PY_TRACKS; k++) {
        const ps_track *t = &py_song.trk[k];
        int on = t->type != PS_NONE && (solo ? t->solo : !t->mute);

        py_mx.gain[k] = on ? py_db_q15(t->level * 10) : 0;
    }
    py_mx.master = py_db_q15(master_db * 10);
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

void py_main(void)
{
    if (py_import_pending()) {                  /* disk mode left with a play/ folder */
        close_audio();
        py_import_run();
        req_load = 1;
    }
    if (req_load) {
        req_load = 0;
        req_tracks = 0;
        py_mx.te_volume = 32767;                /* TE applies its volume after PLAY's mix */
        load_set();
        update_gains();
    }
    if (req_tracks) {                           /* a track added, removed or changed */
        int playing = py_mx.playing || req_resume;

        req_tracks = 0;
        req_resume = 0;
        py_pat_tracks_changed();
        open_audio();
        update_gains();
        seek_to(py_mx.pos, playing);
    }
    py_pat_main(clock_bar(), clock_bpm());
    if (req_stop) {
        req_stop = 0;
        if (py_mx.playing)
            py_mx.playing = 0;                  /* first stop: pause where it is */
        else
            seek_to(0, 0);                      /* second stop: back to bar 1 */
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
    int list[PS_TRACKS], n = rows(list);

    return cursor < n ? list[cursor] : -1;
}

static void changed(int audio)                  /* a track setting changed */
{
    py_pat_dirty();
    update_gains();
    if (audio)
        req_tracks = 1;
}

/* the tracks in list order with track `from` moved to list position `to`, written back to
 * indexes 0..n-1 (track settings and song columns move together); playing pauses meanwhile and
 * the main loop restarts the heads (req_tracks) */
static void move_track(int from, int to)
{
    static ps_track trk[PS_TRACKS];
    static uint8_t cell[PS_ROWS][PS_TRACKS];
    int list[PS_TRACKS], order[PS_TRACKS], n = rows(list), m = 0, i, r;

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
    k->file[0] = 0;
    {
        int r;

        for (r = 0; r < PS_ROWS; r++)
            py_song.cell[r][t] = PS_EMPTY;      /* a fresh column */
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

/* the pattern a SYNTH / DRUM track plays (until the song view: song row 00's chain, its first
 * row); -1 = none */
static int pattern_of(int t)
{
    int c = py_song.cell[0][t];

    return c < PS_CHAINS && py_song.chain[c][0].pat < PS_PATTERNS ? py_song.chain[c][0].pat : -1;
}

/* white turn: that pattern, --, 1..14 (the track gets a new unused chain the first time) */
static void step_pattern(int t, int d)
{
    ps_song *sg = &py_song;
    int v = pattern_of(t), c = sg->cell[0][t];

    v += d;
    v = v < -1 ? -1 : v >= PS_PATTERNS ? PS_PATTERNS - 1 : v;
    if (c >= PS_CHAINS) {
        if (v < 0)
            return;
        c = py_song_new_chain(sg);
        if (c < 0)
            return;
    }
    if (v < 0) {
        sg->cell[0][t] = PS_EMPTY;
        sg->chain[c][0].pat = PS_EMPTY;         /* empty + unused: free again */
    } else {
        sg->chain[c][0].pat = (uint8_t)v;
        sg->chain[c][1].pat = PS_EMPTY;         /* one row */
        sg->cell[0][t] = (uint8_t)c;
    }
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

    v += d;
    v = v < -1 ? -1 : v > 7 ? 7 : v;
    py_song.trk[t].sound = (uint8_t)(v < 0 ? PS_EMPTY : v);
    changed(0);
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
    if (!*TE_SHIFT_HELD || !on_screen() || editor_up())
        return 0;
    t = cursor_track();
    if (t < 0 || !py_song_is_pattern(&py_song, t))
        return 0;
    if (pattern_of(t) < 0)
        step_pattern(t, 1);                     /* no pattern yet: pattern 1 */
    pat = pattern_of(t);
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
            return 0;
        }
        if (*TE_SHIFT_HELD)
            return shown ? take(key) : 0;       /* TE's erase screen only from TE's own tape */
        shown = (uint8_t)!shown;                /* tape again: PLAY <-> TE's tape (TE ignores it) */
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
        int list[PS_TRACKS], n = rows(list), r = key - TE_KEY_T1;

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
            py_song.trk[t].loop ^= 1;
            changed(1);
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
    default:                                    /* rec, insert / join, keys 1-8: nothing */
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
    n = rows(list);
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
    case 2:                                     /* white: what it plays; ADD row: the type */
        if (t < 0) {
            int v = add_type + d;

            add_type = (uint8_t)(v < PS_AUDIO ? PS_DRUM : v > PS_DRUM ? PS_AUDIO : v);
        } else if (py_song.trk[t].type == PS_AUDIO) {
            step_file(t, d);
        } else {
            step_pattern(t, d);
        }
        break;
    default:                                    /* orange: sound slot; shift: master */
        if (*TE_SHIFT_HELD) {
            int v = master_db + d;

            master_db = (int8_t)(v < -60 ? -60 : v > 0 ? 0 : v);
            update_gains();
        } else if (t >= 0 && py_song.trk[t].type != PS_AUDIO) {
            step_sound(t, d);
        }
        break;
    }
    return 1;
}

/* ------------------------------------------------------------------ screen (GUI thread) */

#define C_GREY      GFX_RGB(0x8a, 0x8a, 0x99)
#define C_DIM       GFX_RGB(0x40, 0x40, 0x48)
#define C_LINE      GFX_RGB(0x30, 0x30, 0x38)
#define C_SEL       GFX_RGB(0x1a, 0x1a, 0x22)
#define C_AMBER     GFX_RGB(0xff, 0xb0, 0x3d)
#define C_GREEN     GFX_RGB(0x3d, 0xdc, 0x97)
#define C_BLUE      GFX_RGB(0x6e, 0x8b, 0xff)
#define C_RED       GFX_RGB(0xff, 0x4f, 0x5e)
#define C_WHITE     GFX_WHITE

#define ROW_Y(r)    (28 + (r) * 11)

static uint16_t meter_hold[PY_TRACKS], meter_level[PY_TRACKS];

static int two(int x, int y, uint32_t v, uint16_t c)       /* "07" */
{
    if (v < 10)
        x = gfx_text(x, y, "0", c, 1);
    return gfx_dec(x, y, (int32_t)v, c, 1);
}

static void draw_header(int y)
{
    uint32_t bar = 0, beat = 0, bf = clock_bar();
    int x;

    gfx_text(4, y, "PLAY", C_WHITE, 2);
    if (py_loading)
        gfx_text(48, y + 2, "LOADING", C_GREY, 1);
    x = gfx_text(110, y + 2, "BPM ", C_GREY, 1);
    gfx_dec(x, y + 2, (int32_t)(clock_bpm() + 50) / 100, C_WHITE, 1);
    if (bf) {
        bar = py_mx.pos / bf;
        beat = (py_mx.pos % bf) / (bf / 4);
    }
    x = gfx_text(196, y + 2, "BAR ", C_GREY, 1);
    x = two(x, y + 2, bar + 1, C_WHITE);
    x = gfx_text(x, y + 2, ".", C_WHITE, 1);
    gfx_dec(x, y + 2, (int32_t)beat + 1, C_WHITE, 1);
    if (py_mx.playing) {                        /* PLAY's share of the audio interrupt's time */
        x = gfx_text(150, y + 2, "CPU ", C_GREY, 1);
        gfx_dec(x, y + 2, (py_cpu_all + 5) / 10, py_cpu_all > 700 ? C_RED : C_WHITE, 1);
    }
    gfx_text(280, y + 2, py_mx.playing ? "PLAY" : "STOP", py_mx.playing ? C_GREEN : C_GREY, 1);
}

/* meter (peak since the last frame, held and falling), level, mute, solo of track k */
static void draw_mix(int y, int k, uint16_t col)
{
    const ps_track *t = &py_song.trk[k];
    uint32_t pk = py_mx.peak[k];
    int x, w;

    py_mx.peak[k] = 0;
    if (pk > 32767)
        pk = 32767;
    meter_level[k] = (uint16_t)(pk > meter_level[k] ? pk : meter_level[k] * 7 / 8);
    meter_hold[k] = (uint16_t)(pk > meter_hold[k] ? pk : meter_hold[k] > 200u ? meter_hold[k] - 200u : 0u);
    w = (int)(meter_level[k] * 64u / 32768u);
    gfx_rect(160, y, 64, 5, C_LINE);
    gfx_rect(160, y, w, 5, t->mute ? C_DIM : col);
    gfx_rect(160 + (int)(meter_hold[k] * 63u / 32768u), y, 1, 5, C_WHITE);
    x = gfx_dec(232, y, t->level, C_WHITE, 1);
    gfx_text(x, y, " DB", C_GREY, 1);
    gfx_text(280, y, "M", t->mute ? C_RED : C_DIM, 1);
    gfx_text(292, y, "S", t->solo ? C_AMBER : C_DIM, 1);
}

static void draw_track(int y, int row, int k, int sel)
{
    const ps_track *t = &py_song.trk[k];
    int x;
    uint16_t col = t->type == PS_AUDIO ? C_AMBER : t->type == PS_SYNTH ? C_BLUE : C_RED;

    if (sel)
        gfx_rect(0, y - 3, gfx_w, 11, cut_armed == k ? GFX_RGB(0x40, 0x10, 0x14)
                 : insert_armed ? GFX_RGB(0x10, 0x30, 0x22) : C_SEL);
    gfx_dec(4, y, row + 1, C_WHITE, 1);
    gfx_text(16, y, t->type == PS_AUDIO ? "AUD" : t->type == PS_SYNTH ? "SYN" : "DRM", col, 1);
    if (t->type == PS_AUDIO) {
        gfx_text(36, y, t->file[0] ? t->file : "NO FILE", C_WHITE, 1);
        gfx_text(92, y, t->loop ? "LOOP" : "ONCE", C_GREY, 1);
        if (!set.t[k].used && !py_loading) {
            gfx_text(116, y, set.t[k].err, C_RED, 1);
            return;
        }
        if (py_mx.underruns[k])
            gfx_text(150, y, "!", C_RED, 1);
    } else {
        int pat = pattern_of(k);
        const ps_head *h = py_pat_head(k);

        x = gfx_text(36, y, "PAT ", C_GREY, 1);
        if (pat >= 0 && pat < PS_PATTERNS)
            gfx_dec(x, y, pat + 1, C_WHITE, 1);
        else
            gfx_text(x, y, "--", C_DIM, 1);
        x = gfx_text(64, y, t->type == PS_DRUM ? "KIT " : "SND ", C_GREY, 1);
        if (t->sound < 8)
            gfx_dec(x, y, t->sound + 1, C_WHITE, 1);
        else
            gfx_text(x, y, "CUR", C_WHITE, 1);
        if (t->type == PS_SYNTH) {
            int slot = t->sound < 8 ? t->sound : (int)(*TE_SYNTH_SLOT & 7);

            if (!py_pat_inst(k)) {
                gfx_text(96, y, "NO ENGINE SLOT", C_RED, 1);
                return;
            }
            if (!eh_multi_ok(TE_SYNTH_KNOBS + slot * 8)) {
                gfx_text(96, y, eh_tag(TE_SYNTH_KNOBS + slot * 8) ? "ENGINE NOT IN PLAY YET" : "NOT AN SDK ENGINE",
                         C_RED, 1);
                return;
            }
        }
        if (t->type == PS_SYNTH) {
            x = gfx_text(96, y, "V", C_GREY, 1);
            gfx_dec(x, y, t->voices ? t->voices : PY_SYN_CAP_DEFAULT, C_WHITE, 1);
            if (t->eco)
                gfx_text(108, y, "ECO", C_GREEN, 1);
        }
        if (py_mx.playing && !t->mute)          /* the track's share (percent) */
            gfx_dec(128, y, (py_cpu_trk[k] + 5) / 10, py_cpu_trk[k] > 300 ? C_RED : C_GREY, 1);
        if (h->active && py_mx.playing)
            gfx_rect(150, y + 1, 3, 3, col);   /* playing */
    }
    draw_mix(y, k, col);
}

void py_frame(uint16_t *buf)
{
    int list[PS_TRACKS], n, i, x, t;

    if (!on_screen() || editor_up())
        return;                                 /* an editor opened from PLAY draws instead */
    gfx_begin(buf);
    gfx_rect(0, 0, gfx_w, gfx_h, GFX_BLACK);
    draw_header(4);
    gfx_rect(0, 22, gfx_w, 1, C_LINE);
    n = rows(list);
    for (i = 0; i < n; i++)
        draw_track(ROW_Y(i), i, list[i], i == cursor);
    if (count(PS_NONE)) {                       /* the ADD row */
        int y = ROW_Y(n), ok = can_add(add_type);

        if (cursor == n)
            gfx_rect(0, y - 3, gfx_w, 11, C_SEL);
        x = gfx_text(4, y, "+ ADD ", C_GREY, 1);
        x = gfx_text(x, y, add_type == PS_AUDIO ? "AUDIO" : add_type == PS_SYNTH ? "SYNTH" : "DRUM",
                     ok ? (add_type == PS_AUDIO ? C_AMBER : add_type == PS_SYNTH ? C_BLUE : C_RED) : C_DIM, 1);
        if (!ok)
            gfx_text(x + 8, y, add_type == PS_SYNTH ? "(MAX 3 SYNTH)" : "(MAX 2 DRUM)", C_DIM, 1);
        else if (add_type == PS_AUDIO && !nfiles)
            gfx_text(x + 8, y, "(NO FILES: PUT WAV / AIF IN PLAY/ ON THE DISK)", C_DIM, 1);
    }
    gfx_rect(0, 134, gfx_w, 1, C_LINE);
    x = gfx_text(4, 140, "MASTER ", C_GREY, 1);
    x = gfx_dec(x, 140, master_db, C_WHITE, 1);
    gfx_text(x, 140, " DB", C_GREY, 1);
    t = cursor < n ? list[cursor] : -1;
    if (note_frames) {
        note_frames--;
        gfx_text(120, 140, (const char *)note_msg, C_AMBER, 1);
    } else if (cut_armed >= 0)
        gfx_text(120, 140, "CUT AGAIN TO REMOVE THE TRACK", C_RED, 1);
    else if (insert_armed) {
        x = gfx_text(120, 140, can_add(add_type) ? "INSERT AGAIN: ADD " : "CANNOT ADD ", C_GREEN, 1);
        gfx_text(x, 140, add_type == PS_AUDIO ? "AUDIO" : add_type == PS_SYNTH ? "SYNTH" : "DRUM", C_WHITE, 1);
    }
    else if (t < 0)
        gfx_text(120, 140, "WHITE: TYPE   WHITE PRESS: ADD", C_GREY, 1);
    else if (py_song.trk[t].type == PS_AUDIO)
        gfx_text(120, 140, "WHITE: FILE   PRESS: LOOP / ONCE", C_GREY, 1);
    else if (py_song.trk[t].type == PS_SYNTH)
        gfx_text(120, 140, "PRESS: VOICES  SHIFT: ECO  SHIFT+BLUE: EDIT", C_GREY, 1);
    else
        gfx_text(120, 140, "WHITE: PATTERN  ORANGE: SOUND  SHIFT+BLUE: EDIT", C_GREY, 1);
}
