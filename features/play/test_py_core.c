/* host test for py_core.c (tools/test_all.py builds it with its sibling py_core.c, -I include).
 *
 * set.txt parsing; every set rule (empty, A1 only, gaps, loops without A1, two files for a slot,
 * a bad header, whole bars, tempo from TE); the global-position mapping; and a full stream + mix
 * simulation against an independent per-sample reference: loops wrapping at their own length,
 * LOOP on across the set's end, LOOP off ending, mono and big-endian files, gains, an underrun
 * (silence, then back in sync at the right position), a read error. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "py_core.h"
#include "../../lib/audiofile.c"                /* aud_err_text */

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ------------------------------------------------------------------ set.txt */

static void test_setfile(void)
{
    static const char txt[] =
        "# my set\r\n"
        "bpm 98.5\r\n"
        "audio A1 \"song\" A2 \"drums and more\" A8 \"fx\"\n"   /* earlier versions: ignored */
        "pattern P1 \"intro\" P4 \"beat\"\n"
        "\n"
        "loop on\n"
        "volume 3\n";                               /* line 7: not understood */
    const char *t;
    py_setfile s;

    py_setfile_parse(txt, sizeof(txt) - 1, &s);
    CHECK(s.bpm_x100 == 9850 && s.bad_line == 7);
    t = "bpm 300\n";
    py_setfile_parse(t, (uint32_t)strlen(t), &s);
    CHECK(s.bpm_x100 == 0 && s.bad_line == 1);                              /* out of TE's range */
    t = "bpm 120\nbpm x\n";
    py_setfile_parse(t, (uint32_t)strlen(t), &s);
    CHECK(s.bpm_x100 == 12000 && s.bad_line == 2);
    py_setfile_parse("", 0, &s);
    CHECK(s.bpm_x100 == 0 && s.bad_line == 0);
    CHECK(py_bar_frames(12000) == 88200 && py_bar_frames(9000) == 117600);
}

/* ------------------------------------------------------------------ set rules */

static py_file mk(const char *name, int err, int ch, uint32_t frames)
{
    py_file f;

    memset(&f, 0, sizeof(f));
    strcpy(f.name, name);
    f.a.kind = AUD_WAV;
    f.a.err = (uint8_t)err;
    f.a.channels = (uint8_t)ch;
    f.a.bits = 16;
    f.a.rate = 44100;
    f.a.frames = frames;
    f.a.data_off = 44;
    f.a.data_bytes = frames * (uint32_t)ch * 2;
    return f;
}

static void test_assign(void)
{
    py_file f[3];
    py_setfile sf;
    py_set s;
    const char *t = "bpm 90\n";

    f[0] = mk("SONG.WAV", AUD_OK, 2, 1000);
    f[1] = mk("BAD.AIF", AUD_ERATE, 2, 1000);
    f[2] = mk("LOOP.AIF", AUD_OK, 1, 300);
    py_set_init(&s, 0, 12000);
    CHECK(s.bar_frames == 88200 && !s.t[0].used);
    CHECK(py_set_assign(&s, 0, f, 3, "song.wav", 0) && s.t[0].used && !s.t[0].loop
          && s.t[0].frames == 1000 && s.t[0].file == 0 && !s.t[0].err[0]);
    CHECK(py_set_assign(&s, 3, f, 3, "LOOP.AIF", 1) && s.t[3].loop && s.t[3].channels == 1);
    CHECK(!py_set_assign(&s, 1, f, 3, "BAD.AIF", 1) && !s.t[1].used && !strcmp(s.t[1].err, "NOT 44.1 KHZ"));
    CHECK(!py_set_assign(&s, 2, f, 3, "OLD.WAV", 1) && !strcmp(s.t[2].err, "OLD.WAV: GONE"));
    CHECK(!py_set_assign(&s, 4, f, 3, "", 1) && !strcmp(s.t[4].err, "NO FILE"));
    py_setfile_parse(t, (uint32_t)strlen(t), &sf);
    py_set_init(&s, &sf, 12000);
    CHECK(s.bpm_x100 == 9000 && s.bar_frames == 117600);                    /* set.txt wins */
    py_set_init(&s, 0, 0);
    CHECK(s.bpm_x100 == 12000);
}

