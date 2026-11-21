/* py_pattern: PLAY's pattern tracks (play.h, new_pattern_tape.md 2.2 / 2.3).
 *
 *   song      py_song (py_song.c): up to 8 tracks of any type, song grid, chains; kept in SONG.BIN
 *             in the set folder, written by the main thread ~2 s after the last edit.
 *   patterns  DRUM ROLL's and NOTE ROLL's patterns as PLAY plays them: a copy per pattern,
 *             refreshed in the main loop (one pattern of each kind per pass, round robin): an
 *             edited pattern from the editor's array, an unedited one imported from TE's finger
 *             data (once TE loaded it), else empty. A copy is published by a pointer swap between
 *             two buffers, only when it is consistent (the source hashed the same before and after
 *             the copy) and changed; the audio interrupt reads only published copies.
 *   playing   in the audio interrupt (PLAY's volume hook), per block and pattern track: its song
 *             head (py_song) walks its chains; the editor's own scheduler (dr_schedule /
 *             nr_schedule) gives the hits of the current pattern in this block on PLAY's clock.
 *             DRUM: py_drum plays them from TE's kit of the track's drum slot (read in place).
 *             SYNTH: py_synth plays them on an engine instance (1..3, enginehost) with the
 *             slot's knobs and TE's envelope code. The instances are a pool: a SYNTH track takes
 *             one for its first note and gives it back once its voices are silent, so up to 8
 *             SYNTH tracks take turns, at most 6 sounding at once, the 4th to 6th only while
 *             PLAY's CPU allows it (PY_INST_CYC; a note that finds none takes
 *             an instance whose track only has release tails left, else it is dropped).
 *             DRUM blocks go into py_mx.pin[t];
 *             SYNTH tracks are summed with their gains at 88.2 kHz and decimated once
 *             (ECO tracks: at 44.1 kHz, no decimation) into py_mx.ext. A track at gain 0 renders
 *             nothing. CYCLES per track for PLAY's CPU numbers and the ECO boost budget
 *             (PY_BOOST_CYC: an ECO render may do twice the work where the engine wants it).
 * TE addresses: static RE 2026-11-15 (analysis/notes/re_drum.md, re_voice.md, re_rolls.md). */
#include <op1/te246.h>
#include "../drumroll/dr_core.h"
#include "../drumroll/dr_fs.h"
#include "../drumroll/dr_store.h"
#include "../noteroll/nr_core.h"
#include "../noteroll/nr_fs.h"
#include "../noteroll/nr_store.h"
#include "../enginehost/eh_host.h"
#include "py_core.h"
#include "py_song.h"
#include "py_drum.h"
#include "py_synth.h"
#include "play.h"
#include "py_yaffs.h"
#include "py_slot.h"
#include "py_tev.h"

/* drum kits: [8] holder -> SampleBuffer -> +0 int16 *m_buffer (8 samples of padding first) */
#define TE_DRUM_HOLDERS     ((void **volatile *)0x01cc10cc)  /* [8] */
#define TE_DRUM_KEYS        0x01aaadb4u         /* + slot * 0x180 + key * 16: 8 int16 */
#define TE_SYNTH_ADSR       0x01b60ef0u         /* + slot * 16: 8 int16 (JSON "adsr") */
/* TE's envelope (dsp/Adsr.cpp) on PLAY's own objects: audio interrupt, after TE's render */
#define TE_ADSR_START       ((void (*)(void *))0x0191211a)
#define TE_ADSR_RELEASE     ((void (*)(void *, int))0x019120d0)
#define TE_ADSR_RENDER      ((int (*)(void *, int16_t *, const int16_t *, const int16_t *))0x01912128)
/* TE's finger patterns, loaded from okseq on first use: TE's getter for the finger sequencer
 * (0x019ed238, static RE 2026-11-15) builds both sequencers (synth 0x01dd98bc, drum 0x01dda710) on
 * its first call, reads both okseq rows (0x019ed0a4, SQL on op1.db) and sets FS_LOADED; TE calls it
 * when the finger sequencer is first opened or played */
#define FS_LOADED           (*(volatile uint8_t *)0x01ddb564)
#define TE_FS_GET           ((void *(*)(void))0x019ed238)
#define FS_DNOTE(p, s, h)   (*(volatile int32_t *)(0x01dda720 + (p) * 0x100 + (s) * 8 + (h) * 4))
#define FS_DLEN(p)          (*(volatile int32_t *)(0x01ddb520 + 4 * (p)))
#define FS_SNOTE(p, s, h)   (*(volatile int32_t *)(0x01dd98cc + (p) * 0x100 + (s) * 8 + (h) * 4))
#define FS_SLEN(p)          (*(volatile int32_t *)(0x01dda6cc + 4 * (p)))

extern dr_pattern dr_pat[DR_PATTERNS];          /* DRUM ROLL */
extern volatile uint16_t dr_owned;
extern volatile uint8_t dr_swing, dr_loaded;
extern nr_pattern nr_pat[NR_PATTERNS];          /* NOTE ROLL */
extern volatile uint16_t nr_owned;
extern volatile uint8_t nr_swing, nr_loaded;
extern py_mixer py_mx;                          /* play.c */

#define SONG_FILE       "SONG.BIN"
#define SAVE_QUIET      250                     /* main-loop passes (~2 s) after the last edit */

ps_song py_song;                                /* the GUI and keys edit it */
static volatile uint8_t song_dirty;
static volatile uint8_t slot_edited;            /* the song was edited since the last slot save / load */
static uint16_t quiet;
static const char *song_dir;

/* published pattern copies */
static dr_pattern dcopy[DR_PATTERNS][2];
static const dr_pattern *volatile dpub[DR_PATTERNS];
static volatile uint16_t dlen16[DR_PATTERNS];
static uint32_t dhash[DR_PATTERNS];
static nr_pattern ncopy[NR_PATTERNS][2];
static const nr_pattern *volatile npub[NR_PATTERNS];
static uint32_t nhash[NR_PATTERNS];
static union { dr_pattern d; nr_pattern n; } scratch_pat __attribute__((aligned(4)));
static union { dr_fs_notes d; nr_fs_notes n; } te_notes;
static uint8_t next_d, next_n;
static uint8_t fs_asked;                        /* TE_FS_GET called (once) */

