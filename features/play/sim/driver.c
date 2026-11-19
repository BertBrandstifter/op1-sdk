/* play bench: the composed feature with TE stand-ins and a small in-memory yaffs. Checks:
 *  0 boot: no set folder: no tracks, the ADD row
 *  1 disk sync hook: play/ of image 1 copied into play_b (A1.WAV, A2.AIF, SET.TXT; not the
 *    hidden "._" file), ACTIVE marker; nested calls (depth 1) do nothing; TE's cleanup still runs
 *  2 the main loop loads it: 2 audio files in the pool, still no tracks
 *  3 two AUDIO tracks added on the ADD row (white turn = type, white press = add; each gets the
 *    first unused file), the first switched to ONCE: playing adds exactly the expected samples
 *    into TE's master buffers (gain -9 dB, A2 looped at its own length, A1 silent after its end)
 *  4 stop pauses (nothing added), stop again: bar 1
 *  5 a second import (image 2: A1.WAV mono only): the tracks stay (SONG.BIN carried over), track 1
 *    plays the new A1, track 2 shows "A2.AIF: GONE"
 *  6 image 3 (A2.AIF only): track 1 "A1.WAV: GONE", track 2 plays
 *  7 keys: PLAY is the tape screen: tape again and shift + tape stay PLAY's (no TE tape screen,
 *    no erase screen); from another screen the tape key is TE's; a release whose press PLAY took
 *    is taken; knobs
 *  8 a DRUM track (DRUM ROLL pattern 1, hits on steps 1 and 9) on TE's kit, soloed: exactly the
 *    expected samples, the second hit inside a block; the pattern loops
 *  9 a SYNTH track (NOTE ROLL pattern 1, one note A4 for one step) on a test engine registered as
 *    a multi-instance engine: rendered by instance 1 at A4's increment, TE's envelope stand-ins
 *    started / released after the hold, the decimated level as expected, silent after the release
 * 10 the song is saved to SONG.BIN and read back when the set is re-read (as at boot)
 * 11 every buffer handed to yaffs starts at an even address (TE's NAND driver asserts
 *    te_yflash2.c:600 otherwise: the cause of a boot failure on 2026-11-15)
 * 12 cut twice removes a track
 * 13 insert twice adds a track before the cursor's (its type picked with white while armed); the
 *    other tracks keep their order and patterns; any other key in between cancels
 * 14 shift + blue press on a DRUM track opens DRUM ROLL over PLAY on its pattern and it draws
 *    (also with TE's keyboard in synth mode), its playhead follows PLAY, shift + blue press closes it
 * 16 TE's own finger patterns: not loaded before op1.db is open; then PLAY loads them once (TE's
 *    getter), and a DRUM track on a pattern DRUM ROLL never edited plays TE's hit
 * 17 the list's knobs hold at the ends; an empty column given its pattern while playing joins
 *    at the next bar
 * 15 white turn = sound, orange turn = pattern; SYNTH cost options: white press cycles the voices (2 -> 4 -> 1), shift + white press ECO: the
 *    engine renders every other block at twice the step, with EH_HALF and (light load) EH_BOOST;
 *    orange turn while playing: the row's pattern changes at once (pending until the playing one ends)
 *    on every render, neither after ECO off; muted: nothing renders; PLAY's CPU
 *    numbers (total, per track) appear while playing and clear on stop
 * 21 song view (join): white press on an empty cell = a new chain, white turn its number, shift +
 *    white press clones, cut / insert move a cell (a chain is not pasted into an AUDIO column),
 *    shift + insert / shift + cut twice insert / delete a song row; back on the list, a track with
 *    more than one song row no longer changes its pattern on orange turn
 * 18 play in the song view starts every column at the cursor row from bar 1; play again pauses;
 *    stop on a pause: bar 1 and song row 00 again
 * 19 chain view (white press on a chain cell): pattern, transpose (shift: octaves), last pattern,
 *    row clipboard, row insert / delete; play auditions the chain alone (the other tracks at
 *    gain 0, the head walks that chain, the drum hit plays); stop ends it; orange press: song view
 * 20 AUDIO cells ON / OF: the track follows the song (ON = the file once, OF = a bar of silence,
 *    then again): exactly the expected samples over two circles; without cells it plays free again
 * 22 speech: the help page of the view (drawn), blue turn pages, the next key only closes it;
 *    shift + speech is not PLAY's
 * 23 live: shift + play in the song view cues the cursor row: the column jumps there when its
 *    chain ends and loops back there; rec = live mode: white turn edits nothing, white press cues
 *    (shift + rec) the cursor cell (it does not open the chain), T1 mutes; an AUDIO column cued to another row
 *    changes at the end of its segment, sample by sample
 * 24 render (rec, white turn = bars, rec): the soloed AUDIO track for one bar into
 *    play_r/REND01.AIF: an AIFF header, stereo, exactly the expected samples (the master fader
 *    does not act), the file is then in the tracks' file pool; stop cancels a render and deletes
 *    its file; a soloed DRUM track renders to a mono file; keys do nothing while it renders
 * 25 a rest cell (OF, white turn below 00) in a DRUM column: one silent bar, then the column goes on
 * 26 keys 1-8 = the cursor track's sound / kit (list, song view); a chain row's SND column is the
 *    track's slot while the row plays; a key pressed meanwhile wins until the row ends
 * 27 up to 8 SYNTH tracks share the 5 engine instances: seven SYNTH tracks added on the ADD row;
 *    five taking turns (one note each, one bar after the other) every one plays, all on instance
 *    1 (each gives it back once silent); all seven at once: five play (instances 1-5), the other
 *    two drop their note and show BUSY; with PLAY's CPU over the guard (py_inst_cyc lowered) only
 *    three play, four show BUSY
 * 28 views (v0.8.2): join stays in the song view (plain and live), tape on PLAY: the list; tape
 *    from another screen: the list (not the view left there); join on another screen: taken, a
 *    tape press injected (TE_KEY_PUSH + wake), PLAY opens in the song view
 * 29 song slots (v0.10): shift + join opens the songs page (the headers read), rec twice saves slot
 *    01 (the song + all 28 patterns + swings); song, patterns and swings changed (one pattern TE's
 *    again); play twice asks (v0.11: the patterns that differ, NOTE ROLL 3 among them): join = not
 *    loaded; again, green right on every row (from the slot), left on the first (NOTE ROLL 3 kept), play:
 *    the song, every other pattern (now the editors') and the swings as saved, NOTE ROLL 3 as it
 *    was, the slot shown changed, the song view; cut twice deletes the slot
 * 30 files page (v0.11): shift + join twice (songs page, files page) lists the render folder;
 *    cut once asks, twice deletes the first file (REND01.AIF): gone from the folder and the list
 * 31 renders into disk mode (v0.11): the hook before TE's FatFs unmount on the way into disk
 *    mode (TE's FatFs stand-ins here): with a FATFS pointer nothing; with NULL every render of
 *    the folder in play_r/ byte for byte, every file closed, TE's unmount still called; a volume
 *    without room: skipped, nothing left behind
 * Returns failed-check bits (0 = pass). */
#include <stdarg.h>
#include <op1/te246.h>
#include "../py_core.h"
#include "../py_song.h"
#include "../../enginehost/eh_host.h"

extern void hook_key(int key, int down);
extern void hook_knob(int knob, int16_t *diff);
extern void hook_frame(uint16_t *buf);
extern void hook_volume(void);                  /* entry hook on 0x019de548 (TE's metronome mix) */
extern void hook_cleanup(const char *dir, int depth);  /* call site 0x01997584 */
extern void hook_disk_enter(void *vol, void *fs);       /* call site 0x019975fa */
extern void py_main(void);
extern py_mixer py_mx;
extern volatile uint32_t py_blocks, py_import_files, py_import_errors, py_import_runs;
extern char FB[], FB2[], FB3[], FB4[], FB5[], FB6[], FB7[];
extern int py_help_on(void), py_pat_cued(int t), py_rd_busy(void);
extern char FB8[], FB9[], FB10[], FB11[], FB12[];
extern int py_pat_slot(int t);
extern int py_pat_starved(int t);
extern volatile uint32_t py_boosts, py_inst_cyc;
extern int py_list_pattern(int t);
#define PY_PAT_PENDING  0x100                   /* (play.h) */
extern void py_update_gains(void), py_pat_set_start(int row);
extern volatile uint8_t py_view;                /* 0 list, 1 song view, 2 chain view */
extern const ps_head *py_pat_head(int t);
extern int py_pat_start_row(void), py_pat_audition_track(void);
extern uint8_t dr_pat[];                        /* DRUM ROLL: dr_pattern[14], 3648 bytes each */
extern volatile uint16_t dr_owned;
extern volatile uint8_t dr_loaded;
extern uint8_t nr_pat[];                        /* NOTE ROLL: nr_pattern[14], 1796 bytes each */
extern volatile uint16_t nr_owned;
extern volatile uint8_t nr_loaded;
extern volatile uint32_t py_drum_hits, py_syn_notes;
extern volatile int8_t dr_req_open, dr_ext_pat;
extern volatile uint32_t dr_ext_clock;
extern volatile uint8_t dr_open, dr_cur, dr_via_play;
extern ps_song py_song;
extern volatile uint16_t py_cpu_trk[8], py_cpu_all;
extern void eh_register_multi(int tag, void (*render)(int, int16_t *, const int16_t *, const uint32_t *, int));
extern uint8_t IMG1[], IMG1_END[], IMG2[], IMG2_END[], IMG3[], IMG3_END[];
extern uint8_t IMG4[], IMG4_END[];              /* play_s/SONG01.PSL: the driver writes the file in */
extern uint8_t IMG4_FAT[], IMG4_FATSZ[], IMG4_CL[], IMG4_NCL[], IMG4_DATA[], IMG4_SIZE[];  /* (values) */
extern volatile uint32_t py_export_songs, py_songs_imported, py_songs_left;
extern uint8_t py_snd_why[8];

#define SND_TYPE    ((volatile int32_t *)0xff8016e4)   /* TE's synth sounds: engine type, knobs, */
#define SND_KNOBS   ((volatile int16_t *)0xff8013c4)   /* envelope, octave */
#define SND_ADSR    ((volatile int16_t *)0x01b60ef0)
#define SND_OCT     ((volatile int32_t *)0x01dfe4e0)

#define E34     ((volatile int16_t *)0xff800d34)  /* TE's master bus L / R (int16) */
#define F34     ((volatile int16_t *)0xff800db4)

/* what PLAY adds to the bus for an output word w (PLAY's int32 sum, sample in the top half) */
static int16_t bus_of(int32_t w)
{
    int32_t v = ((w >> 16) * 12409) >> 15;

    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}
volatile int fails;
volatile int32_t dbg[8];                        /* first mismatch: block, frame, got L, want L, got R, want R, pos */

/* ------------------------------------------------------------------ in-memory yaffs */

#define NF      12
#define ARENA   ((uint8_t *)0x03100000)         /* file data: 1 MB per entry (to 0x03d00000) */
#define SLOT    (1u << 20)
typedef struct mfile { char path[64]; uint8_t used, dir; uint32_t size; uint8_t *data; } mfile;
static mfile fs[NF];
typedef struct mfd { int f; uint32_t pos; uint8_t open, write; } mfd;
static mfd fdt[16];
static int image;                               /* index of tape_c.raw */
static int te_cleanups, keys_to_te, knobs_to_te;

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void scpy(char *d, const char *s)
{
    while ((*d++ = *s++) != 0)
        ;
}

static int find(const char *path)
{
    int i;

    for (i = 0; i < NF; i++)
        if (fs[i].used && streq(fs[i].path, path))
            return i;
    return -1;
}

static int add(const char *path, int dir)
{
    int i;

    for (i = 0; i < NF; i++)
        if (!fs[i].used) {
            fs[i].used = 1;
            fs[i].dir = (uint8_t)dir;
            fs[i].size = 0;
            fs[i].data = ARENA + (uint32_t)i * SLOT;
            scpy(fs[i].path, path);
            return i;
        }
    return -1;
}

/* the parent folder of path is dir */
static int in_dir(const char *path, const char *dir)
{
    int n = 0;

    while (dir[n] && path[n] == dir[n])
        n++;
    if (dir[n] || path[n] != '/')
        return 0;
    for (n++; path[n]; n++)
        if (path[n] == '/')
            return 0;
    return 1;
}