/* ------------------------------------------------------------------ mapping */

static void test_map(void)
{
    py_file f[2];
    py_set s;
    uint32_t run;

    f[0] = mk("ONCE.WAV", AUD_OK, 2, 1000);
    f[1] = mk("LOOP.WAV", AUD_OK, 2, 300);
    py_set_init(&s, 0, 12000);
    py_set_assign(&s, 0, f, 2, "ONCE.WAV", 0);
    py_set_assign(&s, 1, f, 2, "LOOP.WAV", 1);
    CHECK(py_map(&s, 0, 0, &run) == 0 && run == 1000);
    CHECK(py_map(&s, 0, 999, &run) == 999 && run == 1);
    CHECK(py_map(&s, 0, 1000, &run) == -1);                                 /* once: done */
    CHECK(py_map(&s, 1, 650, &run) == 50 && run == 250);                    /* loop: modulo */
    CHECK(py_map(&s, 1, 100000, &run) == 100 && run == 200);
    CHECK(py_map(&s, 2, 10, &run) == -1);                                   /* unused */

    /* song-driven: ON OF ON in a circle (bar = 88200 at 120): 300 + 88200 + 300 frames */
    {
        static const uint8_t seg[3] = { 1, 0, 1 };

        py_set_song(&s, 1, seg, 3, 0);
        CHECK(s.t[1].song && s.t[1].cur.n == 3 && s.t[1].cur.period == 88800);
        CHECK(py_map(&s, 1, 0, &run) == 0 && run == 300 && py_set_row(&s, 1, 0) == 0);
        CHECK(py_map(&s, 1, 299, &run) == 299 && run == 1);
        CHECK(py_map(&s, 1, 300, &run) == -1 && run == 88200 && py_set_row(&s, 1, 300) == 1);   /* OF: a bar */
        CHECK(py_map(&s, 1, 88499, &run) == -1 && run == 1);
        CHECK(py_map(&s, 1, 88500, &run) == 0 && run == 300 && py_set_row(&s, 1, 88500) == 2);
        CHECK(py_map(&s, 1, 88800 + 310, &run) == -1 && run == 88190);      /* the circle again */
        CHECK(py_map(&s, 1, 3 * 88800 + 88510, &run) == 10 && run == 290);
        {                                       /* a loop section: rows 0 1 2 1 2 (each segment its row) */
            static const uint8_t lseg[5] = { 1, 0, 1, 0, 1 }, lrow[5] = { 0, 1, 2, 1, 2 };

            py_set_song_rows(&s, 1, lseg, lrow, 5, 0);
            CHECK(py_set_row(&s, 1, 88500) == 2 && py_set_row(&s, 1, 88800) == 1
                  && py_set_row(&s, 1, 88800 + 88200) == 2 && s.t[1].cur.period == 88800 + 88500);
            py_set_song(&s, 1, seg, 3, 5);                                     /* no rows: row0 + i */
            CHECK(py_set_row(&s, 1, 300) == 6);
        }
        py_set_song(&s, 1, seg, 0, 0);                                         /* cells, none at the start row */
        CHECK(s.t[1].song && py_map(&s, 1, 5, &run) == -1 && py_set_row(&s, 1, 5) == -1);
        py_set_song(&s, 1, seg, -1, 0);                                        /* free again: the loop */
        CHECK(!s.t[1].song && py_map(&s, 1, 650, &run) == 50 && py_set_row(&s, 1, 650) == -1);
        py_set_song(&s, 0, seg, 3, 0);
        py_set_assign(&s, 0, f, 2, "ONCE.WAV", 0);                          /* assigning: free */
        CHECK(!s.t[0].song);
    }
}

/* ------------------------------------------------------------------ streaming + mix */

#define NT 3
static uint8_t file[NT][200000];
static uint32_t file_frames[NT] = { 9000, 1300, 700 };
static int file_ch[NT] = { 2, 1, 2 };
static int file_be[NT] = { 0, 1, 1 };
static int track_of_file[NT] = { 0, 1, 5 };                                /* tracks 1, 2, 6 */
static int fail_reads;

static int16_t sample(int fi, uint32_t frame, int c)
{
    return (int16_t)(((frame * 7u + (uint32_t)fi * 1000u + (uint32_t)c * 300u) & 0x3fffu) - 0x2000);
}