static uint64_t dsteps[DR_RATES], nstep;        /* samples per step (16.16) */
static uint32_t steps_bpm;
static volatile uint32_t clock_bar;             /* frames per bar */

static ps_head heads[PS_TRACKS];
/* the list's / keys' sound wins over the playing chain row's until the track's next row */
static volatile uint8_t snd_over[PS_TRACKS];
static int slot_of(int t, int synth);
static volatile uint8_t start_row;              /* the song row every column starts from / loops back to */
static volatile int8_t aud_track = -1;          /* the chain view's audition: this track plays ... */
static volatile uint8_t aud_chain;              /* ... this chain alone */
/* live: the song row track t is asked to jump to (PS_EMPTY = none); the interrupt hands it to the head */
static volatile uint8_t cue_req[PS_TRACKS] = { PS_EMPTY, PS_EMPTY, PS_EMPTY, PS_EMPTY,
                                               PS_EMPTY, PS_EMPTY, PS_EMPTY, PS_EMPTY };
static py_drums drums[PS_TRACKS];
static py_syn syns[PS_TRACKS];
/* the instance pool (audio interrupt; py_pat_start resets it with the interrupt not playing) */
static volatile uint8_t inst_of[PS_TRACKS];     /* SYNTH track -> engine instance 1..6, 0 = none now */
static uint8_t inst_busy;                       /* bit i: instance i is taken */
static volatile uint16_t starved[PS_TRACKS];    /* blocks left of "a note found no instance" (GUI) */
#define STARVED_BLOCKS  690                     /* ~1 s */
volatile uint32_t py_drum_hits, py_syn_notes;   /* bench, debug */

/* ------------------------------------------------------------------ main thread */

void py_pat_dirty(void)
{
    song_dirty = 1;
    slot_edited = 1;
}

/* GUI: a note of SYNTH track t found no free engine instance in the last second */
int py_pat_starved(int t)
{
    return t >= 0 && t < PS_TRACKS && starved[t] != 0 && py_mx.playing;
}

int py_pat_any(void)
{
    int r, t;

    for (r = 0; r < PS_ROWS; r++)
        for (t = 0; t < PS_TRACKS; t++)
            if (py_song_is_pattern(&py_song, t) && py_song.cell[r][t] != PS_EMPTY)
                return 1;
    return 0;
}

static uint8_t songbuf[PS_FILE_BYTES] __attribute__((aligned(4)));   /* SONG.BIN, slots (main; yaffs: even addresses) */

void py_pat_load(const char *dir)
{
    uint8_t *buf = songbuf;
    char path[64];
    int fd, n = -1;

    song_dir = dir;
    TE_SNPRINTF(path, sizeof(path), "%s/%s", dir, SONG_FILE);
    fd = TE_FILE_OPEN(path, TE_FILE_RDONLY, TE_FILE_MODE);
    if (fd >= 0) {
        n = PY_PREAD(fd, buf, PS_FILE_BYTES, 0);
        TE_FILE_CLOSE(fd);
    }
    if (n < 8 || py_song_load(&py_song, buf, (uint32_t)n))      /* (an older, shorter file is read too) */
        py_song_clear(&py_song);
    song_dirty = 0;
}

static void save_song(void)
{
    uint8_t *buf = songbuf;
    char path[64];
    int fd;

    if (!song_dir)
        return;
    PY_MKDIR(song_dir, 0);                      /* the first song before any set was imported */
    py_song_save(&py_song, buf);
    TE_SNPRINTF(path, sizeof(path), "%s/%s", song_dir, SONG_FILE);
    fd = TE_FILE_OPEN(path, TE_FILE_CREATE, TE_FILE_CREATE_MODE);
    if (fd < 0)
        return;
    TE_FILE_WRITE(fd, buf, PS_FILE_BYTES);
    TE_FILE_CLOSE(fd);
}

static void copy_bytes(void *d, const void *s, uint32_t n)
{
    uint8_t *a = d;
    const uint8_t *b = s;

    while (n--)
        *a++ = *b++;
}

/* DRUM ROLL pattern p as PLAY plays it, into out; 0 if the source changed meanwhile */
static int dsource(int p, dr_pattern *out)
{
    if ((dr_owned >> p) & 1) {
        uint32_t h;

        if (!dr_loaded)
            return 0;
        h = dr_hash(&dr_pat[p], sizeof(dr_pattern));
        copy_bytes(out, &dr_pat[p], sizeof(dr_pattern));
        return dr_hash(&dr_pat[p], sizeof(dr_pattern)) == h;
    }
    if (FS_LOADED) {
        int st, hd;

        for (st = 0; st < DR_FS_STEPS; st++)
            for (hd = 0; hd < DR_FS_HANDS; hd++)
                te_notes.d[st][hd] = FS_DNOTE(p, st, hd);
        dr_fs_import(te_notes.d, FS_DLEN(p), out);
    } else {
        dr_pattern_init(out);                   /* TE has not loaded its patterns yet: empty */
    }
    return 1;
}

static int nsource(int p, nr_pattern *out)
{
    if ((nr_owned >> p) & 1) {
        uint32_t h;

        if (!nr_loaded)
            return 0;
        h = nr_hash(&nr_pat[p], sizeof(nr_pattern));
        copy_bytes(out, &nr_pat[p], sizeof(nr_pattern));
        return nr_hash(&nr_pat[p], sizeof(nr_pattern)) == h;
    }
    if (FS_LOADED) {
        int st, hd;

        for (st = 0; st < NR_FS_STEPS; st++)
            for (hd = 0; hd < NR_FS_HANDS; hd++)
                te_notes.n[st][hd] = FS_SNOTE(p, st, hd);
        nr_fs_import(te_notes.n, FS_SLEN(p), out);
    } else {
        nr_pattern_init(out);
    }
    return 1;
}

