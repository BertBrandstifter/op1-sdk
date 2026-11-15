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
 *  7 keys: tape again toggles PLAY / TE's tape; keys go to TE when TE's tape is shown; a release
 *    whose press PLAY took is taken; shift + tape on PLAY does not reach TE; knobs
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
 * 15 white turn = sound, orange turn = pattern; SYNTH cost options: white press cycles the voices (2 -> 4 -> 1), shift + white press ECO: the
 *    engine renders every other block at twice the step; muted: nothing renders; PLAY's CPU
 *    numbers (total, per track) appear while playing and clear on stop
 * Returns failed-check bits (0 = pass). */
#include <stdarg.h>
#include <op1/te246.h>
#include "../py_core.h"
#include "../py_song.h"

extern void hook_key(int key, int down);
extern void hook_knob(int knob, int16_t *diff);
extern void hook_frame(uint16_t *buf);
extern void hook_volume(void);                  /* entry hook on 0x019de548 (TE's metronome mix) */
extern void hook_cleanup(const char *dir, int depth);  /* call site 0x01997584 */
extern void py_main(void);
extern py_mixer py_mx;
extern volatile uint32_t py_blocks, py_import_files, py_import_errors, py_import_runs;
extern char FB[], FB2[], FB3[];
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

static void set_image(uint8_t *b, uint8_t *e)
{
    mcpy(fs[image].data, b, (uint32_t)(e - b));  /* tape_c.raw: its own 3 MB at 0x02200000 */
    fs[image].size = (uint32_t)(e - b);
}

static void clear_master(void)
{
    int i;

    for (i = 0; i < 64; i++)
        E34[i] = F34[i] = 0;
}

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)

/* ------------------------------------------------------------------ a test engine + TE's envelope */

static int eng_vi = -1, eng_calls;
static uint32_t eng_inc;
static int env_starts, env_releases, env_slot = -1;

static void test_engine(int vi, int16_t *out, const int16_t *knobs, const uint32_t *inc, int flags)
{
    int i;

    (void)knobs;
    (void)flags;
    eng_vi = vi;
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
    press(TE_KEY_TAPE);                         /* tape again: TE's tape */
    hook_key(TE_KEY_PLAY, 0);                   /* ... release on TE's tape: still taken */
    CHECK(7, keys_to_te == 0);
    press(TE_KEY_INSERT);                       /* TE's tape shown: goes to TE */
    CHECK(7, keys_to_te == 2);
    *TE_SHIFT_HELD = 1;
    press(TE_KEY_TAPE);                         /* shift + tape on TE's tape: TE's erase screen */
    *TE_SHIFT_HELD = 0;
    CHECK(7, keys_to_te == 4);
    press(TE_KEY_TAPE);                         /* tape again: PLAY */
    *TE_SHIFT_HELD = 1;
    press(TE_KEY_TAPE);                         /* shift + tape on PLAY: not to TE */
    *TE_SHIFT_HELD = 0;
    CHECK(7, keys_to_te == 4);
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
        turn(3, 1);                                                 /* orange: pattern 1 */
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
        turn(3, 1);                                                 /* orange: pattern 1 */
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
        for (b = 0; b < 40; b++)                                    /* inside the note (1 step) */
            hook_volume();
        CHECK(15, eng_calls - calls == 20 && eng_inc == 2 * 21426141u);
        *TE_SHIFT_HELD = 1;
        press(TE_KEY_WHITE_PUSH);                                   /* ECO off */
        *TE_SHIFT_HELD = 0;
        calls = eng_calls;
        for (b = 0; b < 20; b++)
            hook_volume();
        CHECK(15, eng_calls - calls == 20 && eng_inc == 21426141u);
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

    CHECK(11, odd_io == 0);                                         /* TE's NAND driver: even buffers */
    return fails;
}