static int parent_exists(const char *path)
{
    char d[64];
    int i, last = -1;

    for (i = 0; path[i]; i++) {
        d[i] = path[i];
        if (path[i] == '/')
            last = i;
    }
    if (last <= 0)
        return 0;
    d[last] = 0;
    return streq(d, "/yaffs2/user") || find(d) >= 0;
}

int file_open(const char *path, int oflag, int mode)
{
    int f = find(path), d;

    (void)mode;
    if (oflag == TE_FILE_CREATE) {
        if (f < 0) {
            if (!parent_exists(path))
                return -1;
            f = add(path, 0);
        }
        if (f < 0 || fs[f].dir)
            return -1;
        fs[f].size = 0;
    } else if (f < 0 || fs[f].dir) {
        return -1;
    }
    for (d = 3; d < 16; d++)
        if (!fdt[d].open) {
            fdt[d].f = f;
            fdt[d].pos = 0;
            fdt[d].open = 1;
            fdt[d].write = oflag == TE_FILE_CREATE;
            return d;
        }
    return -1;
}

int file_close(int fd)
{
    if (fd < 3 || fd >= 16 || !fdt[fd].open)
        return -1;
    fdt[fd].open = 0;
    return 0;
}

static void mcpy(uint8_t *d, const uint8_t *s, uint32_t n)
{
    while (n--)
        *d++ = *s++;
}

volatile int odd_io;                            /* yaffs buffers at odd addresses (TE asserts) */

int file_pread(int fd, void *buf, uint32_t n, int32_t off)
{
    mfile *m;

    odd_io += (uint32_t)buf & 1;
    if (fd < 3 || fd >= 16 || !fdt[fd].open || off < 0)
        return -1;
    m = &fs[fdt[fd].f];
    if ((uint32_t)off >= m->size)
        return 0;
    if (n > m->size - (uint32_t)off)
        n = m->size - (uint32_t)off;
    mcpy(buf, m->data + off, n);
    return (int)n;
}

int file_write(int fd, const void *buf, uint32_t n)
{
    mfile *m;

    odd_io += (uint32_t)buf & 1;
    if (fd < 3 || fd >= 16 || !fdt[fd].open || !fdt[fd].write)
        return -1;
    m = &fs[fdt[fd].f];
    if (fdt[fd].pos + n > SLOT)
        return -1;
    mcpy(m->data + fdt[fd].pos, buf, n);
    fdt[fd].pos += n;
    if (fdt[fd].pos > m->size)
        m->size = fdt[fd].pos;
    return (int)n;
}

int file_stat(const char *path, uint32_t *st)
{
    int f = find(path);

    if (f < 0)
        return -1;
    st[0x08 / 4] = fs[f].dir ? 0x4000 : 0x8000;
    st[0x1c / 4] = fs[f].size;
    return 0;
}

int file_mkdir(const char *path, int mode)
{
    (void)mode;
    if (find(path) >= 0 || !parent_exists(path))
        return -1;
    return add(path, 1) >= 0 ? 0 : -1;
}

int file_unlink(const char *path)
{
    int f = find(path);

    if (f < 0 || fs[f].dir)
        return -1;
    fs[f].used = 0;
    return 0;
}

int file_rmdir(const char *path) { (void)path; return -1; }

typedef struct mdir { char path[64]; int at; uint8_t open; uint8_t de[11 + 64]; } mdir;
static mdir dirs[4];

void *dir_open(const char *path)
{
    int i, f = find(path);

    if (f < 0 || !fs[f].dir)
        return 0;
    for (i = 0; i < 4; i++)
        if (!dirs[i].open) {
            dirs[i].open = 1;
            dirs[i].at = 0;
            scpy(dirs[i].path, path);
            return &dirs[i];
        }
    return 0;
}

uint8_t *dir_read(void *h)
{
    mdir *d = h;

    for (; d->at < NF; d->at++) {
        mfile *m = &fs[d->at];

        if (m->used && in_dir(m->path, d->path)) {
            const char *slash = m->path;
            const char *p;

            for (p = m->path; *p; p++)
                if (*p == '/')
                    slash = p;
            scpy((char *)d->de + 11, slash + 1);
            d->at++;
            return d->de;
        }
    }
    return 0;
}

int dir_close(void *h)
{
    ((mdir *)h)->open = 0;
    return 0;
}

/* TE's snprintf: %s %d %i %u (what PLAY uses) */
int te_snprintf(char *buf, uint32_t n, const char *fmt, ...)
{
    va_list ap;
    uint32_t o = 0;

    va_start(ap, fmt);
    for (; *fmt && o + 1 < n; fmt++) {
        if (*fmt != '%') {
            buf[o++] = *fmt;
            continue;
        }
        fmt++;
        if (*fmt == 's') {
            const char *s = va_arg(ap, const char *);

            while (*s && o + 1 < n)
                buf[o++] = *s++;
        } else if (*fmt == 'd' || *fmt == 'i' || *fmt == 'u') {
            char t[12];
            int v = va_arg(ap, int), k = 0;
            uint32_t u = (*fmt != 'u' && v < 0) ? (uint32_t)-v : (uint32_t)v;

            if (*fmt != 'u' && v < 0)
                buf[o++] = '-';
            do {
                t[k++] = (char)('0' + u % 10);
                u /= 10;
            } while (u);
            while (k && o + 1 < n)
                buf[o++] = t[--k];
        }
    }
    buf[o] = 0;
    va_end(ap);
    return (int)o;
}

/* sample heap: 1 MB, a block above the highest live one */
#define HEAP        0x03e00000u
#define HEAP_SIZE   (1u << 20)
static uint32_t live_at[32], live_n[32];
static int nlive;

void *heap_malloc(int heap, uint32_t size)
{
    uint32_t top = HEAP;
    int i;

    if (heap != 1 || nlive == 32)
        return 0;
    for (i = 0; i < nlive; i++)
        if (live_at[i] + live_n[i] > top)
            top = live_at[i] + live_n[i];
    size = (size + 15) & ~15u;
    if (size > HEAP + HEAP_SIZE - top)
        return 0;
    live_at[nlive] = top;
    live_n[nlive++] = size;
    return (void *)top;
}

void heap_free(void *p)
{
    int i;

    for (i = 0; i < nlive; i++)
        if (live_at[i] == (uint32_t)p) {
            live_at[i] = live_at[nlive - 1];
            live_n[i] = live_n[--nlive];
            return;
        }
}

void te_cleanup(const char *dir, int depth) { (void)dir; (void)depth; te_cleanups++; }
void console_print(const char *s) { (void)s; }

/* TE's patch setters (JSON + save): counted (check 29) */
static int set_knobs_n, set_adsr_n, set_oct_n;
void set_knobs_stub(void *patch, int16_t *k) { (void)patch; (void)k; set_knobs_n++; }
void set_adsr_stub(void *patch, int16_t *a) { (void)patch; (void)a; set_adsr_n++; }
void set_octave_stub(void *patch, int o) { (void)patch; (void)o; set_oct_n++; }

/* TE's FatFs inside disk mode's volume (check 31): the files written, as lengths + hashes */
#define VF 8
static struct { char path[24]; uint32_t len, hash; uint8_t used; } vf[VF];
static int vf_cur = -1, vf_mkdirs, vf_mounts, vf_open_now;
static uint32_t vf_free = 100u << 20;
static int32_t yf_free = 200 << 20;

int f_open_stub(void *fil, const char *path, int mode)
{
    int i, e = -1;

    (void)fil;
    if (mode != 0x0a)
        return 9;
    for (i = 0; i < VF; i++)
        if (vf[i].used && streq(vf[i].path, path))
            e = i;
    for (i = 0; e < 0 && i < VF; i++)
        if (!vf[i].used)
            e = i;
    if (e < 0)
        return 1;
    vf[e].used = 1;
    scpy(vf[e].path, path);
    vf[e].len = vf[e].hash = 0;
    vf_cur = e;
    vf_open_now++;
    return 0;
}

int f_write_stub(void *fil, const void *buf, uint32_t n, uint32_t *bw)
{
    const uint8_t *b = buf;
    uint32_t i;

    (void)fil;
    if (vf_cur < 0)
        return 9;
    if (n > vf_free)
        n = vf_free;
    for (i = 0; i < n; i++)
        vf[vf_cur].hash = vf[vf_cur].hash * 31u + b[i];
    vf[vf_cur].len += n;
    vf_free -= n;
    *bw = n;
    return 0;
}

int f_close_stub(void *fil) { (void)fil; vf_cur = -1; vf_open_now--; return 0; }
int f_mkdir_stub(const char *p) { (void)p; vf_mkdirs++; return 0; }
uint32_t fat_free_stub(void) { return vf_free; }
int32_t yaffs_free_stub(const char *p) { (void)p; return yf_free; }
int f_mount_stub(void *v, void *f) { (void)v; (void)f; vf_mounts++; return 0; }

int f_unlink_stub(const char *path)
{
    int i;

    for (i = 0; i < VF; i++)
        if (vf[i].used && streq(vf[i].path, path))
            vf[i].used = 0;
    return 0;
}
void log_key(int key, int down) { (void)key; (void)down; keys_to_te++; }
void log_knob(int knob, int16_t *diff) { (void)knob; (void)diff; knobs_to_te++; }
void nop(void) {}
static uint16_t pushed[16];
static int npushed, wakes;
/* op1.db (0 until the driver opens it) and TE's finger-sequencer getter (it loads okseq: here one
 * TE drum pattern 0 with a hit on step 1, the rest empty) */
static int db_open_now, fs_gets;
static uint32_t db_dummy;

void *db_op1(void) { return db_open_now ? &db_dummy : 0; }

void *fs_get(void)
{
    int p, s, h;

    fs_gets++;
    for (p = 0; p < 14; p++) {
        *(volatile int32_t *)(0x01ddb520 + 4 * p) = 16;                 /* drum lengths */
        *(volatile int32_t *)(0x01dda6cc + 4 * p) = 16;                 /* synth lengths */
        for (s = 0; s < 32; s++)
            for (h = 0; h < 2; h++) {
                *(volatile int32_t *)(0x01dda720 + p * 0x100 + s * 8 + h * 4) = -1;
                *(volatile int32_t *)(0x01dd98cc + p * 0x100 + s * 8 + h * 4) = -1;
            }
    }
    *(volatile int32_t *)(0x01dda720 + 3 * 0x100) = 53;                 /* TE's drum pattern 4: step 1, key 0 */
    *(volatile uint8_t *)0x01ddb564 = 1;
    return 0;
}

void key_push(uint16_t ev) { if (npushed < 16) pushed[npushed++] = ev; }
void key_wake(void) { wakes++; }

/* ------------------------------------------------------------------ the sets' samples (run_playtest.py) */

static int16_t a1s(uint32_t i, int c) { return (int16_t)(((i * (c ? 29u : 13u)) & 0x3fffu) - 0x2000); }
static int16_t a2s(uint32_t i) { return (int16_t)(((i * 7u) & 0x1fffu) - 0x1000); }

static int32_t soft(int32_t x)
{
    int32_t a = x < 0 ? -x : x;

    if (a > 24576) {
        a = 24576 + ((a - 24576) >> 2);
        if (a > 32767)
            a = 32767;
    }
    return x < 0 ? -a : a;
}

/* the words the hook must add at global frame g (A1.WAV once: stereo 176400 frames; A2.AIF looped:
 * mono big-endian 88200 frames) */
static int32_t expect1(uint32_t g, int c)
{
    int32_t g9 = 11627, mv = (32767 * 32767) >> 15;
    int32_t acc = (g < 176400u ? (a1s(g, c) * g9) >> 15 : 0) + ((a2s(g % 88200u) * g9) >> 15);

    return (int32_t)((uint32_t)soft((acc * mv) >> 15) << 16);
}

static void press(int key)
{
    hook_key(key, 1);
    hook_key(key, 0);
}

static void shift_press(int key)
{
    *TE_SHIFT_HELD = 1;
    press(key);
    *TE_SHIFT_HELD = 0;
}

static void set_image(uint8_t *b, uint8_t *e)
{
    mcpy(fs[image].data, b, (uint32_t)(e - b));  /* tape_c.raw: its own 3 MB at 0x02200000 */
    fs[image].size = (uint32_t)(e - b);
}

/* image 4: play_s/SONG01.PSL = d (n bytes): data, size, FAT chain (both FATs) */
static uint32_t song_hash;