/* refresh one DRUM ROLL and one NOTE ROLL pattern's published copy (round robin) */
static void refresh(void)
{
    int p = next_d;
    uint32_t h;

    next_d = (uint8_t)((p + 1) % DR_PATTERNS);
    if (dsource(p, &scratch_pat.d)) {
        h = dr_hash(&scratch_pat.d, sizeof(dr_pattern));
        if (!dpub[p] || h != dhash[p]) {
            dr_pattern *back = dpub[p] == &dcopy[p][0] ? &dcopy[p][1] : &dcopy[p][0];

            copy_bytes(back, &scratch_pat.d, sizeof(dr_pattern));
            dlen16[p] = (uint16_t)dr_fs_length(back);
            __asm__ volatile("" ::: "memory");
            dpub[p] = back;
            dhash[p] = h;
        }
    }
    p = next_n;
    next_n = (uint8_t)((p + 1) % NR_PATTERNS);
    if (nsource(p, &scratch_pat.n)) {
        h = nr_hash(&scratch_pat.n, sizeof(nr_pattern));
        if (!npub[p] || h != nhash[p]) {
            nr_pattern *back = npub[p] == &ncopy[p][0] ? &ncopy[p][1] : &ncopy[p][0];

            copy_bytes(back, &scratch_pat.n, sizeof(nr_pattern));
            __asm__ volatile("" ::: "memory");
            npub[p] = back;
            nhash[p] = h;
        }
    }
}

/* ------------------------------------------------------------------ song slots (main thread)
 * SONGnn.BIN in PY_DIR_S: the song with all 28 patterns as PLAY plays them (py_slot.c). A load
 * makes every pattern the editors' (NOTE ROLL / DRUM ROLL write TE's copies and op1.db). */
extern void nr_put_pattern(int p, const nr_pattern *in), nr_put_done(int swing);
extern void dr_put_pattern(int p, const dr_pattern *in), dr_put_done(int swing);

static nr_pattern n_empty;
static dr_pattern d_empty;
static uint8_t empties;
static int slot_fd = -1;
static volatile int8_t cur_slot = -1;           /* the slot last saved or loaded, -1 = none */
static uint32_t slot_hash;                      /* the patterns' hashes then */

static int sl_get(void *ctx, int kind, int p, void *out)
{
    (void)ctx;
    return kind == PSL_DR ? !dsource(p, out) : !nsource(p, out);
}

static void sl_put(void *ctx, int kind, int p, const void *in)
{
    (void)ctx;
    if (kind == PSL_DR)
        dr_put_pattern(p, in);
    else
        nr_put_pattern(p, in);
}

static int sl_write(void *ctx, const void *buf, uint32_t n)
{
    (void)ctx;
    return TE_FILE_WRITE(slot_fd, buf, n) == (int)n ? 0 : -1;
}

static int sl_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    (void)ctx;
    return PY_PREAD(slot_fd, buf, n, (int32_t)off);
}

/* the synth sounds a slot stores (py_slot.h): TE type, octave, the SDK tag, knobs, envelope.
 * Taken back only onto a drwave sound and only if the stored one is an SDK engine PLAY runs here
 * (the slot keeps its sound otherwise: TE's engine type is not switched). Every value written is
 * clamped into TE's ranges first (stored sounds are range-checked at boot: out of range = no
 * boot): drwave's knob ranges, the envelope 0..32767 (play mode 0..3), the octave -4..4. TE's
 * own setters write the patch's JSON and save it, as its screens do (knobs last: it saves). */
#define TE_PATCH_SET_ADSR   ((void (*)(void *patch, int16_t *adsr))0x01a00c98)
#define TE_PATCH_SET_OCTAVE ((void (*)(void *patch, int octave))0x019ff36c)

static const int16_t drwave_max[8] = { 24575, 16383, 16383, 32767, 32767, 32767, 32767, 32767 };
uint8_t py_snd_why[PSL_SOUNDS];                 /* sl_sok's last answer per slot: PY_SND_* */
uint8_t py_snd_tag[PSL_SOUNDS];                 /* the stored sound's SDK tag (0: none) */

static int16_t le16(const uint8_t *b) { return (int16_t)(b[0] | b[1] << 8); }

static void unpack_knobs(const uint8_t *rec, int16_t *k)
{
    int i;

    for (i = 0; i < 8; i++)
        k[i] = le16(rec + 4 + 2 * i);
}

static int sl_sget(void *ctx, int s, uint8_t *rec)
{
    const volatile int16_t *k = TE_SYNTH_KNOBS + s * 8;
    const volatile int16_t *a = (const volatile int16_t *)(TE_SYNTH_ADSR + (uint32_t)s * 16u);
    int i;

    (void)ctx;
    rec[0] = (uint8_t)TE_SYNTH_TYPE[s];
    rec[1] = (uint8_t)(int8_t)TE_SYNTH_OCTAVE[s];
    rec[2] = (uint8_t)(TE_SYNTH_TYPE[s] == TE_INSTR_DRWAVE ? eh_tag(k) : 0);
    rec[3] = 0;
    for (i = 0; i < 8; i++) {
        rec[4 + 2 * i] = (uint8_t)k[i];
        rec[5 + 2 * i] = (uint8_t)((uint16_t)k[i] >> 8);
        rec[20 + 2 * i] = (uint8_t)a[i];
        rec[21 + 2 * i] = (uint8_t)((uint16_t)a[i] >> 8);
    }
    return 0;
}

static int sl_sok(void *ctx, int s, const uint8_t *rec)
{
    int16_t k[8];
    int why;

    (void)ctx;
    unpack_knobs(rec, k);
    py_snd_tag[s] = (uint8_t)(rec[0] == TE_INSTR_DRWAVE ? eh_tag(k) : 0);
    why = !py_snd_tag[s] ? PY_SND_TE
        : !eh_multi_ok(k) ? PY_SND_MISSING
        : TE_SYNTH_TYPE[s] != TE_INSTR_DRWAVE ? PY_SND_OTHER : PY_SND_OK;
    py_snd_why[s] = (uint8_t)why;
    return why == PY_SND_OK;
}

