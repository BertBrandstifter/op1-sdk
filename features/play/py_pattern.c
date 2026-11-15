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
 *             SYNTH: py_synth plays them on the track's own engine instance (1..3, enginehost)
 *             with the slot's knobs and TE's envelope code. DRUM blocks go into py_mx.pin[t];
 *             SYNTH tracks are summed with their gains at 88.2 kHz and decimated once
 *             (ECO tracks: at 44.1 kHz, no decimation) into py_mx.ext. A track at gain 0 renders
 *             nothing. CYCLES per track for PLAY's CPU numbers.
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
static union { dr_pattern d; nr_pattern n; } scratch_pat;
static union { dr_fs_notes d; nr_fs_notes n; } te_notes;
static uint8_t next_d, next_n;
static uint8_t fs_asked;                        /* TE_FS_GET called (once) */

static uint64_t dsteps[DR_RATES], nstep;        /* samples per step (16.16) */
static uint32_t steps_bpm;
static volatile uint32_t clock_bar;             /* frames per bar */

static ps_head heads[PS_TRACKS];
static py_drums drums[PS_TRACKS];
static py_syn syns[PS_TRACKS];
static volatile uint8_t inst_of[PS_TRACKS];     /* SYNTH track -> engine instance 1..3, 0 none */
volatile uint32_t py_drum_hits, py_syn_notes;   /* bench, debug */

/* ------------------------------------------------------------------ main thread */

void py_pat_dirty(void)
{
    song_dirty = 1;
}

/* engine instances 1..3 to the first three SYNTH tracks (in track order) */
void py_pat_tracks_changed(void)
{
    int t, n = 1;

    for (t = 0; t < PS_TRACKS; t++)
        inst_of[t] = (uint8_t)(py_song.trk[t].type == PS_SYNTH && n < EH_INST ? n++ : 0);
}

int py_pat_inst(int t)
{
    return inst_of[t];
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

void py_pat_load(const char *dir)
{
    static uint8_t buf[PS_FILE_BYTES] __attribute__((aligned(4)));   /* yaffs: even addresses */
    char path[64];
    int fd, n = -1;

    song_dir = dir;
    TE_SNPRINTF(path, sizeof(path), "%s/%s", dir, SONG_FILE);
    fd = TE_FILE_OPEN(path, TE_FILE_RDONLY, TE_FILE_MODE);
    if (fd >= 0) {
        n = PY_PREAD(fd, buf, sizeof(buf), 0);
        TE_FILE_CLOSE(fd);
    }
    if (n != (int)sizeof(buf) || py_song_load(&py_song, buf, (uint32_t)n))
        py_song_clear(&py_song);
    song_dirty = 0;
    py_pat_tracks_changed();
}

static void save_song(void)
{
    static uint8_t buf[PS_FILE_BYTES] __attribute__((aligned(4)));   /* yaffs: even addresses */
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
    TE_FILE_WRITE(fd, buf, sizeof(buf));
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
    refresh();
    if (song_dirty) {
        song_dirty = 0;
        quiet = SAVE_QUIET;
    } else if (quiet && !--quiet) {
        save_song();
    }
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

static int32_t hist88[62];                      /* the shared decimator's state */
static uint8_t hist_live;                       /* hist88 holds something not yet flushed */

/* main thread, the interrupt not playing: every pattern head placed at global frame g */
void py_pat_start(uint32_t g)
{
    int t, i;

    for (t = 0; t < PS_TRACKS; t++) {
        heads[t].active = 0;
        if (py_song_is_pattern(&py_song, t)) {
            py_song_start(&py_song, t, 0, 0, clock_bar, pat_len, 0, &heads[t]);
            py_song_seek(&py_song, t, g, clock_bar, pat_len, 0, &heads[t]);
        }
        for (i = 0; i < PY_DVOICES; i++)
            drums[t].v[i].on = 0;
        py_syn_reset(&syns[t]);
    }
    for (i = 0; i < 62; i++)
        hist88[i] = 0;
    hist_live = 0;
}

/* ------------------------------------------------------------------ audio interrupt */

static int slot_of(int t, int synth)
{
    int s = py_song.trk[t].sound;

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

static void synth_emit(void *ctx, const nr_hit *h)
{
    const emit_ctx *c = ctx;
    int key = h->n->key + 12 * TE_SYNTH_OCTAVE[c->slot] + c->tsp;

    if (key < 0 || key > 127)
        return;
    py_syn_note(&syns[c->t], key, (h->n->vel * 127 + 50) / 100, h->hold);
    py_syn_notes++;
}

static const py_syn_api te_syn_api = {
    eh_render_inst, TE_ADSR_START, TE_ADSR_RELEASE, TE_ADSR_RENDER,
};

static int32_t bus88[128];                      /* non-ECO SYNTH tracks, gains applied, 88.2 kHz */
static int32_t eco44[PY_BLOCK];                 /* ECO SYNTH tracks + the decimated bus */
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
    c.slot = slot_of(t, synth);
    while (h->active && g < end && guard++ < 8) {
        uint32_t hend = h->start + h->len, n;

        if (g >= hend) {
            py_song_next(&py_song, t, bar, pat_len, 0, h);
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
        uint8_t eco = tr->eco ? 1 : 0;

        s->cap = (uint8_t)(tr->voices ? tr->voices : PY_SYN_CAP_DEFAULT);
        s->par = (uint8_t)(inst_of[t] & 1);    /* ECO tracks on instances 1 / 3 and 2: other blocks */
        if (s->eco != eco) {
            s->eco = eco;
            s->phase = 0;
        }
        if (!gain || !inst_of[t]) {
            py_syn_reset(s);
        } else if (py_syn_active(s) || s->phase) {
            peak_to(t, py_syn_block(s, &te_syn_api, inst_of[t], (const int16_t *)(TE_SYNTH_KNOBS + c.slot * 8),
                                    (const int16_t *)(TE_SYNTH_ADSR + (uint32_t)c.slot * 16u), c.slot,
                                    gain, bus88, eco44, pos / PY_BLOCK));
            bus = !eco;
            py_mx.ext_used = 1;
        }
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

void py_pat_block(uint32_t pos)
{
    int t, i, dp = -1, np = -1, bus = 0;
    uint32_t c0, c1;

    for (t = 0; t < PS_TRACKS; t++) {
        const ps_head *h = &heads[t];

        if (!py_song_is_pattern(&py_song, t))
            continue;
        c0 = cycles();
        bus |= track_block(t, pos);
        c1 = cycles();
        py_trk_cyc[t] += c1 - c0;
        if (h->active && h->pat < 14 && pos >= h->start) {
            if (py_song.trk[t].type == PS_DRUM && dp < 0) {
                dp = h->pat;
                dr_ext_clock = pos - h->start;
            } else if (py_song.trk[t].type == PS_SYNTH && np < 0) {
                np = h->pat;
                nr_ext_clock = pos - h->start;
            }
        }
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
    py_dec_cyc += cycles() - c0;
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