static int mem_read(void *ctx, int k, uint32_t off, void *buf, uint32_t n)
{
    int fi;

    (void)ctx;
    if (fail_reads) {
        fail_reads--;
        return -1;
    }
    for (fi = 0; fi < NT; fi++)
        if (track_of_file[fi] == k)
            break;
    if (fi == NT || off + n > sizeof(file[0]))
        return -1;
    memcpy(buf, file[fi] + off, n);
    return (int)n;
}

static int32_t ref_soft(int32_t x)
{
    int32_t a = x < 0 ? -x : x;

    if (a > 24576) {
        a = 24576 + ((a - 24576) >> 2);
        if (a > 32767)
            a = 32767;
    }
    return x < 0 ? -a : a;
}

/* the expected output word (added to 0) of frame g, channel c */
static int32_t ref_word(const py_set *s, const py_mixer *m, uint32_t g, int c, const uint8_t *silent)
{
    int32_t acc = 0, mv = (m->master * m->te_volume) >> 15;
    int fi;

    for (fi = 0; fi < NT; fi++) {
        int k = track_of_file[fi];
        uint32_t run;
        int32_t f = py_map(s, k, g, &run);

        if (f < 0 || (silent && silent[k]))
            continue;
        acc += (sample(fi, (uint32_t)f, file_ch[fi] == 2 ? c : 0) * m->gain[k]) >> 15;
    }
    return (int32_t)((uint32_t)ref_soft((acc * mv) >> 15) << 16);
}

static int16_t ringmem[PY_TRACKS][1024 * 2];

static void setup(py_set *s, py_mixer *m, int loop)
{
    py_file f[NT];
    char nm[8];
    int fi, k;

    for (fi = 0; fi < NT; fi++) {
        uint32_t i, data_off = 54 + 10 * (uint32_t)fi;
        int c;

        snprintf(nm, sizeof(nm), "A%d.WAV", track_of_file[fi] + 1);
        f[fi] = mk(nm, AUD_OK, file_ch[fi], file_frames[fi]);
        f[fi].a.big_endian = (uint8_t)file_be[fi];
        f[fi].a.data_off = data_off;
        memset(file[fi], 0xee, data_off);                                   /* header junk */
        for (i = 0; i < file_frames[fi]; i++)
            for (c = 0; c < file_ch[fi]; c++) {
                uint16_t v = (uint16_t)sample(fi, i, c);
                uint8_t *p = file[fi] + data_off + (i * (uint32_t)file_ch[fi] + (uint32_t)c) * 2;

                if (file_be[fi]) {
                    p[0] = (uint8_t)(v >> 8);
                    p[1] = (uint8_t)v;
                } else {
                    p[0] = (uint8_t)v;
                    p[1] = (uint8_t)(v >> 8);
                }
            }
    }
    py_set_init(s, 0, 12000);
    for (fi = 0; fi < NT; fi++)                 /* track 0: once or looped; the others loop */
        py_set_assign(s, track_of_file[fi], f, NT, f[fi].name, fi ? 1 : loop);
    memset(m, 0, sizeof(*m));
    m->set = s;
    m->chunk = 256;
    m->master = 32767;
    m->te_volume = 26029;                                                   /* -2 dB */
    for (k = 0; k < PY_TRACKS; k++) {
        m->gain[k] = py_db_q15(-30 - 10 * k);
        if (s->t[k].used) {
            m->ring[k].buf = ringmem[k];
            m->ring[k].size = 1024;
            m->ring[k].ch = s->t[k].channels;
        }
    }
}

static int check_block(const py_set *s, const py_mixer *m, uint32_t g0, const int32_t *tx,
                       const uint8_t *silent)
{
    int f, c;

    for (f = 0; f < PY_BLOCK; f++)
        for (c = 0; c < 2; c++)
            if (tx[2 * f + c] != ref_word(s, m, g0 + (uint32_t)f, c, silent)) {
                printf("  mismatch at frame %u ch %d: %d vs %d\n", g0 + (uint32_t)f, c,
                       tx[2 * f + c], ref_word(s, m, g0 + (uint32_t)f, c, silent));
                return 0;
            }
    return 1;
}