static void sl_sput(void *ctx, int s, const uint8_t *rec)
{
    static int16_t k[8] __attribute__((aligned(4))), a[8] __attribute__((aligned(4)));
    volatile int16_t *tk = TE_SYNTH_KNOBS + s * 8;
    volatile int16_t *ta = (volatile int16_t *)(TE_SYNTH_ADSR + (uint32_t)s * 16u);
    int i, o = (int8_t)rec[1];

    (void)ctx;
    unpack_knobs(rec, k);
    for (i = 0; i < 8; i++) {
        a[i] = le16(rec + 20 + 2 * i);
        k[i] = k[i] < 0 ? 0 : k[i] > drwave_max[i] ? drwave_max[i] : k[i];
        a[i] = a[i] < 0 ? 0 : a[i];
    }
    if (a[4] > 0x3fff)                          /* play mode (>> 12): 0..3 */
        a[4] = 0x3fff;
    o = o < -4 ? -4 : o > 4 ? 4 : o;
    for (i = 0; i < 8; i++) {
        ta[i] = a[i];
        tk[i] = k[i];
    }
    TE_SYNTH_OCTAVE[s] = o;
    TE_PATCH_SET_OCTAVE(TE_SYNTH_PATCH(s), o);
    TE_PATCH_SET_ADSR(TE_SYNTH_PATCH(s), (int16_t *)ta);
    TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(s), (int16_t *)tk);
}

static psl_io slot_io = { 0, { sizeof(nr_pattern), sizeof(dr_pattern) }, { &n_empty, &d_empty }, &scratch_pat,
                          sl_get, sl_put, sl_write, sl_read, 0, sl_sget, sl_sput, sl_sok };

static void slot_path(char *out, int n)
{
    TE_SNPRINTF(out, 64, "%s/SONG%d%d.BIN", PY_DIR_S, (n + 1) / 10, (n + 1) % 10);
}

void py_pat_slot_path(char *out, int n) { slot_path(out, n); }

void py_pat_slot_sizes(uint32_t size[2])
{
    size[0] = slot_io.size[0];
    size[1] = slot_io.size[1];
}

int py_pat_slot_busy(void) { return slot_fd >= 0; }

static void slot_open(int n, int write)
{
    char path[64];

    if (!empties) {
        nr_pattern_init(&n_empty);
        dr_pattern_init(&d_empty);
        empties = 1;
    }
    slot_path(path, n);
    if (write)
        PY_MKDIR(PY_DIR_S, 0);
    slot_fd = write ? TE_FILE_OPEN(path, TE_FILE_CREATE, TE_FILE_CREATE_MODE)
                    : TE_FILE_OPEN(path, TE_FILE_RDONLY, TE_FILE_MODE);
}

static void slot_close(void)
{
    if (slot_fd >= 0)
        TE_FILE_CLOSE(slot_fd);
    slot_fd = -1;
}

/* the published patterns' hashes in one number (what plays: an edit in an editor changes it) */
static uint32_t pat_hashes(void)
{
    uint32_t h = 0;
    int p;

    for (p = 0; p < PSL_PATS; p++)
        h = (h * 31u + dhash[p]) * 31u + nhash[p];
    return h;
}

/* both editors have read op1.db (else their load would replace ours later) and TE's own
 * patterns are in RAM (else an unedited pattern would read empty) */
static int slots_ready(void)
{
    if (!nr_loaded || !dr_loaded)
        return 0;
    if (!FS_LOADED && TE_DB_OP1() && *TE_DB_LOCK && !fs_asked) {
        fs_asked = 1;
        TE_FS_GET();
    }
    return FS_LOADED || ((nr_owned & dr_owned) & 0x3fff) == 0x3fff;
}

int py_pat_slot_peek(int n, psl_info *info)
{
    int r;

    slot_open(n, 0);
    r = slot_fd < 0 ? PSL_NOFILE : py_slot_peek(&slot_io, info);
    slot_close();
    return r;
}

int py_pat_slot_save(int n)
{
    uint8_t swing[2];
    psl_info info;
    int r;

    if (!slots_ready())
        return PSL_WAIT;
    swing[PSL_NR] = nr_swing;
    swing[PSL_DR] = dr_swing;
    slot_io.cur_sound = (int)(*TE_SYNTH_SLOT & 7);
    slot_open(n, 1);
    if (slot_fd < 0)
        return PSL_IO;
    r = py_slot_save(&py_song, swing, &slot_io, songbuf);
    slot_close();
    if (r == PSL_OK) {                          /* read it back once */
        slot_open(n, 0);
        r = slot_fd < 0 ? PSL_IO : py_slot_check(&slot_io, &info) ? PSL_IO : PSL_OK;
        slot_close();
    }
    if (r != PSL_OK) {
        char path[64];

        slot_path(path, n);
        PY_UNLINK(path);                        /* no half-written slot */
        return r;
    }
    cur_slot = (int8_t)n;
    slot_edited = 0;
    slot_hash = pat_hashes();
    return PSL_OK;
}

/* which patterns / sounds of slot n differ from now (diff[3]), which patterns its song plays;
 * *sbad: the stored sounds that cannot come back (why: py_snd_why, the engine: py_snd_tag) */
int py_pat_slot_diff(int n, uint16_t diff[3], uint16_t used[2], uint8_t *sbad)
{
    psl_info info;
    int r;

    diff[0] = diff[1] = diff[2] = used[0] = used[1] = 0;
    *sbad = 0;
    if (!slots_ready())
        return PSL_WAIT;
    slot_open(n, 0);
    r = slot_fd < 0 ? PSL_NOFILE : py_slot_diff(&slot_io, &info, diff);
    slot_close();
    if (r == PSL_OK) {
        used[0] = info.used[0];
        used[1] = info.used[1];
        *sbad = info.sbad;
    }
    return r;
}

/* the interrupt not playing; the caller reopens the AUDIO tracks and the heads. keep: patterns
 * left as they are (bit p of [PSL_NR] / [PSL_DR]), sounds ([2]) */
int py_pat_slot_load(int n, const uint16_t keep[3])
{
    uint8_t swing[2];
    int r, i;

    if (!slots_ready())
        return PSL_WAIT;
    slot_open(n, 0);
    r = slot_fd < 0 ? PSL_NOFILE : py_slot_load(&py_song, swing, &slot_io, songbuf, keep);
    slot_close();
    if (r != PSL_OK)
        return r;
    nr_put_done(swing[PSL_NR]);
    dr_put_done(swing[PSL_DR]);
    for (i = 0; i < PSL_PATS; i++)
        refresh();                              /* every published copy anew before play */
    song_dirty = 1;                             /* SONG.BIN: the loaded song */
    cur_slot = (int8_t)n;
    slot_edited = (keep[0] | keep[1]) != 0;     /* a pattern kept: not the slot's any more (a
                                                 * sound kept: the song is still the slot's) */
    slot_hash = pat_hashes();
    return PSL_OK;
}