static void song_image(const uint8_t *d, uint32_t n)
{
    uint32_t cl0 = (uint32_t)IMG4_CL, ncl = (uint32_t)IMG4_NCL, need = (n + 511) / 512, c, f, v, j;
    uint8_t *sz = IMG4 + (uint32_t)IMG4_SIZE;

    mcpy(IMG4 + (uint32_t)IMG4_DATA, d, n);
    for (j = 0, song_hash = 0; j < n; j++)
        song_hash = song_hash * 31u + d[j];
    sz[0] = (uint8_t)n, sz[1] = (uint8_t)(n >> 8), sz[2] = (uint8_t)(n >> 16), sz[3] = (uint8_t)(n >> 24);
    for (f = 0; f < 2; f++)
        for (c = 0; c < ncl; c++) {
            uint8_t *e = IMG4 + (uint32_t)IMG4_FAT + f * (uint32_t)IMG4_FATSZ + 2 * (cl0 + c);

            v = c + 1 < need ? cl0 + c + 1 : c + 1 == need ? 0xffff : 0;
            e[0] = (uint8_t)v, e[1] = (uint8_t)(v >> 8);
        }
}

static void clear_master(void)
{
    int i;

    for (i = 0; i < 64; i++)
        E34[i] = F34[i] = 0;
}

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)

/* ------------------------------------------------------------------ a test engine + TE's envelope */

static int eng_vi = -1, eng_calls, eng_insts;   /* eng_insts: bit i = instance i rendered */
static int eng_flags;                           /* the render flags seen (OR) */
static uint32_t eng_inc;
static int env_starts, env_releases, env_slot = -1;

static void test_engine(int vi, int16_t *out, const int16_t *knobs, const uint32_t *inc, int flags)
{
    int i;

    (void)knobs;
    (void)flags;
    eng_vi = vi;
    eng_insts |= 1 << (vi / 6);
    eng_flags |= flags;
    eng_inc = inc[0];
    eng_calls++;
    for (i = 0; i < 128; i++)
        out[i] = 1000;
}

void env_start(void *o) { ((uint32_t *)o)[0] = 1; env_starts++; }
void env_release(void *o, int slot) { ((uint32_t *)o)[0] = 10; env_releases++; env_slot = slot; }

int env_render(void *o, int16_t *env, const int16_t *adsr, const int16_t *vel)
{
    uint32_t *st = o;
    int i;

    (void)adsr;
    (void)vel;
    for (i = 0; i < 128; i++)
        env[i] = st[0] < 12 ? 32767 : 0;
    if (st[0] >= 10)
        st[0]++;
    return st[0] >= 12;
}

/* ------------------------------------------------------------------ helpers */

static void turn(int knob, int steps)
{
    int16_t d = steps > 0 ? 1 : -1;

    while (steps) {
        hook_knob(knob, &d);
        steps += steps > 0 ? -1 : 1;
    }
}

static int ntracks(void)
{
    int t, n = 0;

    for (t = 0; t < PS_TRACKS; t++)
        n += py_song.trk[t].type != PS_NONE;
    return n;
}

static void mains(int n)
{
    while (n--)
        py_main();
}

static int py_song_used0(void)                  /* column 1 has a cell */
{
    int r, n = 0;

    for (r = 0; r < PS_ROWS; r++)
        n += py_song.cell[r][0] != PS_EMPTY;
    return n;
}