static void test_stream(void)
{
    static uint8_t scratch[256 * 4];
    int32_t tx[2 * PY_BLOCK];
    py_set s;
    py_mixer m;
    int b, ok = 1;

    /* track 0 once (9000 frames), the loops looping: all in sync, track 0 silent after its end */
    setup(&s, &m, 0);
    py_seek(&m, 0);
    py_fill(&m, mem_read, 0, 1u << 30, scratch);
    CHECK(py_ahead(&m, 0) > 1024 - 256 && py_ahead(&m, 1) > 1024 - 256 && py_ahead(&m, 5) > 1024 - 256);
    m.playing = 1;
    for (b = 0; b < 1000; b++) {
        uint32_t g0 = m.pos;

        if (!(b % 4))
            py_fill(&m, mem_read, 0, 1u << 30, scratch);
        memset(tx, 0, sizeof(tx));
        py_mix(&m, tx, tx + 1, 2);
        ok &= check_block(&s, &m, g0, tx, 0);
    }
    CHECK(ok && m.playing && m.pos == 1000 * PY_BLOCK && !m.underruns[0] && !m.underruns[1]
          && !m.read_errors);
    m.playing = 0;
    memset(tx, 0, sizeof(tx));
    py_mix(&m, tx, tx + 1, 2);                                                         /* stopped: nothing */
    CHECK(tx[0] == 0 && tx[127] == 0);

    /* LOOP on, from a seek into the middle, across the set's end twice */
    setup(&s, &m, 1);
    py_seek(&m, 5000);
    py_fill(&m, mem_read, 0, 1u << 30, scratch);
    m.playing = 1;
    ok = 1;
    for (b = 0; b < 400; b++) {
        uint32_t g0 = m.pos;

        if (!(b % 4))
            py_fill(&m, mem_read, 0, 1u << 30, scratch);
        memset(tx, 0, sizeof(tx));
        py_mix(&m, tx, tx + 1, 2);
        ok &= check_block(&s, &m, g0, tx, 0);
    }
    CHECK(ok && m.playing && m.pos == 5000 + 400 * PY_BLOCK && !m.underruns[0]);

    /* underrun: no filling for 40 blocks (2560 frames > the 1024-frame ring): each track is
     * silent while its ring is late, then all are back in sync at the right position */
    {
        uint32_t late[PY_TRACKS] = { 0 };
        int resynced = 1, exact = 1, k;

        for (b = 0; b < 40; b++) {
            uint32_t g0 = m.pos;
            uint8_t silent[PY_TRACKS];

            for (k = 0; k < PY_TRACKS; k++) {
                silent[k] = s.t[k].used && py_ahead(&m, k) < PY_BLOCK;
                late[k] += silent[k];
            }
            memset(tx, 0, sizeof(tx));
            py_mix(&m, tx, tx + 1, 2);
            exact &= check_block(&s, &m, g0, tx, silent);
        }
        CHECK(exact && late[0] >= 20 && m.underruns[0] == late[0] && m.underruns[1] == late[1]
              && m.underruns[5] == late[5]);
        for (b = 0; b < 40; b++) {
            uint32_t g0 = m.pos;

            py_fill(&m, mem_read, 0, 1u << 30, scratch);
            memset(tx, 0, sizeof(tx));
            py_mix(&m, tx, tx + 1, 2);
            resynced &= check_block(&s, &m, g0, tx, 0);
        }
        CHECK(resynced);
    }

    /* a read error: counted, that chunk is silence, nothing crashes */
    setup(&s, &m, 0);
    py_seek(&m, 0);
    fail_reads = 1;
    py_fill(&m, mem_read, 0, 1u << 30, scratch);
    CHECK(m.read_errors == 1);

    /* budget: one call reads at most about the budget */
    setup(&s, &m, 0);
    py_seek(&m, 0);
    CHECK(py_fill(&m, mem_read, 0, 1500, scratch) <= 1500 + 256 * 4);
}

/* pattern-track inputs: mono into both channels with their gain, cleared after use; without an
 * audio set the clock runs on (no end) */
/* a song-driven track through the ring: ON OF OF ON at a 1000-frame bar (track 1: 1300 frames),
 * against the samples worked out here (not py_map); the other tracks loop on */