int py_pat_slot_delete(int n)
{
    char path[64];

    slot_path(path, n);
    if (cur_slot == n)
        cur_slot = -1;
    return PY_UNLINK(path) < 0 ? PSL_IO : PSL_OK;
}

/* GUI: the slot last saved or loaded (-1: none); *changed: the song or a pattern changed since */
int py_pat_slot_now(int *changed)
{
    *changed = slot_edited || pat_hashes() != slot_hash;
    return cur_slot;
}

void py_pat_main(uint32_t bar, uint32_t bpm_x100)
{
    int r;

    if (bpm_x100 != steps_bpm) {                /* 64-bit divides: here, not in the interrupt */
        for (r = 0; r < DR_RATES; r++)
            dsteps[r] = dr_step_q16(bpm_x100, r);
        nstep = nr_step_q16(bpm_x100);
        steps_bpm = bpm_x100;
    }
    clock_bar = bar;
    /* TE's own finger patterns (every pattern NOTE ROLL / DRUM ROLL never edited) are not in RAM
     * until TE loads them: once op1.db is open (as NOTE ROLL waits for it) and the song has a
     * SYNTH / DRUM track, load them as opening the finger sequencer would; until then such a
     * pattern plays empty. Main thread, once. */
    if (!FS_LOADED && !fs_asked && TE_DB_OP1() && *TE_DB_LOCK && py_pat_any()) {
        fs_asked = 1;
        TE_FS_GET();
    }
    if (py_pat_any())
        py_tev_main();                          /* TE engines: a prototype of each one a slot holds */
    refresh();
    if (song_dirty) {
        song_dirty = 0;
        quiet = SAVE_QUIET;
    } else if (quiet && !--quiet) {
        save_song();
    }
}

/* the tempo changed (TE's tempo page): every frame count of the pattern clock scaled by the new
 * bar length over the old one, the play position with it, so each track keeps its place in the
 * bar and its pattern; voices sound on. Main thread; the interrupt sees all of it at once. */
static uint32_t rescale(uint32_t x, uint32_t nb, uint32_t ob)
{
    return (uint32_t)(((uint64_t)x * nb + ob / 2) / ob);
}

void py_pat_retime(uint32_t bar, uint32_t bpm_x100)
{
    uint64_t ds[DR_RATES], ns = nr_step_q16(bpm_x100);
    uint32_t ob = clock_bar, mask;
    int r, t;

    for (r = 0; r < DR_RATES; r++)
        ds[r] = dr_step_q16(bpm_x100, r);
    __asm__ volatile("cli %0;" : "=d"(mask) : : "memory");
    if (ob && bar != ob) {
        for (t = 0; t < PS_TRACKS; t++) {
            uint32_t end = rescale(heads[t].start + heads[t].len, bar, ob);

            heads[t].start = rescale(heads[t].start, bar, ob);
            heads[t].len = end - heads[t].start;    /* (the next head starts where this one ends) */
        }
        py_mx.pos = rescale(py_mx.pos, bar, ob);
    }
    for (r = 0; r < DR_RATES; r++)
        dsteps[r] = ds[r];
    nstep = ns;
    steps_bpm = bpm_x100;
    clock_bar = bar;
    __asm__ volatile("sti %0;" : : "d"(mask) : "memory");
}

/* length (frames) of what pattern track t plays for pattern p */
static uint32_t pat_len(void *ctx, int t, int p)
{
    (void)ctx;
    if (p < 0 || p >= 14)
        return 0;
    if (py_song.trk[t].type == PS_DRUM && dpub[p])
        return dlen16[p] * clock_bar / 16;      /* multiply first: 1/16 = 5512.5 frames at 120 */
    if (py_song.trk[t].type == PS_SYNTH && npub[p])
        return npub[p]->len * clock_bar / 16;
    return 0;
}

static volatile uint8_t hd;                     /* a render: every SYNTH track without ECO, all voices */
static int32_t bus88[128];                      /* non-ECO SYNTH tracks, gains applied, 88.2 kHz */
static int32_t eco44[PY_BLOCK];                 /* ECO SYNTH tracks + the decimated bus */
static int32_t hist88[62];                      /* the shared decimator's state */
static uint8_t hist_live;                       /* hist88 holds something not yet flushed */

/* ECO boost: an ECO track's render carries PY_SYN_BOOST (ACID 3: its filter at 88.2 kHz while the
 * cutoff is high, up to twice the render's work) while PLAY's work in the last block of the same
 * parity, the boosts granted in this block and this track's last render stay under PY_BOOST_CYC */
#define PY_BOOST_CYC    200000u                 /* CYCLES per block: ~35 % of one at 400 MHz (TE's own
                                                 * 25-30 %; sim load bench, ECO off: PLAY's peak 201k) */
typedef char py_boost_flags_match[PY_SYN_BOOST == EH_BOOST && PY_SYN_HALF == EH_HALF ? 1 : -1];
static uint32_t blk_cyc[2];                     /* PLAY's track work in the last block of each parity */
static uint32_t blk_sum, boost_add;             /* this block: track work so far, the boosts' extra */
static uint32_t ren_cyc[PS_TRACKS];             /* the track's last ECO render */
volatile uint32_t py_boosts;                    /* ECO renders that carried PY_SYN_BOOST (sim) */