static int streq0(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

int main(void)
{
    int i, b, bad;
    uint32_t st[32];

    *TE_EDIT_MODE = TE_EDIT_MODE_TAPE;
    *TE_BPM = 120.0f;
    image = add("/yaffs2/user/tape_c.raw", 0);
    fs[image].data = (uint8_t *)0x02200000;     /* its own 3 MB area */

    /* 0: boot without a set */
    py_main();
    CHECK(0, ntracks() == 0 && py_mx.set);

    /* 1: leaving disk mode with play/ (image 1) */
    set_image(IMG1, IMG1_END);
    hook_cleanup("/tape", 1);                   /* nested: nothing */
    CHECK(1, py_import_runs == 0 && te_cleanups == 1);
    hook_cleanup("", 0);
    CHECK(1, te_cleanups == 2 && py_import_runs == 1 && py_import_files == 3 && py_import_errors == 0);
    CHECK(1, file_stat("/yaffs2/user/play_b/A1.WAV", st) == 0 && st[7] == 176400u * 4 + 44
          && file_stat("/yaffs2/user/play_b/A2.AIF", st) == 0
          && file_stat("/yaffs2/user/play_b/SET.TXT", st) == 0
          && file_stat("/yaffs2/user/play_b/ACTIVE", st) == 0
          && file_stat("/yaffs2/user/play_b/_A1~1.WAV", st) < 0);

    /* 2: loaded: no tracks yet */
    py_main();
    CHECK(2, ntracks() == 0 && py_mx.set->bpm_x100 == 12000);

    /* 3: two AUDIO tracks from the ADD row; the first ONCE; play */
    turn(2, -1);                                /* ADD: SYNTH -> AUDIO */
    press(TE_KEY_WHITE_PUSH);                   /* track 1 = A1.WAV */
    turn(0, 1);                                 /* the ADD row */
    press(TE_KEY_WHITE_PUSH);                   /* track 2 = A2.AIF */
    turn(0, -1);
    press(TE_KEY_WHITE_PUSH);                   /* track 1: ONCE */
    py_main();
    CHECK(3, ntracks() == 2 && py_song.trk[0].type == PS_AUDIO && streq0(py_song.trk[0].file, "A1.WAV")
          && !py_song.trk[0].loop && streq0(py_song.trk[1].file, "A2.AIF") && py_song.trk[1].loop
          && py_mx.set->t[0].used && py_mx.set->t[1].used && py_mx.set->t[1].big_endian);
    press(TE_KEY_PLAY);
    py_main();
    CHECK(3, py_mx.playing);
    bad = 0;
    for (b = 0; b < 2900; b++) {                /* past A1's end at frame 176400 */
        uint32_t g0 = py_mx.pos;

        clear_master();
        hook_volume();
        if (b < 4 || (g0 <= 88200 && g0 + 64 > 88200) || (g0 <= 176400 && g0 + 64 > 176400) || b == 2899)
            for (i = 0; i < 64; i++)
                if (E34[i] != bus_of(expect1(g0 + (uint32_t)i, 0)) || F34[i] != bus_of(expect1(g0 + (uint32_t)i, 1))) {
                    if (!bad) {
                        dbg[0] = b; dbg[1] = i; dbg[2] = E34[i]; dbg[3] = expect1(g0 + (uint32_t)i, 0);
                        dbg[6] = (int32_t)g0;
                    }
                    bad = 1;
                }
        if (!(b % 4))
            py_main();
    }
    CHECK(3, !bad && py_mx.playing && py_mx.pos == 2900u * 64 && !py_mx.underruns[0] && !py_mx.underruns[1]
          && !py_mx.read_errors);

    /* 4: stop pauses, stop again: bar 1 */
    hook_frame((uint16_t *)FB);
    press(TE_KEY_STOP);
    py_main();
    clear_master();
    hook_volume();
    CHECK(4, !py_mx.playing && E34[0] == 0 && F34[63] == 0 && py_mx.pos == 2900u * 64);
    press(TE_KEY_STOP);
    py_main();
    CHECK(4, py_mx.pos == 0);
    mains(300);                                 /* SONG.BIN written */

    /* 5: image 2 (A1.WAV mono only): the tracks stay */
    set_image(IMG2, IMG2_END);
    hook_cleanup("", 0);
    py_main();
    CHECK(5, ntracks() == 2 && py_mx.set->t[0].used && py_mx.set->t[0].channels == 1 && !py_mx.set->t[1].used
          && streq0(py_mx.set->t[1].err, "A2.AIF: GONE")
          && file_stat("/yaffs2/user/play_a/SONG.BIN", st) == 0
          && file_stat("/yaffs2/user/play_b/A1.WAV", st) < 0);

    /* 6: image 3 (A2.AIF only) */
    set_image(IMG3, IMG3_END);
    hook_cleanup("", 0);
    py_main();
    CHECK(6, !py_mx.set->t[0].used && streq0(py_mx.set->t[0].err, "A1.WAV: GONE") && py_mx.set->t[1].used);
    hook_frame((uint16_t *)FB2);

    /* 7: keys */
    keys_to_te = 0;
    press(TE_KEY_INSERT);                       /* PLAY shown: kept from TE */
    CHECK(7, keys_to_te == 0);
    hook_key(TE_KEY_PLAY, 1);                   /* press on PLAY ... */
    press(TE_KEY_TAPE);                         /* tape again: still PLAY (TE's tape screen is gone) */
    hook_key(TE_KEY_PLAY, 0);                   /* ... and its release: taken */
    CHECK(7, keys_to_te == 0);
    press(TE_KEY_INSERT);                       /* PLAY's, not TE's */
    CHECK(7, keys_to_te == 0);
    *TE_SHIFT_HELD = 1;
    press(TE_KEY_TAPE);                         /* shift + tape: not to TE (its erase screen) */
    *TE_SHIFT_HELD = 0;
    CHECK(7, keys_to_te == 0);
    *TE_EDIT_MODE = 0;                          /* another screen: the tape key is TE's (it opens tape mode) */
    press(TE_KEY_TAPE);
    *TE_EDIT_MODE = TE_EDIT_MODE_TAPE;
    CHECK(7, keys_to_te == 2);
    py_main();                                  /* the play press above */
    if (py_mx.playing) {
        press(TE_KEY_STOP);
        py_main();
    }
    press(TE_KEY_STOP);
    py_main();
    {
        int16_t d = 1;

        knobs_to_te = 0;
        turn(0, -5);                            /* cursor: track 1 */
        hook_knob(1, &d);                       /* PLAY: level -9 -> -8 */
        *TE_EDIT_MODE = 0;                      /* synth screen: knobs to TE */
        hook_knob(1, &d);
        CHECK(7, knobs_to_te == 1 && py_song.trk[0].level == -8);
        *TE_EDIT_MODE = TE_EDIT_MODE_TAPE;
    }

    /* 8: a DRUM track */
    {
        static int16_t kit[8 + 2000];
        static void *sb[1], *holder[1];
        volatile uint32_t *holders = (uint32_t *)0x01cc10cc;
        volatile int16_t *keyblk = (int16_t *)(0x01aaadb4 + 2 * 0x180);   /* slot 2, key 0 */
        uint8_t *lane = dr_pat;                                         /* pattern 0, lane 0 */
        int st2, ok = 1;
        uint32_t g;

        for (i = 0; i < 2000; i++)
            kit[8 + i] = (int16_t)((i * 53) % 20000 - 10000);
        sb[0] = kit;
        holder[0] = sb;
        for (i = 0; i < 8; i++)
            holders[i] = (uint32_t)holder;
        keyblk[0] = 0;
        keyblk[1] = 0; keyblk[5] = 0;                               /* start frame 0 */
        keyblk[2] = 61; keyblk[6] = 0x75bf;                         /* end: frame 1000 */
        keyblk[3] = 8192;                                           /* one-shot */
        keyblk[4] = 8192;                                           /* forward */
        keyblk[7] = 8192;                                           /* volume unity */
        *(volatile int32_t *)0xff8016d8 = 2;                        /* TE's drum slot 3 */
        for (i = 0; i < 8; i++)
            *(volatile int32_t *)(0x01dfe500 + 4 * i) = 0;          /* octaves */
        for (i = 0; i < 8 * 456; i++)
            lane[i] = 0;
        for (i = 0; i < 8; i++) {                                   /* lanes: key i, 16 x 1/16, 100 % */
            lane[i * 456 + 0] = (uint8_t)i;
            lane[i * 456 + 1] = 16;
            lane[i * 456 + 2] = 3;
            lane[i * 456 + 3] = 100;
        }
        for (st2 = 0; st2 < 16; st2 += 8) {                         /* lane 0, steps 1 and 9 */
            uint8_t *sp = lane + 8 + st2 * 7;

            sp[0] = 1; sp[1] = 100; sp[2] = 0; sp[3] = 12; sp[4] = 0; sp[5] = 100; sp[6] = 1;
        }
        dr_owned = 1;
        dr_loaded = 1;
        turn(0, 5);                                                 /* the ADD row */
        turn(2, 2);                                                 /* AUDIO -> SYNTH -> DRUM */
        press(TE_KEY_WHITE_PUSH);                                   /* track 3 = DRUM */
                                                                    /* a new track starts on pattern 1 */
        press(TE_KEY_GREEN_PUSH);                                   /* solo */
        mains(30);                                                  /* published; tracks reopened */
        CHECK(8, ntracks() == 3 && py_song.trk[2].type == PS_DRUM && py_song.cell[0][2] < PS_CHAINS
              && py_song.chain[py_song.cell[0][2]][0].pat == 0 && py_song.trk[2].solo);
        press(TE_KEY_PLAY);
        py_main();
        CHECK(8, py_mx.playing && py_mx.pos == 0);
        for (b = 0; b < 1400 && ok; b++) {
            g = py_mx.pos;
            clear_master();
            hook_volume();
            for (i = 0; i < 64; i++) {
                uint32_t f = g + (uint32_t)i, at = f % 88200 >= 44100 ? f % 88200 - 44100 : f % 88200;
                int32_t want = 0;

                if (at < 1000) {
                    int32_t y = (kit[8 + at] * 20544) >> 15;            /* the voice */
                    int32_t v = ((y * 11627) >> 15) * 32766 >> 15;     /* -9 dB, master */

                    want = bus_of((int32_t)((uint32_t)soft(v) << 16));
                }
                if (E34[i] != want || F34[i] != want) {
                    dbg[0] = b; dbg[1] = i; dbg[2] = E34[i]; dbg[3] = want; dbg[6] = (int32_t)f;
                    ok = 0;
                    break;
                }
            }
            if (!(b % 4))
                py_main();
        }
        CHECK(8, ok && py_drum_hits == 3);
        press(TE_KEY_GREEN_PUSH);                                   /* unsolo */
        press(TE_KEY_STOP);
        py_main();
        press(TE_KEY_STOP);
        py_main();
    }

    /* 9: a SYNTH track on a test engine (tag 1 as a multi-instance engine) */
    {
        uint8_t *p = nr_pat;                                        /* NOTE ROLL pattern 0 */
        volatile int16_t *knobs = (int16_t *)(0xff8013c4 + 1 * 16); /* slot 2's stored knobs */
        int got = 0;

        eh_register_multi(1, test_engine);
        for (i = 0; i < 4; i++)
            knobs[4 + i] = (int16_t)(1 << 12);                      /* tag 1 in slots 5-8 */
        *(volatile int32_t *)0xff8016d4 = 1;                        /* TE's synth slot 2 */
        for (i = 0; i < 8; i++)
            *(volatile int32_t *)(0x01dfe4e0 + 4 * i) = 0;          /* octaves */
        for (i = 0; i < 1796; i++)
            p[i] = 0;
        p[0] = 16;                                                  /* 16 steps */
        for (i = 0; i < 64; i++) {
            uint8_t *sp = p + 4 + i * 28;
            int n;

            for (n = 0; n < 6; n++)
                sp[n * 4] = 0xff;                                   /* empty */
            sp[25] = 100;                                           /* prob */
            sp[26] = 1;                                             /* ratch */
        }
        p[4] = 69; p[5] = 100; p[6] = 4;                            /* step 1: A4, vel 100, 1 step */
        nr_owned = 1;
        nr_loaded = 1;
        turn(0, 5);                                                 /* the ADD row */
        turn(2, -1);                                                /* DRUM -> SYNTH */
        press(TE_KEY_WHITE_PUSH);                                   /* track 4 = SYNTH */
                                                                    /* a new track starts on pattern 1 */
        press(TE_KEY_GREEN_PUSH);                                   /* solo */
        mains(30);
        CHECK(9, ntracks() == 4 && py_song.trk[3].type == PS_SYNTH && py_song.cell[0][3] < PS_CHAINS
              && py_song.cell[0][3] != py_song.cell[0][2] && py_song.chain[py_song.cell[0][3]][0].pat == 0);
        press(TE_KEY_PLAY);
        py_main();
        for (b = 0; b < 120; b++) {
            clear_master();
            hook_volume();
            if (b == 4)
                got = E34[0];
            if (!(b % 4))
                py_main();
        }
        {
            int32_t want = bus_of((int32_t)((uint32_t)soft(((((999 * 32765) >> 15) * 11627) >> 15) * 32766 >> 15) << 16));

            CHECK(9, py_syn_notes == 1 && eng_vi == 6 && eng_inc == 21426141u && env_starts == 1
                  && env_releases == 1 && env_slot == 1);
            CHECK(9, got >= want - 2 && got <= want + 2);
            CHECK(9, E34[0] == 0);                                  /* released and done by now */
            dbg[4] = got; dbg[5] = want;
        }
        press(TE_KEY_STOP);
        py_main();
    }

    /* 10: SONG.BIN saved, read back when the set is re-read (as at boot) */
    mains(300);
    py_song.trk[3].type = PS_NONE;                                  /* forget in RAM ... */
    press(TE_KEY_ORANGE_PUSH);                                      /* ... re-read */
    py_main();
    CHECK(10, ntracks() == 4 && py_song.trk[3].type == PS_SYNTH && py_song.trk[2].type == PS_DRUM);
    hook_frame((uint16_t *)FB3);

    /* 12: cut twice removes the track under the cursor */
    turn(0, -9);
    press(TE_KEY_CUT);
    CHECK(12, ntracks() == 4);
    press(TE_KEY_CUT);
    CHECK(12, ntracks() == 3 && py_song.trk[0].type == PS_NONE);

    /* 13: insert twice on the first row: a new AUDIO track before it; the others keep their order
     * and the drum track its pattern */
    {
        int dc;

        press(TE_KEY_INSERT);                                       /* armed */
        CHECK(13, ntracks() == 3);
        turn(2, -1);                                                /* SYNTH -> AUDIO while armed */
        press(TE_KEY_INSERT);
        py_main();
        dc = py_song.cell[0][2];
        CHECK(13, ntracks() == 4 && py_song.trk[0].type == PS_AUDIO && py_song.trk[1].type == PS_AUDIO
              && streq0(py_song.trk[1].file, "A2.AIF") && py_song.trk[2].type == PS_DRUM
              && py_song.trk[3].type == PS_SYNTH && dc < PS_CHAINS && py_song.chain[dc][0].pat == 0);
        press(TE_KEY_INSERT);                                       /* armed ... */
        press(TE_KEY_PLAY);                                         /* ... another key: cancelled */
        press(TE_KEY_INSERT);
        CHECK(13, ntracks() == 4);
    }

    /* 14: shift + blue press on the DRUM track: DRUM ROLL opens over PLAY on its pattern (no
     * finger sequencer needed, no keys pressed for the user); PLAY stops drawing and leaves the
     * knobs to it; playing, its playhead follows PLAY's clock; shift + blue press closes it */
    {
        int r;

        for (r = 0; r < 4 && py_song.trk[r].type != PS_DRUM; r++)
            ;
        turn(0, -9);
        turn(0, r);                                                 /* cursor on the DRUM track */
        *(volatile int32_t *)0x01ab3148 = 0;                        /* drum sequencer: not finger */
        *TE_SHIFT_HELD = 1;
        press(TE_KEY_BLUE_PUSH);
        *TE_SHIFT_HELD = 0;
        CHECK(14, dr_open == 1 && dr_via_play == 1 && dr_cur == 0 && npushed == 0);
        {                                                           /* drawn although TE's keyboard is in synth mode */
            volatile uint16_t *scr = (uint16_t *)0x03090000;
            int px, drawn = 0;

            *(volatile int32_t *)0x01de4c40 = 0;                    /* TE_PLAYMODE: synth */
            for (px = 0; px < 320 * 256; px++)
                scr[px] = 0x1234;
            hook_frame((uint16_t *)scr);
            for (px = 0; px < 320 * 256; px++)
                drawn += scr[px] != 0x1234;
            CHECK(14, drawn > 320 * 100);                           /* DRUM ROLL's screen, not TE's tape */
        }
        {
            uint8_t before = py_song.trk[r].level;
            int16_t d = 1;

            hook_knob(1, &d);                                       /* the editor's knob, not PLAY's */
            CHECK(14, py_song.trk[r].level == (int8_t)before && knobs_to_te == 1);
        }
        press(TE_KEY_PLAY);
        py_main();
        for (b = 0; b < 100; b++)
            hook_volume();
        CHECK(14, py_mx.playing && dr_ext_pat == 0 && dr_ext_clock > 0);
        press(TE_KEY_STOP);
        py_main();
        hook_volume();
        CHECK(14, !py_mx.playing && dr_ext_pat == -1);
        *TE_SHIFT_HELD = 1;
        press(TE_KEY_BLUE_PUSH);                                    /* closes the editor */
        *TE_SHIFT_HELD = 0;
        CHECK(14, dr_open == 0);
        press(TE_KEY_STOP);
        py_main();
    }

    /* 15: SYNTH cost options */
    {
        int t, k, calls;
        uint32_t boosts;

        for (t = 0; t < PS_TRACKS && py_song.trk[t].type != PS_SYNTH; t++)
            ;
        for (k = 0; k < PS_TRACKS; k++)
            py_song.trk[k].solo = py_song.trk[k].mute = 0;
        turn(0, -9);
        turn(0, t);                                                 /* rows: index order */
        press(TE_KEY_GREEN_PUSH);                                   /* solo: gains anew */
        CHECK(15, t < PS_TRACKS && py_song.trk[t].solo && py_mx.gain[t] && py_song.trk[t].voices == 0);
        {                                                           /* white turn = sound, orange = pattern */
            uint8_t snd = py_song.trk[t].sound, pat = py_song.chain[py_song.cell[0][t]][0].pat;

            turn(2, 1);
            CHECK(15, py_song.trk[t].sound != snd && py_song.chain[py_song.cell[0][t]][0].pat == pat);
            turn(2, -1);
            turn(3, 1);
            CHECK(15, py_song.trk[t].sound == snd && py_song.chain[py_song.cell[0][t]][0].pat == pat + 1);
            turn(3, -1);
            CHECK(15, py_song.chain[py_song.cell[0][t]][0].pat == pat);
        }
        press(TE_KEY_WHITE_PUSH);
        CHECK(15, py_song.trk[t].voices == 4);
        press(TE_KEY_WHITE_PUSH);
        CHECK(15, py_song.trk[t].voices == 1);
        press(TE_KEY_WHITE_PUSH);
        CHECK(15, py_song.trk[t].voices == 2 && !py_song.trk[t].eco);
        *TE_SHIFT_HELD = 1;
        press(TE_KEY_WHITE_PUSH);
        *TE_SHIFT_HELD = 0;
        CHECK(15, py_song.trk[t].eco == 1);
        mains(4);
        press(TE_KEY_STOP);                                         /* bar 1 */
        py_main();
        press(TE_KEY_PLAY);
        py_main();
        calls = eng_calls;
        boosts = py_boosts;
        eng_flags = 0;
        for (b = 0; b < 40; b++)                                    /* inside the note (1 step) */
            hook_volume();
        CHECK(15, eng_calls - calls == 20 && eng_inc == 2 * 21426141u);
        CHECK(15, (eng_flags & (EH_HALF | EH_BOOST)) == (EH_HALF | EH_BOOST) && py_boosts - boosts == 20);
        *TE_SHIFT_HELD = 1;
        press(TE_KEY_WHITE_PUSH);                                   /* ECO off */
        *TE_SHIFT_HELD = 0;
        calls = eng_calls;
        boosts = py_boosts;
        eng_flags = 0;
        for (b = 0; b < 20; b++)
            hook_volume();
        CHECK(15, eng_calls - calls == 20 && eng_inc == 21426141u);
        CHECK(15, !(eng_flags & (EH_HALF | EH_BOOST)) && py_boosts == boosts);
        {                                                           /* orange turn while playing: the row */
            int was = py_list_pattern(t);                           /* shows the new pattern at once, */

            turn(3, 1);                                             /* pending until the playing one ends */
            CHECK(15, was >= 0 && !(was & PY_PAT_PENDING) && py_list_pattern(t) == ((was + 1) | PY_PAT_PENDING));
            turn(3, -1);
            CHECK(15, py_list_pattern(t) == was);
        }
        press(TE_KEY_BLUE_PUSH);                                    /* mute (solo wins: unsolo too) */
        press(TE_KEY_GREEN_PUSH);
        calls = eng_calls;
        for (b = 0; b < 20; b++)
            hook_volume();
        CHECK(15, py_song.trk[t].mute && !py_mx.gain[t] && eng_calls == calls);
        for (b = 0; b < 400; b++)
            hook_volume();
        CHECK(15, py_cpu_all > 0);
        dbg[4] = py_cpu_all; dbg[5] = py_cpu_trk[t];
        press(TE_KEY_STOP);
        py_main();
        hook_volume();
        CHECK(15, py_cpu_all == 0 && py_cpu_trk[t] == 0);
        press(TE_KEY_BLUE_PUSH);                                    /* unmute */
    }

    /* 16: TE's finger patterns: asked for once op1.db is open (the song has pattern tracks) */
    {
        int t, k;
        uint32_t hits;

        mains(10);
        CHECK(16, fs_gets == 0);                                    /* op1.db not open yet */
        *(void *volatile *)0x01bab718 = &db_dummy;                  /* TE's db semaphore exists */
        db_open_now = 1;
        mains(10);
        CHECK(16, fs_gets == 1 && *(volatile uint8_t *)0x01ddb564 == 1);
        for (t = 0; t < PS_TRACKS && py_song.trk[t].type != PS_DRUM; t++)
            ;
        for (k = 0; k < PS_TRACKS; k++)
            py_song.trk[k].solo = py_song.trk[k].mute = 0;
        dr_owned &= (uint16_t)~(1u << 3);                           /* pattern 4: TE's own */
        turn(0, -9);
        turn(0, t);
        turn(3, 3);                                                 /* orange: pattern 1 -> 4 */
        press(TE_KEY_GREEN_PUSH);                                   /* solo */
        mains(30);                                                  /* the copies refresh */
        CHECK(16, t < PS_TRACKS && py_song.chain[py_song.cell[0][t]][0].pat == 3);
        hits = py_drum_hits;
        press(TE_KEY_STOP);
        py_main();
        press(TE_KEY_STOP);
        py_main();
        press(TE_KEY_PLAY);
        py_main();
        for (b = 0; b < 10; b++)
            hook_volume();
        CHECK(16, py_drum_hits == hits + 1);
        press(TE_KEY_STOP);
        py_main();
        dbg[6] = fs_gets;
    }

    /* 17: the list's knobs hold at the ends (pattern 1..14, sound 1..8, no "--" / CUR); a column
     * that is empty at play (an older song) and gets its pattern while playing joins at the next
     * bar (TE's drum pattern 4 from check 16: one hit per bar) */
    {
        int t, c;
        uint32_t hits;

        for (t = 0; t < PS_TRACKS && py_song.trk[t].type != PS_DRUM; t++)
            ;
        turn(0, -9);
        turn(0, t);
        c = py_song.cell[0][t];
        turn(3, -20);
        CHECK(17, py_song.cell[0][t] == c && py_song.chain[c][0].pat == 0);
        turn(3, 30);
        CHECK(17, py_song.chain[c][0].pat == 13);
        turn(3, -10);                                               /* back to pattern 4 */
        turn(2, -20);
        CHECK(17, py_song.trk[t].sound == 0);
        turn(2, 20);
        CHECK(17, py_song.trk[t].sound == 7);
        turn(2, -7);
        CHECK(17, py_song.chain[c][0].pat == 3 && py_song.trk[t].sound == 0);
        press(TE_KEY_STOP);
        py_main();
        press(TE_KEY_STOP);
        py_main();
        py_song.cell[0][t] = PS_EMPTY;                              /* an empty column, as at play */
        press(TE_KEY_PLAY);
        py_main();
        hits = py_drum_hits;
        for (b = 0; b < 700; b++)
            hook_volume();
        py_song.cell[0][t] = (uint8_t)c;                            /* its pattern, while playing */
        for (b = 0; b < 700; b++)                                   /* to just past bar 2 (1378 blocks) */
            hook_volume();
        CHECK(17, py_drum_hits == hits + 1);
        press(TE_KEY_STOP);
        py_main();
    }

    /* 21: the song view */
    {
        int k, dc, nc, cl, pat;

        for (k = 0; k < PS_TRACKS; k++)
            py_song.trk[k].solo = py_song.trk[k].mute = 0;
        press(TE_KEY_STOP);
        py_main();
        press(TE_KEY_STOP);
        py_main();
        press(TE_KEY_JOIN);
        CHECK(21, py_view == 1 && py_song.trk[2].type == PS_DRUM && py_song.trk[0].type == PS_AUDIO);
        /* join stays in the song view (plain and live), tape: the list; on another screen tape
         * shows the list (not the view left), join injects a tape press and opens the song view */
        press(TE_KEY_JOIN);
        CHECK(28, py_view == 1);
        shift_press(TE_KEY_REC);                                    /* live */
        press(TE_KEY_JOIN);
        CHECK(28, py_view == 1);
        shift_press(TE_KEY_REC);
        press(TE_KEY_TAPE);
        CHECK(28, py_view == 0);
        press(TE_KEY_JOIN);
        *TE_EDIT_MODE = 0;                                          /* the synth screen */
        press(TE_KEY_TAPE);                                         /* TE opens the tape screen: PLAY's list */
        *TE_EDIT_MODE = TE_EDIT_MODE_TAPE;
        CHECK(28, py_view == 0);
        *TE_EDIT_MODE = 0;
        npushed = 0;
        wakes = 0;
        press(TE_KEY_JOIN);                                         /* taken: a tape press injected */
        CHECK(28, npushed == 2 && pushed[0] == (TE_KEY_TAPE | 1 << 8) && pushed[1] == TE_KEY_TAPE && wakes == 1);
        press(TE_KEY_TAPE);                                         /* (TE plays the injected press) */
        *TE_EDIT_MODE = TE_EDIT_MODE_TAPE;
        CHECK(28, py_view == 1);
        npushed = 0;
        turn(1, 2);                                                 /* column 3: the DRUM track */
        dc = py_song.cell[0][2];
        turn(0, 1);                                                 /* row 01 */
        press(TE_KEY_WHITE_PUSH);                                   /* empty, no last chain: a new one */
        nc = py_song.cell[1][2];
        CHECK(21, dc < PS_CHAINS && nc < PS_CHAINS - 1 && nc != dc && py_view == 1);
        turn(2, 1);
        CHECK(21, py_song.cell[1][2] == nc + 1);
        turn(2, -1);
        CHECK(21, py_song.cell[1][2] == nc);
        turn(0, -1);
        *TE_SHIFT_HELD = 1;
        press(TE_KEY_WHITE_PUSH);                                   /* row 00: clone its chain */
        *TE_SHIFT_HELD = 0;
        cl = py_song.cell[0][2];
        CHECK(21, cl < PS_CHAINS && cl != dc && cl != nc && py_song.chain[cl][0].pat == py_song.chain[dc][0].pat
              && py_view == 1);
        turn(0, 1);
        press(TE_KEY_CUT);
        CHECK(21, py_song.cell[1][2] == PS_EMPTY);
        turn(0, 1);
        press(TE_KEY_INSERT);
        CHECK(21, py_song.cell[2][2] == nc);
        turn(1, -2);
        press(TE_KEY_INSERT);                                       /* a chain into an AUDIO column: no */
        CHECK(21, py_song.cell[2][0] == PS_EMPTY);
        turn(1, 2);
        turn(0, -2);
        *TE_SHIFT_HELD = 1;
        press(TE_KEY_INSERT);                                       /* a new song row 00 */
        CHECK(21, py_song.cell[0][2] == PS_EMPTY && py_song.cell[1][2] == cl && py_song.cell[3][2] == nc);
        press(TE_KEY_CUT);                                          /* armed */
        CHECK(21, py_song.cell[1][2] == cl);
        press(TE_KEY_CUT);                                          /* deleted */
        *TE_SHIFT_HELD = 0;
        CHECK(21, py_song.cell[0][2] == cl && py_song.cell[2][2] == nc && py_song.cell[3][2] == PS_EMPTY);
        press(TE_KEY_TAPE);                                         /* the list: rows 00 + 02: no quick edit */
        pat = py_song.chain[cl][0].pat;
        turn(0, -9);
        turn(0, 2);
        turn(3, 1);
        turn(3, -2);
        CHECK(21, py_view == 0 && py_song.chain[cl][0].pat == pat && py_song.cell[0][2] == cl);
        press(TE_KEY_JOIN);

        /* 18: play from the cursor row */
        turn(0, 1);                                                 /* row 01 */
        press(TE_KEY_WHITE_PUSH);                                   /* the last chain here (nc) ... */
        turn(2, dc - nc);                                           /* ... made dc: 00 cl, 01 dc, 02 nc (no rows) */
        CHECK(18, py_song.cell[1][2] == dc);
        press(TE_KEY_PLAY);
        py_main();
        CHECK(18, py_mx.playing && py_mx.pos == 0 && py_pat_start_row() == 1);
        for (b = 0; b < 10; b++)
            hook_volume();
        CHECK(18, py_pat_head(2)->active && py_pat_head(2)->row == 1 && py_pat_head(2)->solo == PS_EMPTY);
        press(TE_KEY_PLAY);                                         /* pause */
        py_main();
        CHECK(18, !py_mx.playing && py_mx.pos == 640);
        press(TE_KEY_STOP);                                         /* bar 1, row 00 */
        py_main();
        CHECK(18, py_mx.pos == 0 && py_pat_start_row() == 0);

        /* 19: the chain view */
        {
            ps_crow *cr = py_song.chain[dc];
            uint32_t hits;

            press(TE_KEY_WHITE_PUSH);                               /* row 01's chain dc */
            CHECK(19, py_view == 2);
            turn(2, -20);
            CHECK(19, cr[0].pat == PS_EMPTY);
            turn(2, 1);                                             /* pattern 1 (DRUM ROLL's own) */
            turn(1, 1);
            turn(2, 2);
            CHECK(19, cr[0].pat == 0 && cr[0].tsp == 2);
            *TE_SHIFT_HELD = 1;
            turn(2, 1);
            CHECK(19, cr[0].tsp == 14);
            turn(2, -1);
            *TE_SHIFT_HELD = 0;
            turn(1, -1);
            turn(0, 1);
            press(TE_KEY_WHITE_PUSH);                               /* row 1: the last pattern */
            CHECK(19, cr[1].pat == 0 && cr[0].tsp == 2);
            press(TE_KEY_CUT);
            turn(0, 1);
            press(TE_KEY_INSERT);
            CHECK(19, cr[1].pat == PS_EMPTY && cr[2].pat == 0);
            *TE_SHIFT_HELD = 1;
            press(TE_KEY_INSERT);
            CHECK(19, cr[2].pat == PS_EMPTY && cr[3].pat == 0);
            press(TE_KEY_CUT);
            press(TE_KEY_CUT);
            *TE_SHIFT_HELD = 0;
            CHECK(19, cr[2].pat == 0 && cr[3].pat == PS_EMPTY);
            press(TE_KEY_CUT);                                      /* the chain: one row, pattern 1 +2 */
            CHECK(19, cr[2].pat == PS_EMPTY && cr[0].pat == 0);
            mains(30);
            hits = py_drum_hits;
            press(TE_KEY_PLAY);                                     /* audition */
            py_main();
            CHECK(19, py_mx.playing && py_pat_audition_track() == 2 && py_mx.gain[2] && !py_mx.gain[0]
                  && !py_mx.gain[1] && !py_mx.gain[3]);
            for (b = 0; b < 10; b++)
                hook_volume();
            CHECK(19, py_drum_hits == hits + 1 && py_pat_head(2)->active && py_pat_head(2)->solo == dc);
            hook_frame((uint16_t *)FB5);
            press(TE_KEY_STOP);
            py_main();
            CHECK(19, !py_mx.playing && py_mx.pos == 0 && py_pat_audition_track() < 0 && py_mx.gain[0]);
            press(TE_KEY_ORANGE_PUSH);
            CHECK(19, py_view == 1);
        }

        /* 20: AUDIO cells: track 1 (A2.AIF, mono, one bar) ON, OF, then again */
        {
            int32_t mv;
            uint32_t under, rerr;

            turn(1, -2);                                            /* column 1 */
            turn(0, -1);
            press(TE_KEY_WHITE_PUSH);                               /* row 00: ON */
            turn(0, 1);
            turn(2, 2);                                             /* row 01: empty -> ON -> OF */
            turn(0, -1);
            press(TE_KEY_GREEN_PUSH);                               /* solo */
            py_main();
            CHECK(20, py_song.cell[0][0] == PS_ON && py_song.cell[1][0] == PS_OF && py_mx.set->t[0].song
                  && py_mx.set->t[0].cur.n == 2 && py_mx.set->t[0].cur.period == 176400u && !py_mx.set->t[1].song);
            press(TE_KEY_PLAY);
            py_main();
            under = py_mx.underruns[0];
            rerr = py_mx.read_errors;
            mv = (py_mx.master * py_mx.te_volume) >> 15;
            bad = 0;
            for (b = 0; b < 4200; b++) {                            /* into the second circle's OF */
                uint32_t g0 = py_mx.pos, p0 = g0 % 176400u;

                clear_master();
                hook_volume();
                if (b < 4 || (p0 <= 88200 && p0 + 64 > 88200) || p0 + 64 > 176400 || p0 < 64 || b == 4199)
                    for (i = 0; i < 64; i++) {
                        uint32_t p = (g0 + (uint32_t)i) % 176400u;
                        int32_t acc = p < 88200u ? (a2s(p) * py_mx.gain[0]) >> 15 : 0;
                        int16_t want = bus_of((int32_t)((uint32_t)soft((acc * mv) >> 15) << 16));

                        if (E34[i] != want || F34[i] != want) {
                            if (!bad) {
                                dbg[0] = b; dbg[1] = i; dbg[2] = E34[i]; dbg[3] = want; dbg[6] = (int32_t)g0;
                            }
                            bad = 1;
                        }
                    }
                if (b == 100)
                    hook_frame((uint16_t *)FB4);
                if (!(b % 4))
                    py_main();
            }
            CHECK(20, !bad && py_mx.playing && py_mx.gain[0] && py_mx.underruns[0] == under && py_mx.read_errors == rerr);
            press(TE_KEY_STOP);
            py_main();
            press(TE_KEY_STOP);
            py_main();
            press(TE_KEY_CUT);                                      /* no cells: free again */
            turn(0, 1);
            press(TE_KEY_CUT);
            press(TE_KEY_GREEN_PUSH);
            py_main();
            CHECK(20, py_song.cell[0][0] == PS_EMPTY && py_song.cell[1][0] == PS_EMPTY && !py_mx.set->t[0].song);
            press(TE_KEY_TAPE);
            CHECK(20, py_view == 0);
        }

        /* 22: the help page */
        {
            int8_t lvl = py_song.trk[0].level;
            int te = keys_to_te;

            press(TE_KEY_SPEECH);
            CHECK(22, py_help_on() && keys_to_te == te);
            turn(1, 1);                                             /* not the level */
            turn(0, 1);                                             /* page 2 */
            hook_frame((uint16_t *)FB6);
            press(TE_KEY_JOIN);                                     /* closes it, nothing else */
            CHECK(22, !py_help_on() && py_view == 0 && py_song.trk[0].level == lvl);
            *TE_SHIFT_HELD = 1;
            press(TE_KEY_SPEECH);                                   /* shift + speech: not PLAY's */
            *TE_SHIFT_HELD = 0;
            CHECK(22, !py_help_on() && keys_to_te == te + 2);
            press(TE_KEY_JOIN);
            press(TE_KEY_SPEECH);                                   /* the song view's page, closed by speech */
            CHECK(22, py_view == 1 && py_help_on());
            press(TE_KEY_SPEECH);
            CHECK(22, py_view == 1 && !py_help_on());
        }

        /* 23: live. Column 3 (DRUM): row 00 chain cl, row 01 chain dc, both pattern 1 (one bar) */
        {
            uint32_t under, rerr, sw;
            int32_t mv;
            int reached = 0;

            py_song.chain[cl][0].pat = 0;
            mains(30);
            turn(1, 2);                                             /* column 3 */
            turn(0, -1);                                            /* row 00 */
            press(TE_KEY_PLAY);
            py_main();
            for (b = 0; b < 10; b++)
                hook_volume();
            CHECK(23, py_mx.playing && py_pat_head(2)->active && py_pat_head(2)->row == 0 && py_pat_cued(2) < 0);
            turn(0, 1);
            *TE_SHIFT_HELD = 1;
            press(TE_KEY_PLAY);                                     /* cue row 01 on every column */
            *TE_SHIFT_HELD = 0;
            CHECK(23, py_mx.playing && py_pat_cued(2) == 1 && py_pat_cued(3) == 1);
            hook_volume();
            CHECK(23, py_pat_head(2)->cue == 1 && py_pat_head(2)->row == 0 && py_pat_cued(2) == 1);
            for (b = 0; b < 1500 && !reached; b++) {                /* the chain (one bar) ends */
                hook_volume();
                reached = py_pat_head(2)->row == 1;
                if (!(b % 4))
                    py_main();
            }
            CHECK(23, reached && py_mx.pos >= 88200u && py_mx.pos < 88200u + 128 && py_pat_head(2)->start == 88200u
                  && py_pat_head(2)->start_row == 1 && py_pat_head(2)->cue == PS_EMPTY && py_pat_cued(2) < 0);
            shift_press(TE_KEY_REC);                                /* live mode */
            turn(2, 1);
            CHECK(23, py_song.cell[1][2] == dc);                    /* white turn: no edit */
            turn(0, -1);
            press(TE_KEY_WHITE_PUSH);                               /* cue the cell: not the chain view */
            CHECK(23, py_view == 1 && py_pat_cued(2) == 0 && py_pat_cued(3) < 0);
            press(TE_KEY_CUT);                                      /* no edit either */
            CHECK(23, py_song.cell[0][2] == cl);
            press(TE_KEY_T1);
            CHECK(23, py_song.trk[0].mute);
            press(TE_KEY_T1);
            CHECK(23, !py_song.trk[0].mute);
            hook_volume();
            hook_frame((uint16_t *)FB7);
            press(TE_KEY_STOP);
            py_main();
            press(TE_KEY_STOP);
            py_main();
            CHECK(23, !py_mx.playing && py_mx.pos == 0 && py_pat_cued(2) < 0);
            shift_press(TE_KEY_REC);                                /* editing again */

            /* AUDIO column 1: rows 00 ON, 01 ON, 03 OF; playing from 00, cued to 03 */
            turn(1, -2);
            press(TE_KEY_WHITE_PUSH);
            turn(0, 1);
            press(TE_KEY_WHITE_PUSH);
            turn(0, 2);
            press(TE_KEY_WHITE_PUSH);
            press(TE_KEY_WHITE_PUSH);
            press(TE_KEY_GREEN_PUSH);                               /* solo */
            turn(0, -3);
            CHECK(23, py_song.cell[0][0] == PS_ON && py_song.cell[1][0] == PS_ON && py_song.cell[2][0] == PS_EMPTY
                  && py_song.cell[3][0] == PS_OF);
            press(TE_KEY_PLAY);
            py_main();
            under = py_mx.underruns[0];
            rerr = py_mx.read_errors;
            CHECK(23, py_mx.playing && py_mx.set->t[0].cur.n == 2 && !py_mx.set->t[0].pending);
            shift_press(TE_KEY_REC);
            turn(0, 3);
            press(TE_KEY_WHITE_PUSH);                               /* cue row 03 */
            py_main();
            sw = py_mx.set->t[0].sw;
            CHECK(23, py_mx.set->t[0].pending && sw == 88200u && py_mx.set->t[0].nxt.row0 == 3
                  && py_mx.set->t[0].nxt.n == 1);
            mv = (py_mx.master * py_mx.te_volume) >> 15;
            bad = 0;
            for (b = 0; b < 1500; b++) {                            /* through the switch at 88200 */
                uint32_t g0 = py_mx.pos;

                clear_master();
                hook_volume();
                if (b < 4 || (g0 + 256 > 88200u && g0 < 88200u + 256) || b == 1499)
                    for (i = 0; i < 64; i++) {
                        uint32_t g = g0 + (uint32_t)i;
                        int32_t acc = g < 88200u ? (a2s(g) * py_mx.gain[0]) >> 15 : 0;
                        int16_t want = bus_of((int32_t)((uint32_t)soft((acc * mv) >> 15) << 16));

                        if (E34[i] != want || F34[i] != want) {
                            if (!bad) {
                                dbg[0] = b; dbg[1] = i; dbg[2] = E34[i]; dbg[3] = want; dbg[6] = (int32_t)g0;
                            }
                            bad = 1;
                        }
                    }
                if (!(b % 4))
                    py_main();
            }
            CHECK(23, !bad && py_mx.gain[0] && py_mx.underruns[0] == under && py_mx.read_errors == rerr);
            press(TE_KEY_STOP);
            py_main();
            press(TE_KEY_STOP);
            py_main();
            CHECK(23, !py_mx.set->t[0].pending && py_mx.set->t[0].cur.n == 2);    /* the song again */
            shift_press(TE_KEY_REC);
            press(TE_KEY_CUT);                                      /* row 03 */
            turn(0, -2);
            press(TE_KEY_CUT);
            turn(0, -1);
            press(TE_KEY_CUT);
            press(TE_KEY_GREEN_PUSH);
            py_main();
            CHECK(23, !py_song_used0());
            press(TE_KEY_TAPE);
            CHECK(23, py_view == 0);
        }

        /* 24: render */
        {
            static uint8_t rb[64 * 4] __attribute__((aligned(4)));
            static const uint32_t at[3] = { 0, 1000, 88200 - 64 };
            int32_t mv = (32767 * 32767) >> 15;
            int fd, n, a, lvl;

            press(TE_KEY_JOIN);                                     /* the song view, row 00 */
            turn(0, -9);
            turn(1, -9);
            turn(1, 1);                                             /* column 2: A2.AIF, free, looped */
            press(TE_KEY_GREEN_PUSH);                               /* solo */
            *TE_SHIFT_HELD = 1;
            turn(3, -3);                                            /* the master: not in a render */
            *TE_SHIFT_HELD = 0;
            press(TE_KEY_REC);                                      /* asks */
            CHECK(24, !py_rd_busy());
            turn(2, -20);                                           /* 1 bar */
            press(TE_KEY_REC);
            CHECK(24, py_rd_busy());
            lvl = py_song.trk[1].level;
            for (n = 0; n < 30000 && py_rd_busy(); n++) {
                py_main();
                hook_volume();
                if (n == 40) {
                    hook_frame((uint16_t *)FB8);
                    press(TE_KEY_BLUE_PUSH);                        /* keys: nothing while it renders */
                    turn(1, 3);
                }
            }
            py_main();
            py_main();
            CHECK(24, !py_rd_busy() && !py_mx.playing && py_mx.pos == 0 && py_song.trk[1].level == lvl
                  && !py_song.trk[1].mute);
            CHECK(24, file_stat("/yaffs2/user/play_r/REND01.AIF", st) == 0 && st[7] == 54u + 88200u * 4u);
            fd = file_open("/yaffs2/user/play_r/REND01.AIF", 0, 0x100);
            CHECK(24, fd >= 0 && file_pread(fd, rb, 54, 0) == 54 && rb[0] == 'F' && rb[8] == 'A' && rb[11] == 'F'
                  && rb[21] == 2 && rb[22] == 0 && rb[23] == 1 && rb[24] == 0x58 && rb[25] == 0x88 && rb[27] == 16
                  && rb[28] == 0x40 && rb[29] == 0x0e && rb[30] == 0xac && rb[31] == 0x44 && rb[38] == 'S');
            bad = 0;
            for (a = 0; a < 3 && fd >= 0; a++) {
                file_pread(fd, rb, 256, (int32_t)(54u + at[a] * 4u));
                for (i = 0; i < 64; i++) {
                    int32_t acc = (a2s(at[a] + (uint32_t)i) * py_mx.gain[1]) >> 15;
                    int16_t want = (int16_t)soft((acc * mv) >> 15);
                    int16_t gl = (int16_t)(rb[4 * i] << 8 | rb[4 * i + 1]), gr = (int16_t)(rb[4 * i + 2] << 8 | rb[4 * i + 3]);

                    if (gl != want || gr != want) {
                        if (!bad) {
                            dbg[0] = a; dbg[1] = i; dbg[2] = gl; dbg[3] = want;
                        }
                        bad = 1;
                    }
                }
            }
            if (fd >= 0)
                file_close(fd);
            CHECK(24, !bad && py_mx.gain[1]);
            press(TE_KEY_TAPE);                                     /* the list: the file is in the pool */
            turn(0, -9);
            for (n = 0; n < 4 && !streq0(py_song.trk[0].file, "REND01.AIF"); n++)
                turn(2, 1);
            py_main();
            CHECK(24, streq0(py_song.trk[0].file, "REND01.AIF") && py_mx.set->t[0].used
                  && py_mx.set->t[0].frames == 88200u && py_mx.set->t[0].channels == 2);
            for (n = 0; n < 4 && !streq0(py_song.trk[0].file, "A2.AIF"); n++)
                turn(2, 1);
            py_main();
            press(TE_KEY_JOIN);

            press(TE_KEY_REC);                                      /* a render cancelled with stop */
            press(TE_KEY_REC);
            py_main();
            for (n = 0; n < 5; n++) {
                hook_volume();
                py_main();
            }
            CHECK(24, py_rd_busy() && file_stat("/yaffs2/user/play_r/REND02.AIF", st) == 0);
            press(TE_KEY_STOP);
            py_main();
            CHECK(24, !py_rd_busy() && !py_mx.playing && file_stat("/yaffs2/user/play_r/REND02.AIF", st) < 0);
            press(TE_KEY_REC);                                      /* asked, then another key: nothing */
            press(TE_KEY_PLAY);
            py_main();
            CHECK(24, !py_rd_busy() && !py_mx.playing);

            py_song.trk[2].sound = 2;                               /* the kit of check 8 (slot 3) */
            press(TE_KEY_GREEN_PUSH);                               /* the DRUM track alone: a mono file */
            turn(1, 1);
            press(TE_KEY_GREEN_PUSH);
            press(TE_KEY_REC);
            turn(2, -20);
            press(TE_KEY_REC);
            for (n = 0; n < 30000 && py_rd_busy(); n++) {
                py_main();
                hook_volume();
            }
            py_main();
            py_main();
            CHECK(24, !py_rd_busy() && file_stat("/yaffs2/user/play_r/REND02.AIF", st) == 0
                  && st[7] == 54u + 88200u * 2u);
            fd = file_open("/yaffs2/user/play_r/REND02.AIF", 0, 0x100);
            n = 0;
            if (fd >= 0) {
                for (a = 0; a < 64; a++) {                          /* the first 8192 frames */
                    file_pread(fd, rb, 256, 54 + 256 * a);
                    for (i = 0; i < 256; i++)
                        n += rb[i] != 0;
                }
                file_pread(fd, rb, 54, 0);
                file_close(fd);
            }
            dbg[5] = n;
            CHECK(24, rb[21] == 1 && rb[22] == 0 && rb[23] == 1 && rb[24] == 0x58 && rb[25] == 0x88);   /* mono, 88200 */
            CHECK(24, n > 16);                                      /* the hit on step 1 */
            press(TE_KEY_GREEN_PUSH);
            *TE_SHIFT_HELD = 1;
            turn(3, 3);
            *TE_SHIFT_HELD = 0;
            press(TE_KEY_TAPE);
            CHECK(24, py_view == 0);
        }

        /* 25: a rest cell (OF) in the DRUM column: white turn below 00; the column plays its
         * chain (one bar), a silent bar, then starts again: one hit in two bars and a block */
        {
            uint32_t hits;
            uint8_t c0 = py_song.cell[0][2], c1 = py_song.cell[1][2];

            press(TE_KEY_JOIN);
            turn(0, -9);
            turn(1, -9);
            turn(1, 2);                                             /* column 3 */
            turn(0, 1);                                             /* row 01: chain dc */
            turn(2, -200);
            CHECK(25, py_song.cell[1][2] == PS_EMPTY);
            turn(2, 1);
            CHECK(25, py_song.cell[1][2] == PS_REST);
            turn(2, 1);
            CHECK(25, py_song.cell[1][2] == 0);
            turn(2, -1);
            turn(0, 1);
            press(TE_KEY_CUT);                                      /* row 02 empty: 00 chain, 01 rest */
            turn(0, -2);
            mains(30);
            hits = py_drum_hits;
            press(TE_KEY_PLAY);
            py_main();
            for (b = 0; b < 1379 + 700; b++) {                      /* into the rest's bar */
                hook_volume();
                if (!(b % 4))
                    py_main();
            }
            CHECK(25, py_pat_head(2)->active && py_pat_head(2)->row == 1 && py_pat_head(2)->pat == PS_EMPTY
                  && py_pat_head(2)->start == 88200u && py_pat_head(2)->len == 88200u);
            for (b = 0; b < 700; b++) {                             /* past it: row 00 again */
                hook_volume();
                if (!(b % 4))
                    py_main();
            }
            CHECK(25, py_pat_head(2)->row == 0 && py_pat_head(2)->start == 176400u
                  && py_drum_hits == hits + 2 + 1);                 /* steps 1 and 9, then step 1 again */
            press(TE_KEY_STOP);
            py_main();
            press(TE_KEY_STOP);
            py_main();
            py_song.cell[0][2] = c0;
            py_song.cell[1][2] = c1;
            press(TE_KEY_TAPE);
            CHECK(25, py_view == 0);
        }

        /* 26: sounds. Keys 1-8 set the cursor track's sound (list, song view); a chain row's SND
         * (chain view, green to the third column, white turn) is the track's slot while that row
         * plays; a key pressed meanwhile wins until the row ends */
        {
            uint8_t ch = py_song.cell[0][2];
            ps_crow keep = py_song.chain[ch][0];

            turn(0, -9);
            turn(0, 2);                                             /* the list: the DRUM track */
            press(TE_KEY_SOUND5);
            CHECK(26, py_song.trk[2].sound == 4 && py_pat_slot(2) == 4);
            press(TE_KEY_JOIN);
            turn(0, -9);
            turn(1, -9);
            turn(1, 2);
            press(TE_KEY_SOUND3);                                   /* the song view */
            CHECK(26, py_song.trk[2].sound == 2 && py_view == 1);
            press(TE_KEY_WHITE_PUSH);                               /* row 00's chain */
            CHECK(26, py_view == 2 && py_song.chain[ch][0].snd == PS_EMPTY);
            turn(1, 5);                                             /* SND */
            turn(2, 7);                                             /* -, 1 .. 7 */
            CHECK(26, py_song.chain[ch][0].snd == 6 && py_song.chain[ch][0].pat == keep.pat
                  && py_song.chain[ch][0].tsp == keep.tsp);
            turn(2, 5);
            CHECK(26, py_song.chain[ch][0].snd == 7);
            turn(2, -1);
            press(TE_KEY_ORANGE_PUSH);
            press(TE_KEY_PLAY);
            py_main();
            for (b = 0; b < 10; b++)
                hook_volume();
            CHECK(26, py_mx.playing && py_pat_slot(2) == 6 && py_song.trk[2].sound == 2);
            press(TE_KEY_SOUND2);                                   /* now, over the row's */
            hook_volume();
            CHECK(26, py_song.trk[2].sound == 1 && py_pat_slot(2) == 1);
            for (b = 0; b < 1400; b++) {                            /* row 01 (no SND): the track's */
                hook_volume();
                if (!(b % 4))
                    py_main();
            }
            CHECK(26, py_pat_head(2)->row == 1 && py_pat_slot(2) == 1);
            for (b = 0; b < 1400; b++) {                            /* row 00 again: its SND, not the key's */
                hook_volume();
                if (!(b % 4))
                    py_main();
            }
            CHECK(26, py_pat_head(2)->row == 0 && py_pat_slot(2) == 6);
            press(TE_KEY_STOP);
            py_main();
            press(TE_KEY_STOP);
            py_main();
            CHECK(26, py_pat_slot(2) == 6);                         /* stopped at bar 1: row 00's */
            py_song.chain[ch][0] = keep;
            py_song.trk[2].sound = 2;
            press(TE_KEY_TAPE);
            CHECK(26, py_view == 0);
        }
    }

    /* 27: seven SYNTH tracks on five engine instances */
    {
        uint8_t *p = nr_pat;                                        /* NOTE ROLL pattern 0: A4, one step */
        int t, r, pass, starts, inst_seen = 0, starved = 0;

        press(TE_KEY_STOP);
        py_main();
        press(TE_KEY_STOP);
        py_main();
        for (i = 0; i < 1796; i++)
            p[i] = 0;
        p[0] = 16;
        for (i = 0; i < 64; i++) {
            uint8_t *sp = p + 4 + i * 28;
            int n;

            for (n = 0; n < 6; n++)
                sp[n * 4] = 0xff;
            sp[25] = 100;
            sp[26] = 1;
        }
        p[4] = 69; p[5] = 100; p[6] = 4;
        nr_owned = 1;
        mains(30);                                                  /* the copy published */
        *(volatile int32_t *)0xff8016d4 = 1;                        /* TE's synth slot 2: the test engine */
        for (t = 0; t < PS_TRACKS; t++)
            py_song.trk[t].type = PS_NONE;
        py_main();
        turn(0, -9);
        turn(2, -9);
        turn(2, 1);                                                 /* AUDIO -> SYNTH */
        for (t = 0; t < 7; t++) {
            turn(0, 9);                                             /* the ADD row */
            press(TE_KEY_WHITE_PUSH);                               /* SYNTH stays the ADD row's type */
            mains(2);
        }
        CHECK(27, ntracks() == 7);
        for (t = 0; t < 7; t++) {
            ps_track *k = &py_song.trk[t];

            CHECK(27, k->type == PS_SYNTH);
            k->mute = k->solo = 0;
            k->voices = k->eco = 0;
            k->sound = 1;
            for (r = 0; r < PS_ROWS; r++)                           /* one bar each, in turn (tracks 0-4) */
                py_song.cell[r][t] = r < 5 && t < 5 ? (r == t ? 0x10 : PS_REST) : PS_EMPTY;
        }
        for (r = 0; r < PS_CROWS; r++) {
            py_song.chain[0x10][r].pat = r ? PS_EMPTY : 0;
            py_song.chain[0x10][r].tsp = 0;
            py_song.chain[0x10][r].snd = PS_EMPTY;
        }
        py_pat_set_start(0);
        py_update_gains();
        starts = env_starts;
        eng_insts = 0;
        press(TE_KEY_PLAY);
        py_main();
        for (b = 0; b < 5 * 1378 - 100; b++) {                      /* five bars (120 bpm), not the 6th's note */
            hook_volume();
            for (t = 0; t < 5; t++)
                starved |= py_pat_starved(t) << t;
            if (!(b % 4))
                py_main();
        }
        inst_seen = eng_insts;
        CHECK(27, env_starts - starts == 5 && inst_seen == 1 << 1 && starved == 0);
        dbg[6] = env_starts - starts; dbg[7] = inst_seen | starved << 8;
        press(TE_KEY_STOP);
        py_main();
        press(TE_KEY_STOP);
        py_main();
        for (t = 0; t < 7; t++)                                     /* all seven at once */
            for (r = 0; r < 5; r++)
                py_song.cell[r][t] = r ? PS_EMPTY : 0x10;
        for (pass = 0; pass < 2; pass++) {                          /* CPU fine, then over the guard */
            if (pass)
                py_inst_cyc = 0;
            starts = env_starts;
            eng_insts = starved = 0;
            press(TE_KEY_PLAY);
            py_main();
            for (b = 0; b < 200; b++) {
                hook_volume();
                if (!(b % 4))
                    py_main();
            }
            for (t = 0; t < 7; t++)
                starved |= py_pat_starved(t) << t;
            inst_seen = eng_insts;
            if (!pass)
                CHECK(27, env_starts - starts == 5 && inst_seen == (31 << 1) && starved == (3 << 5));
            else
                CHECK(27, env_starts - starts == 3 && inst_seen == (7 << 1) && starved == (15 << 3));
            dbg[4 + pass] = (int32_t)((env_starts - starts) | (uint32_t)inst_seen << 8 | (uint32_t)starved << 16);
            press(TE_KEY_STOP);
            py_main();
            press(TE_KEY_STOP);
            py_main();
        }
        py_inst_cyc = 250000u;
    }

    {   /* 29: song slots */
        static ps_song keep;
        extern volatile uint8_t nr_swing, dr_swing;
        extern int py_pat_slot_now(int *changed);
        uint32_t nsum = 0, dsum = 0, n2 = 0, d2 = 0, j;
        int same = 1, changed = 0, smask = 0, ns = 0, s0 = 8, snd_ok = 1;

        press(TE_KEY_STOP);
        py_main();
        press(TE_KEY_STOP);
        py_main();
        nr_owned = dr_owned = 0x3fff;                               /* every pattern the editors': its bytes play */
        nr_pat[2 * 1796] = 32;                                      /* NOTE ROLL pattern 3: 32 steps */
        dr_pat[3 * 3648 + 8] = 1;                                   /* DRUM ROLL pattern 4: lane 1, step 1 on */
        nr_swing = 20;
        dr_swing = 40;
        for (j = 0; j < 8; j++) {                                   /* every synth sound: drwave, tag 1 */
            uint32_t i2;

            SND_TYPE[j] = 2;
            for (i2 = 0; i2 < 8; i2++) {
                SND_KNOBS[j * 8 + i2] = (int16_t)(i2 < 4 ? 1000 + j * 10 + i2 : 1 << 12 | (j * 7 + i2));
                SND_ADSR[j * 8 + i2] = (int16_t)(2000 + j * 10 + i2);
            }
            SND_OCT[j] = 1;
        }
        for (j = 0; j < PS_TRACKS; j++)                             /* a chain row on its own sound 6 */
            if (py_song.trk[j].type == PS_SYNTH && py_song.cell[0][j] < PS_CHAINS
                && py_song.chain[py_song.cell[0][j]][0].pat < 14 && py_song.trk[j].sound != 5) {
                py_song.chain[py_song.cell[0][j]][0].snd = 5;
                break;
            }
        mains(30);                                                  /* the published copies anew */
        press(TE_KEY_TAPE);
        shift_press(TE_KEY_JOIN);
        CHECK(29, py_view == 3);
        mains(2);                                                   /* the headers */
        press(TE_KEY_REC);                                          /* asks */
        CHECK(29, find("/yaffs2/user/play_s/SONG01.BIN") < 0);
        press(TE_KEY_REC);
        mains(2);
        CHECK(29, find("/yaffs2/user/play_s/SONG01.BIN") >= 0);
        smask = find("/yaffs2/user/play_s/SONG01.BIN") >= 0 ? fs[find("/yaffs2/user/play_s/SONG01.BIN")].data[32] : 0;
        for (j = 0; j < 8; j++)
            ns += (smask >> j) & 1;
        for (s0 = 0; s0 < 8 && !((smask >> s0) & 1); s0++)
            ;
        CHECK(29, ns >= 2 && s0 < 8 && ((smask >> 5) & 1));
        hook_frame((uint16_t *)FB9);                                /* the songs page: slot 01 = the song now */
        dbg[0] = find("/yaffs2/user/play_s/SONG01.BIN") >= 0 ? (int32_t)fs[find("/yaffs2/user/play_s/SONG01.BIN")].size : -1;
        mcpy((uint8_t *)&keep, (const uint8_t *)&py_song, sizeof(keep));
        for (j = 0; j < 14 * 1796; j++)
            if (j / 1796 != 2)                                      /* (NOTE ROLL 3: kept below) */
                nsum = nsum * 31u + nr_pat[j];
        for (j = 0; j < 14 * 3648; j++)
            dsum = dsum * 31u + dr_pat[j];
        py_song.cell[0][0] = 0x55;                                  /* edits after the save */
        py_song.trk[0].level = -33;
        nr_pat[2 * 1796] = 8;
        dr_pat[3 * 3648 + 8] = 0;
        dr_owned &= (uint16_t)~1u;
        nr_swing = dr_swing = 0;
        for (j = 0; j < 8; j++)                                     /* the song's sounds edited */
            if ((smask >> j) & 1) {
                SND_KNOBS[j * 8] = (int16_t)(SND_KNOBS[j * 8] + 1);
                SND_ADSR[j * 8 + 1] = (int16_t)(SND_ADSR[j * 8 + 1] + 1);
                SND_OCT[j] = 2;
            }
        SND_TYPE[s0] = 0;                                           /* the first one an FM sound now */
        set_knobs_n = set_adsr_n = set_oct_n = 0;
        press(TE_KEY_PLAY);                                         /* asks */
        mains(2);
        CHECK(29, py_song.trk[0].level == -33);
        press(TE_KEY_PLAY);
        mains(3);                                                   /* compared: asks */
        CHECK(29, py_song.trk[0].level == -33 && py_view == 3);
        press(TE_KEY_JOIN);                                         /* not loaded */
        mains(2);
        CHECK(29, py_song.trk[0].level == -33 && py_view == 3 && nr_swing == 0);
        press(TE_KEY_PLAY);
        press(TE_KEY_PLAY);
        mains(3);
        for (j = 0; j < 28; j++) {                                  /* every row: from the slot */
            turn(1, 1);
            turn(0, 1);
        }
        turn(0, -28);
        turn(1, -1);                                                /* the first row: NOTE ROLL 3, kept */
        hook_frame((uint16_t *)FB10);                               /* the load question */
        turn(0, 40);                                                /* the last row: sound 6 */
        turn(1, -1);                                                /* kept ... */
        turn(1, 1);                                                 /* ... no: from the slot */
        turn(0, -1);                                                /* sound 2, an FM sound now */
        turn(1, 1);                                                 /* (cannot come back: kept anyway) */
        hook_frame((uint16_t *)FB12);
        turn(0, -40);
        CHECK(29, py_song.trk[0].level == -33);
        press(TE_KEY_PLAY);
        mains(3);
        for (j = 0; j < 8; j++) {                                   /* back as saved, the FM one kept */
            int back = SND_KNOBS[j * 8] == (int16_t)(1000 + j * 10) && SND_ADSR[j * 8 + 1] == (int16_t)(2000 + j * 10 + 1)
                       && SND_OCT[j] == 1;

            if ((smask >> j) & 1)
                snd_ok &= (int)j == s0 ? !back && SND_OCT[j] == 2 : back;
        }
        CHECK(29, snd_ok && set_knobs_n == ns - 1 && set_adsr_n == ns - 1 && set_oct_n == ns - 1
                  && py_snd_why[s0] == 2 /* PY_SND_OTHER */);
        dbg[4] = smask | ns << 8 | s0 << 12 | snd_ok << 16 | set_knobs_n << 20 | py_snd_why[s0] << 28;
        SND_TYPE[s0] = 2;
        CHECK(29, nr_pat[2 * 1796] == 8);                           /* kept as it was */
        py_pat_slot_now(&changed);
        CHECK(29, changed);
        for (j = 0; j < 14 * 1796; j++)
            if (j / 1796 != 2)
                n2 = n2 * 31u + nr_pat[j];
        for (j = 0; j < 14 * 3648; j++)
            d2 = d2 * 31u + dr_pat[j];
        for (j = 0; j < sizeof(keep); j++)
            same &= ((const uint8_t *)&keep)[j] == ((const uint8_t *)&py_song)[j];
        CHECK(29, same && n2 == nsum && d2 == dsum && nr_owned == 0x3fff && dr_owned == 0x3fff);
        CHECK(29, nr_swing == 20 && dr_swing == 40 && py_view == 1 && !py_mx.playing);
        dbg[1] = same | (n2 == nsum) << 1 | (d2 == dsum) << 2 | (nr_owned == 0x3fff) << 3 | (dr_owned == 0x3fff) << 4
                 | nr_swing << 8 | dr_swing << 16 | py_view << 24;
        shift_press(TE_KEY_JOIN);                                   /* cut twice: deleted */
        mains(2);
        press(TE_KEY_CUT);
        press(TE_KEY_CUT);
        mains(2);
        CHECK(29, find("/yaffs2/user/play_s/SONG01.BIN") < 0 && py_view == 3);
        press(TE_KEY_JOIN);
    }

    {   /* 30: files page */
        extern volatile uint8_t py_nrfiles;
        uint32_t st[32];
        int before;

        press(TE_KEY_STOP);
        mains(1);
        shift_press(TE_KEY_JOIN);                                   /* the songs page */
        mains(2);
        shift_press(TE_KEY_JOIN);                                   /* the files page */
        mains(2);
        before = py_nrfiles;
        CHECK(30, py_view == 4 && before >= 1 && file_stat("/yaffs2/user/play_r/REND01.AIF", st) == 0);
        hook_frame((uint16_t *)FB11);                               /* the files page */
        press(TE_KEY_CUT);                                          /* asks */
        mains(2);
        CHECK(30, file_stat("/yaffs2/user/play_r/REND01.AIF", st) == 0);
        press(TE_KEY_CUT);
        mains(3);
        CHECK(30, file_stat("/yaffs2/user/play_r/REND01.AIF", st) < 0 && py_nrfiles == before - 1 && py_view == 4);
        dbg[2] = before | py_nrfiles << 8 | py_view << 16;
        press(TE_KEY_JOIN);
    }

    {   /* 31: renders into disk mode */
        extern volatile uint32_t py_export_files, py_export_skipped;
        int i, k, nr = 0, ok = 1, nv = 0;

        for (i = 0; i < NF; i++)
            nr += fs[i].used && !fs[i].dir && in_dir(fs[i].path, "/yaffs2/user/play_r");
        hook_disk_enter(0, (void *)4);                              /* (mounting: not ours) */
        CHECK(31, vf_mounts == 1 && !vf_mkdirs && !vf[0].used);
        hook_disk_enter(0, 0);
        for (i = 0; i < NF; i++) {
            uint32_t h = 0, j;
            char p[24];

            if (!fs[i].used || fs[i].dir || !in_dir(fs[i].path, "/yaffs2/user/play_r"))
                continue;
            for (j = 0; j < fs[i].size; j++)
                h = h * 31u + fs[i].data[j];
            scpy(p, "/play_r/");
            scpy(p + 8, fs[i].path + 20);                           /* "/yaffs2/user/play_r/" */
            for (k = 0; k < VF && !(vf[k].used && streq(vf[k].path, p)); k++)
                ;
            ok &= k < VF && vf[k].len == fs[i].size && vf[k].hash == h;
        }
        for (k = 0; k < VF; k++)
            nv += vf[k].used;
        CHECK(31, nr >= 1 && ok && nv == nr && py_export_files == (uint32_t)nr && !vf_open_now && vf_mounts == 2);
        dbg[3] = nr | nv << 8 | ok << 16 | (int)py_export_files << 24;
        for (k = 0; k < VF; k++)                                    /* no room: skipped */
            vf[k].used = 0;
        vf_free = 2000000u + 1000u;
        hook_disk_enter(0, 0);
        for (k = nv = 0; k < VF; k++)
            nv += vf[k].used;
        CHECK(31, !nv && py_export_skipped == (uint32_t)nr && !vf_open_now);
    }

    {   /* 29 (cont.): a slot into disk mode (play_s/SONG01.PSL) and shared files back into slots */
        static const char s1[] = "/yaffs2/user/play_s/SONG01.BIN", s2[] = "/yaffs2/user/play_s/SONG02.BIN";
        uint32_t st[32], h = 0, j, imported;
        int k, f, ok = 0;

        vf_free = 100u << 20;
        press(TE_KEY_STOP);
        mains(1);
        shift_press(TE_KEY_JOIN);                                   /* the songs page, slot 01 */
        mains(2);
        press(TE_KEY_REC);
        press(TE_KEY_REC);
        mains(2);
        f = find(s1);
        CHECK(29, f >= 0 && py_view == 3);
        for (k = 0; k < VF; k++)
            vf[k].used = 0;
        hook_disk_enter(0, 0);
        if (f >= 0)
            for (j = 0; j < fs[f].size; j++)
                h = h * 31u + fs[f].data[j];
        for (k = 0; k < VF; k++)
            ok |= vf[k].used && streq(vf[k].path, "/play_s/SONG01.PSL") && f >= 0 && vf[k].len == fs[f].size
                  && vf[k].hash == h;
        CHECK(29, ok && py_export_songs == 1 && !vf_open_now);
        if (f >= 0)
            song_image(fs[f].data, fs[f].size);                     /* the computer's copy */
        set_image(IMG4, IMG4_END);
        imported = py_songs_imported;
        hook_cleanup("", 0);                                        /* slot 01 holds it: nothing */
        CHECK(29, py_songs_imported == imported && find(s2) < 0 && !py_songs_left);
        file_unlink(s1);                                            /* gone here: back into slot 01 */
        hook_cleanup("", 0);
        f = find(s1);
        CHECK(29, f >= 0 && py_songs_imported == imported + 1);
        for (j = 0, h = 0; f >= 0 && j < fs[f].size; j++)
            h = h * 31u + fs[f].data[j];
        CHECK(29, f >= 0 && h == song_hash);
        py_song.trk[0].level = -20;                                 /* slot 01 another song now */
        press(TE_KEY_REC);
        press(TE_KEY_REC);
        mains(2);
        hook_cleanup("", 0);                                        /* the shared one: slot 02 */
        f = find(s2);
        for (j = 0, h = 0; f >= 0 && j < fs[f].size; j++)
            h = h * 31u + fs[f].data[j];
        CHECK(29, f >= 0 && h == song_hash && py_songs_imported == imported + 2);
        file_unlink(s2);
        IMG4[(uint32_t)IMG4_DATA + 100] ^= 1;                       /* damaged: left */
        set_image(IMG4, IMG4_END);
        hook_cleanup("", 0);
        CHECK(29, file_stat(s2, st) < 0 && py_songs_left == 1 && py_songs_imported == imported + 2);
        dbg[5] = (int)py_export_songs | (int)(py_songs_imported - imported) << 8 | (int)py_songs_left << 16;
        press(TE_KEY_JOIN);
    }

    CHECK(11, odd_io == 0);                                         /* TE's NAND driver: even buffers */
    return fails;
}