static void test_stream_song(void)
{
    static uint8_t scratch[256 * 4];
    static const uint8_t seg[4] = { 1, 0, 0, 1 };
    int32_t tx[2 * PY_BLOCK];
    py_set s;
    py_mixer m;
    int b, f, ok = 1;

    setup(&s, &m, 1);
    s.bar_frames = 1000;
    py_set_song(&s, 1, seg, 4, 0);
    CHECK(s.t[1].cur.period == 4600);
    m.gain[0] = m.gain[5] = 0;
    py_seek(&m, 0);
    py_fill(&m, mem_read, 0, 1u << 30, scratch);
    m.playing = 1;
    for (b = 0; b < 300; b++) {                                             /* four circles */
        uint32_t g0 = m.pos;

        if (!(b % 4))
            py_fill(&m, mem_read, 0, 1u << 30, scratch);
        memset(tx, 0, sizeof(tx));
        py_mix(&m, tx, tx + 1, 2);
        for (f = 0; f < PY_BLOCK; f++) {
            uint32_t p = (g0 + (uint32_t)f) % 4600u;
            int32_t mv = (m.master * m.te_volume) >> 15, acc = 0;

            if (p < 1300)
                acc = (sample(1, p, 0) * m.gain[1]) >> 15;
            else if (p >= 3300)
                acc = (sample(1, p - 3300, 0) * m.gain[1]) >> 15;
            ok &= tx[2 * f] == (int32_t)((uint32_t)ref_soft((acc * mv) >> 15) << 16) && tx[2 * f + 1] == tx[2 * f];
        }
    }
    CHECK(ok && !m.underruns[1] && !m.read_errors);
}

/* live cues on a song-driven track: ON ON (rows 4, 5; 1300 frames each) cued to OF ON (rows 9, 10)
 * while row 4 plays: the switch at the end of the segment, the ring taken back and refilled, the
 * samples exact through the switch; a cue too close to the segment's end waits for the next one;
 * a second cue replaces a pending one; a cue to nothing = silence; a silent track: the next bar */