/* main thread, the interrupt not playing: every pattern head placed at global frame g */
void py_pat_start(uint32_t g)
{
    int t, i;

    for (t = 0; t < PS_TRACKS; t++) {
        heads[t].active = 0;
        cue_req[t] = PS_EMPTY;
        snd_over[t] = 0;
        if (py_song_is_pattern(&py_song, t)) {
            if (t == aud_track)
                py_song_start_chain(&py_song, t, aud_chain, 0, clock_bar, pat_len, 0, &heads[t]);
            else
                py_song_start(&py_song, t, start_row, 0, clock_bar, pat_len, 0, &heads[t]);
            py_song_seek(&py_song, t, g, clock_bar, pat_len, 0, &heads[t]);
        }
        for (i = 0; i < PY_DVOICES; i++)
            drums[t].v[i].on = 0;
        py_syn_reset(&syns[t]);
        inst_of[t] = 0;
        starved[t] = 0;
        ren_cyc[t] = 0;
    }
    inst_busy = 0;
    blk_cyc[0] = blk_cyc[1] = 0;
    for (i = 0; i < 62; i++)
        hist88[i] = 0;
    hist_live = 0;
    for (i = 0; i < 128; i++)                   /* a block left half done (a render cancelled) */
        bus88[i] = 0;
    for (i = 0; i < PY_BLOCK; i++) {
        eco44[i] = py_mx.ext[i] = 0;
        for (t = 0; t < PS_TRACKS; t++)
            py_mx.pin[t][i] = 0;
    }
    py_mx.pin_used = py_mx.ext_used = 0;
}

/* the track's sound was set by hand (list, keys 1-8): it sounds now, whatever the chain row says */
void py_pat_sound_set(int t)
{
    snd_over[t] = 1;
}

/* the slot track t plays now (0..7) */
int py_pat_slot(int t)
{
    return slot_of(t, py_song.trk[t].type == PS_SYNTH);
}

/* a render (main, not playing): every SYNTH track at full quality */
void py_pat_hd(int on)
{
    hd = (uint8_t)(on != 0);
}

/* frames of the longest pattern column played once from song row `row` (to its loop point); 0 =
 * no pattern track plays there */
uint32_t py_pat_song_frames(int row)
{
    uint32_t max = 0;
    int t, i;

    for (t = 0; t < PS_TRACKS; t++) {
        ps_head h;
        uint32_t sum = 0;
        int r0, c0;

        if (!py_song_is_pattern(&py_song, t) || !py_song_start(&py_song, t, row, 0, clock_bar, pat_len, 0, &h))
            continue;
        r0 = h.row;
        c0 = h.crow;
        for (i = 0; i < PS_ROWS * PS_CROWS; i++) {
            sum += h.len;
            py_song_next(&py_song, t, clock_bar, pat_len, 0, &h);
            if (!h.active || (h.row == r0 && h.crow == c0 && !h.loops))
                break;                          /* (a loop's first row again: not the end) */
        }
        if (sum > max)
            max = sum;
    }
    return max;
}

void py_pat_set_start(int row)
{
    start_row = (uint8_t)(row < 0 || row >= PS_ROWS ? 0 : row);
}

int py_pat_start_row(void)
{
    return start_row;
}

void py_pat_audition(int t, int chain)
{
    aud_chain = (uint8_t)chain;
    aud_track = (int8_t)(t >= 0 && t < PS_TRACKS && chain >= 0 && chain < PS_CHAINS ? t : -1);
}

int py_pat_audition_track(void)
{
    return aud_track;
}

/* live (any thread): pattern track t jumps to song row `row` when its current chain ends */
void py_pat_cue(int t, int row)
{
    if (t >= 0 && t < PS_TRACKS && row >= 0 && row < PS_ROWS)
        cue_req[t] = (uint8_t)row;
}

/* the row track t is cued to; -1 = no cue pending */
int py_pat_cued(int t)
{
    const ps_head *h = &heads[t];

    if (cue_req[t] < PS_ROWS)
        return cue_req[t];
    return h->active && h->solo == PS_EMPTY && h->cue < PS_ROWS ? h->cue : -1;
}

/* ------------------------------------------------------------------ audio interrupt */

/* the sound / kit slot track t uses now: the chain row's while it plays (if it names one), else
 * the track's (an older song's CUR: TE's current slot) */
static int slot_of(int t, int synth)
{
    int s = heads[t].active && heads[t].snd < 8 && !snd_over[t] ? heads[t].snd : py_song.trk[t].sound;

    return s < 8 ? s : (int)((synth ? *TE_SYNTH_SLOT : *TE_DRUM_SLOT) & 7);
}

typedef struct emit_ctx {
    const void *p;
    uint32_t base;                              /* pattern clock at the block's first sample */
    int t, slot, tsp;
} emit_ctx;

static void drum_emit(void *ctx, const dr_hit *h)
{
    const emit_ctx *c = ctx;
    const dr_lane *l = &((const dr_pattern *)c->p)->lane[h->lane];
    void **holder = TE_DRUM_HOLDERS[c->slot];
    const int16_t *pcm, *key;
    int32_t gain;

    if (!holder || !*holder)
        return;
    pcm = *(int16_t **)*holder;                 /* SampleBuffer +0: m_buffer */
    if (!pcm)
        return;
    pcm += 8;                                   /* 8 samples of padding */
    key = (const int16_t *)(TE_DRUM_KEYS + (uint32_t)c->slot * 0x180u + (uint32_t)l->key * 16u);
    gain = h->s->p[DR_VEL] * l->level * 32768 / 10000;
    py_drum_hit(&drums[c->t], pcm, key, h->s->p[DR_TONE] + 12 * TE_DRUM_OCTAVE[c->slot] + c->tsp,
                gain > 32767 ? 32767 : gain, h->hold, h->full, h->at - c->base);
    py_drum_hits++;
}

static void inst_free(int t)
{
    if (inst_of[t]) {
        inst_busy &= (uint8_t)~(1u << inst_of[t]);
        inst_of[t] = 0;
    }
}

/* overload guard: instances 1..PY_INST_SAFE any time; the 4th to 6th only while PLAY's peak
 * block (the last of either parity, at least PY_INST_EST per busy instance: at the song's start
 * nothing is measured yet and every track's first note comes in the same block) plus one more
 * instance's share stays under py_inst_cyc */
#define PY_INST_SAFE    3
#define PY_INST_EST     41000u                  /* CYCLES per sounding instance before any measure
                                                 * (sim: ACID 3 45k, MONO 3 23..37k per voice); v0.16:
                                                 * 41k (was 50k) so 6 x EST fits PY_INST_CYC and a 6th
                                                 * instance can start before anything is measured */
#define PY_INST_CYC     348000u                 /* CYCLES per block: ~60 % of one (v0.17, the owner's
                                                 * choice; was 250k = 43 %); TE's own 25-30 % on top:
                                                 * heavy songs may glitch (sim: 3 ACID 3 ECO off: 201k) */
volatile uint32_t py_inst_cyc = PY_INST_CYC;    /* (the sim lowers it) */

static int inst_room(void)
{
    uint32_t peak = blk_cyc[0] > blk_cyc[1] ? blk_cyc[0] : blk_cyc[1];
    int i, busy = 0;

    for (i = 1; i < EH_INST; i++)
        busy += (inst_busy >> i) & 1;
    if (busy < PY_INST_SAFE || hd)              /* (a render to a file: no deadline) */
        return 1;
    if (peak < (uint32_t)busy * PY_INST_EST)
        peak = (uint32_t)busy * PY_INST_EST;
    return peak + peak / (uint32_t)busy <= py_inst_cyc;
}

/* track t's instance, taken from the pool if it has none: a free one (the 4th to 6th while the CPU
 * allows: inst_room), else one whose track only has release tails left (cut); 0 = none (BUSY) */
static int inst_take(int t)
{
    int i, u, room = inst_room();

    if (inst_of[t])
        return inst_of[t];
    for (i = 1; i < EH_INST && room; i++)
        if (!(inst_busy & (1u << i))) {
            inst_busy |= (uint8_t)(1u << i);
            inst_of[t] = (uint8_t)i;
            return i;
        }
    for (u = 0; u < PS_TRACKS; u++)
        if (u != t && inst_of[u] && py_syn_tail(&syns[u])) {
            inst_of[t] = inst_of[u];
            inst_of[u] = 0;
            py_syn_reset(&syns[u]);
            return inst_of[t];
        }
    starved[t] = STARVED_BLOCKS;
    return 0;
}

static void synth_emit(void *ctx, const nr_hit *h)
{
    const emit_ctx *c = ctx;
    int key = h->n->key + 12 * TE_SYNTH_OCTAVE[c->slot] + c->tsp;

    if (key < 0 || key > 127)
        return;
    py_syn_notes++;
    if (!py_mx.gain[c->t] || !inst_take(c->t))
        return;                                 /* muted (renders nothing), or no instance free */
    py_syn_note(&syns[c->t], key, (h->n->vel * 127 + 50) / 100, h->hold);
}

/* the engine of the track being rendered: an SDK engine's instance (drwave sounds) or TE's own
 * voices of the instance (py_tev: every other engine; set by track_block) */
static int syn_te;

static int syn_render(int inst, int v, int16_t *out, const int16_t *knobs, const uint32_t *inc,
                      const int16_t *env, int flags)
{
    return syn_te ? py_tev_render(inst, v, out, knobs, inc, env, flags)
                  : eh_render_inst(inst, v, out, knobs, inc, flags);
}

static void syn_key_up(int inst, int v, const int16_t *knobs)
{
    if (syn_te)
        py_tev_key_up(inst, v, knobs);
}

static const py_syn_api te_syn_api = {
    syn_render, syn_key_up, TE_ADSR_START, TE_ADSR_RELEASE, TE_ADSR_RENDER,
};

/* track t's SYNTH sound in slot `slot` on its instance: 1 = it can sound (SDK engine, or TE's
 * engine with its voices ready: py_tev_bind; fresh voices restart the notes) */
static int syn_bind(int t, int slot)
{
    int r;

    syn_te = TE_SYNTH_TYPE[slot] != TE_INSTR_DRWAVE;
    if (!syn_te)
        return 1;
    r = py_tev_bind(inst_of[t], slot);
    if (r == 2)
        py_syn_restart(&syns[t]);
    return r != 0;
}

volatile uint32_t py_trk_cyc[PS_TRACKS];        /* CYCLES per track, summed (play.c windows them) */
volatile uint32_t py_dec_cyc;                   /* CYCLES of the shared decimator, summed */