static void test_cue(void)
{
    static uint8_t scratch[256 * 4];
    static const uint8_t on2[2] = { 1, 1 }, ofon[2] = { 0, 1 }, on1[1] = { 1 };
    int32_t tx[2 * PY_BLOCK];
    py_set s;
    py_mixer m;
    uint32_t sw, run;
    int b, f, ok = 1;

    setup(&s, &m, 1);
    s.bar_frames = 1000;
    py_set_song(&s, 1, on2, 2, 4);
    m.gain[0] = m.gain[5] = 0;
    py_seek(&m, 0);
    py_fill(&m, mem_read, 0, 1u << 30, scratch);
    CHECK(py_set_row(&s, 1, 0) == 4 && py_set_row(&s, 1, 1300) == 5 && py_set_cued(&s, 1, 0) == -1);
    m.playing = 1;
    for (b = 0; b < 9; b++) {
        memset(tx, 0, sizeof(tx));
        py_mix(&m, tx, tx + 1, 2);
    }
    py_fill(&m, mem_read, 0, 1u << 30, scratch);                            /* read ahead past the switch */
    CHECK(m.ring[1].wr > 1300);
    sw = py_set_cue(&s, &m, 1, ofon, 2, 9, 256);                           /* pos 576: row 4 ends at 1300 */
    CHECK(sw == 1300 && s.t[1].pending && m.ring[1].wr == 1300 && py_set_cued(&s, 1, m.pos) == 9);
    CHECK(py_map(&s, 1, 1200, &run) == 1200 && run == 100);                 /* up to the switch */
    CHECK(py_map(&s, 1, 1300, &run) == -1 && run == 1000 && py_set_row(&s, 1, 1300) == 9);
    CHECK(py_map(&s, 1, 2300, &run) == 0 && run == 1300 && py_set_row(&s, 1, 2300) == 10);
    CHECK(py_map(&s, 1, 3600 + 10, &run) == -1 && run == 990);              /* the new circle: 2300 */
    for (b = 9; b < 120; b++) {
        uint32_t g0 = m.pos;

        py_fill(&m, mem_read, 0, 1u << 30, scratch);
        memset(tx, 0, sizeof(tx));
        py_mix(&m, tx, tx + 1, 2);
        for (f = 0; f < PY_BLOCK; f++) {
            uint32_t g = g0 + (uint32_t)f, p = g < 1300 ? g : (g - 1300) % 2300u;
            int32_t mv = (m.master * m.te_volume) >> 15, acc = 0;

            if (g < 1300)
                acc = (sample(1, p, 0) * m.gain[1]) >> 15;
            else if (p >= 1000)
                acc = (sample(1, p - 1000, 0) * m.gain[1]) >> 15;
            ok &= tx[2 * f] == (int32_t)((uint32_t)ref_soft((acc * mv) >> 15) << 16);
        }
    }
    CHECK(ok && !m.underruns[1] && py_set_cued(&s, 1, m.pos) == -1);

    /* pos 7680 = 1300 + 2 x 2300 + 1780: in the ON segment (row 10), 520 frames left: a guard of
     * 600 reaches into the next segment (OF, 1000 frames): the switch waits for its end */
    CHECK(m.pos == 7680);
    sw = py_set_cue(&s, &m, 1, on1, 1, 20, 600);
    CHECK(sw == 7680 + 520 + 1000 && py_set_cued(&s, 1, m.pos) == 20);
    sw = py_set_cue(&s, &m, 1, on2, 2, 30, 600);                           /* replaces it, same frame */
    CHECK(sw == 9200 && py_set_cued(&s, 1, m.pos) == 30 && py_set_row(&s, 1, 9200) == 30
          && py_set_row(&s, 1, 9199) == 9);
    sw = py_set_cue(&s, &m, 1, on1, 0, 40, 600);                           /* to an empty cell: silence */
    CHECK(sw == 9200 && py_map(&s, 1, 9300, &run) == -1 && py_set_row(&s, 1, 9300) == -1);
    m.pos = 9600;                                                           /* silent now: the next bar */
    sw = py_set_cue(&s, &m, 1, on1, 1, 50, 600);
    CHECK(sw == 11000 && py_map(&s, 1, 10990, &run) == -1 && run == 10
          && py_map(&s, 1, 11000, &run) == 0 && py_set_row(&s, 1, 11000) == 50);
    py_set_song(&s, 1, on2, 2, 0);                                          /* the song anew: no cue */
    CHECK(!s.t[1].pending && py_set_cued(&s, 1, 0) == -1);
    py_set_song(&s, 1, on2, -1, 0);
    CHECK(py_set_cue(&s, &m, 1, on1, 1, 0, 600) == 0 && !s.t[1].pending);  /* a free track: no cues */
}

static void test_pattern_inputs(void)
{
    static py_set s;
    static py_mixer m;
    int32_t tx[2 * PY_BLOCK];
    int f, ok = 1;

    py_set_init(&s, 0, 12000);
    memset(&m, 0, sizeof(m));
    m.set = &s;
    m.master = 32767;
    m.te_volume = 32767;
    m.gain[3] = 16384;
    m.playing = 1;
    for (f = 0; f < PY_BLOCK; f++)
        m.pin[3][f] = f * 100 - 3000;
    m.pin_used = 1 << 3;
    memset(tx, 0, sizeof(tx));
    py_mix(&m, tx, tx + 1, 2);
    for (f = 0; f < PY_BLOCK; f++) {
        int32_t v = (((f * 100 - 3000) * 16384) >> 15) * 32766 >> 15;

        ok &= tx[2 * f] == (int32_t)((uint32_t)v << 16) && tx[2 * f + 1] == tx[2 * f] && m.pin[3][f] == 0;
    }
    CHECK(ok && m.pin_used == 0 && m.peak[3] == 1650 && m.playing && m.pos == PY_BLOCK);
    memset(tx, 0, sizeof(tx));
    py_mix(&m, tx, tx + 1, 2);                                              /* nothing new: silence */
    CHECK(tx[0] == 0 && tx[127] == 0 && m.playing && m.pos == 2 * PY_BLOCK);
}

int main(void)
{
    test_pattern_inputs();
    test_setfile();
    test_assign();
    test_map();
    test_stream();
    test_stream_song();
    test_cue();
    CHECK(py_db_q15(0) == 32767 && py_db_q15(-60) == 16423 && py_db_q15(-600) == 33
          && py_db_q15(-601) == 0 && py_db_q15(10) == 32767);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