static inline uint32_t cycles(void)
{
    uint32_t c;

    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

static void peak_to(int t, uint32_t pk)
{
    if (pk > 65535)
        pk = 65535;
    if (pk > py_mx.peak[t])
        py_mx.peak[t] = (uint16_t)pk;
}

/* track t: the hits of this block, then its voices: DRUM into pin[t], SYNTH into bus88 / eco44
 * with its gain. A track at gain 0 (muted, level -60, another one solo) renders nothing. */
static int track_block(int t, uint32_t pos)
{
    ps_head *h = &heads[t];
    const ps_track *tr = &py_song.trk[t];
    uint32_t g = pos, end = pos + PY_BLOCK, bar = clock_bar;
    int synth = tr->type == PS_SYNTH, guard = 0, bus = 0;
    int32_t gain = py_mx.gain[t];
    emit_ctx c;

    c.t = t;
    c.slot = slot_of(t, synth);                 /* (a hit before a row change in this block: the old row's) */
    if (cue_req[t] < PS_ROWS && aud_track < 0) {    /* live: a cue for this column */
        uint8_t r = cue_req[t];

        cue_req[t] = PS_EMPTY;
        if (h->active)
            h->cue = r;                         /* when its chain ends (py_song_next) */
        else
            h->start_row = r;                   /* silent: it joins there at the next bar (below) */
    }
    if (!h->active && aud_track < 0 && bar && py_song.cell[h->start_row][t] < PS_CHAINS) {
        /* a column that was empty when play started (or got its first pattern since): it joins
         * at the next bar, in time with the rest (before, it waited for stop + play) */
        uint32_t nb = pos - pos % bar;

        if (nb < pos)
            nb += bar;
        if (nb < end)
            py_song_start(&py_song, t, h->start_row, nb, bar, pat_len, 0, h);
    }
    if (h->active && (int32_t)(h->start - g) > 0)
        g = h->start;                           /* the head starts inside this block */
    while (h->active && g < end && guard++ < 8) {
        uint32_t hend = h->start + h->len, n;

        if (g >= hend) {
            py_song_next(&py_song, t, bar, pat_len, 0, h);
            snd_over[t] = 0;                    /* a new row: its sound again */
            c.slot = slot_of(t, synth);
            continue;
        }
        n = (hend < end ? hend : end) - g;
        c.base = pos - h->start;                /* pattern clock of the block's first sample */
        c.tsp = h->tsp;
        if (h->pat < 14) {
            if (synth && npub[h->pat]) {
                c.p = npub[h->pat];
                nr_schedule(c.p, g - h->start, n, nstep, nr_swing, h->start, synth_emit, &c);
            } else if (!synth && dpub[h->pat]) {
                const dr_pattern *p = dpub[h->pat];
                uint64_t step[DR_LANES];
                int l;

                for (l = 0; l < DR_LANES; l++)
                    step[l] = p->lane[l].rate < DR_RATES ? dsteps[p->lane[l].rate] : 0;
                c.p = p;
                dr_schedule(p, g - h->start, n, step, dr_swing, h->start, drum_emit, &c);
            }
        }
        g += n;
    }
    if (synth) {
        py_syn *s = &syns[t];
        uint8_t eco = tr->eco && !hd ? 1 : 0;

        s->cap = (uint8_t)(hd ? 0 : tr->voices ? tr->voices : PY_SYN_CAP_DEFAULT);
        s->par = (uint8_t)(inst_of[t] & 1);    /* ECO tracks on instances 1 / 3 and 2: other blocks */
        if (s->eco != eco) {
            s->eco = eco;
            s->phase = 0;
        }
        if (starved[t])
            starved[t]--;
        if (!gain || !inst_of[t]) {
            py_syn_reset(s);
        } else if (py_syn_active(s) || s->phase) {
            uint32_t blk = pos / PY_BLOCK, c0;
            int renders = eco && !s->phase && !((blk ^ s->par) & 1);   /* (else: no render this block) */

            s->boost = (uint8_t)(renders && blk_cyc[blk & 1] + boost_add + ren_cyc[t] <= PY_BOOST_CYC);
            if (s->boost) {
                boost_add += ren_cyc[t];
                py_boosts++;
            }
            c0 = cycles();
            if (syn_bind(t, c.slot)) {
                if (syn_te)
                    py_tev_enter(inst_of[t]);
                peak_to(t, py_syn_block(s, &te_syn_api, inst_of[t], (const int16_t *)(TE_SYNTH_KNOBS + c.slot * 8),
                                        (const int16_t *)(TE_SYNTH_ADSR + (uint32_t)c.slot * 16u), c.slot,
                                        gain, bus88, eco44, blk));
                if (syn_te)
                    py_tev_leave(inst_of[t]);
            } else {
                py_syn_reset(s);                /* not playable (now): its notes drop */
            }
            if (renders)
                ren_cyc[t] = cycles() - c0;
            bus = !eco;
            py_mx.ext_used = 1;
        }
        if (!py_syn_active(s) && !s->phase)
            inst_free(t);                       /* silent: the instance goes back to the pool */
    } else if (!gain) {
        int i;

        for (i = 0; i < PY_DVOICES; i++)
            drums[t].v[i].on = 0;
    } else if (py_drum_active(&drums[t])) {
        py_drum_render(&drums[t], py_mx.pin[t], PY_BLOCK);
        py_mx.pin_used |= (uint8_t)(1 << t);
    }
    return bus;
}

extern volatile int8_t dr_ext_pat, nr_ext_pat;  /* the editors' playheads follow PLAY */
extern volatile uint32_t dr_ext_clock, nr_ext_clock;

/* one block in steps (the render does them one at a time): step 0..PS_TRACKS-1 = that track's
 * hits and voices (nothing if it is no pattern track), step PS_TRACKS = the shared decimator and
 * the SYNTH sum into py_mx.ext */
void py_pat_step(uint32_t pos, int step)
{
    static int dp, np, bus;
    uint32_t c0;
    int i;

    if (step == 0) {
        dp = np = -1;
        bus = 0;
        blk_sum = boost_add = 0;
        for (i = 0; i < PS_TRACKS; i++)         /* a track removed or no longer SYNTH: its instance back */
            if (inst_of[i] && py_song.trk[i].type != PS_SYNTH) {
                py_syn_reset(&syns[i]);
                inst_free(i);
            }
    }
    if (step < PS_TRACKS) {
        const ps_head *h = &heads[step];

        if (!py_song_is_pattern(&py_song, step))
            return;
        c0 = cycles();
        bus |= track_block(step, pos);
        c0 = cycles() - c0;
        py_trk_cyc[step] += c0;
        blk_sum += c0;
        if (h->active && h->pat < 14 && pos >= h->start) {
            if (py_song.trk[step].type == PS_DRUM && dp < 0) {
                dp = h->pat;
                dr_ext_clock = pos - h->start;
            } else if (py_song.trk[step].type == PS_SYNTH && np < 0) {
                np = h->pat;
                nr_ext_clock = pos - h->start;
            }
        }
        return;
    }
    dr_ext_pat = (int8_t)dp;
    nr_ext_pat = (int8_t)np;
    c0 = cycles();
    if (bus || hist_live) {                     /* one decimator for every non-ECO SYNTH track */
        static int32_t out[PY_BLOCK];

        py_syn_decimate(hist88, bus88, out);    /* silent bus: flushes the history (62 < 128) */
        for (i = 0; i < PY_BLOCK; i++)
            eco44[i] += out[i];
        for (i = 0; i < 128; i++)
            bus88[i] = 0;
        hist_live = (uint8_t)bus;
        py_mx.ext_used = 1;
    }
    if (py_mx.ext_used)
        for (i = 0; i < PY_BLOCK; i++) {
            py_mx.ext[i] += eco44[i];
            eco44[i] = 0;
        }
    c0 = cycles() - c0;
    py_dec_cyc += c0;
    blk_cyc[(pos / PY_BLOCK) & 1] = blk_sum + c0;
}

void py_pat_block(uint32_t pos)
{
    int step;

    for (step = 0; step <= PS_TRACKS; step++)
        py_pat_step(pos, step);
}

void py_pat_idle(void)
{
    dr_ext_pat = -1;
    nr_ext_pat = -1;
}

/* GUI: the head of track t */
const ps_head *py_pat_head(int t)
{
    return &heads[t];
}
