/* PLAY demo songs: four song slot files (py_slot.h, "PSL2") written by PLAY's own py_slot_save,
 * then read back by py_slot_check / py_slot_load and compared (gen_demos.py builds and runs it).
 *
 *   SONG13.PSL  WAREHOUSE  techno, A minor, ~128 BPM: 303 line (ACID 3), pad, bell motif, drums
 *   SONG14.PSL  PIANO HOUSE  house, F major, ~124 BPM: M1 piano stabs, organ bass, strings, drums
 *   SONG15.PSL  RADIO  pop, G major, ~112 BPM: e.piano, bass, saw lead melody, bell pad, drums
 *   SONG16.PSL  PRELUDE  classic: J. S. Bach, Prelude in C major BWV 846, bars 1-19 + a final
 *               chord (public domain), harpsichord + strings, ~76 BPM
 *
 * Own compositions (MIT, like the rest of PLAY) on common genre patterns, the Bach a transcription
 * of the public-domain score. Sounds: SDK engines PLAY restores from a slot (ACID 3 tag 8,
 * STATION 1 tag 3) in synth sound slots 5-8, knob blocks built like the engines' own banks
 * (acid.c ac_bank_knobs, m1.c bank_load); drums: DRUM ROLL lanes on the "m1 drums" kit keys
 * (m1_drumkit.py) in drum slot 8. The tempo is not part of a slot: set it on TE's tempo page.
 *
 *   cc ... gen_demos.c py_song.c py_slot.c nr_core.c dr_core.c && ./gen_demos <out dir> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "py_song.h"
#include "py_slot.h"
#include "nr_core.h"
#include "dr_core.h"

/* ------------------------------------------------------------------ sounds */

#define DRWAVE      2                           /* TE_INSTR_DRWAVE */
#define SND0        4                           /* synth sound slot 5 (0-based 4) */
#define KIT         7                           /* drum slot 8 */

static int16_t eh_p2s(int tag, int s, int v)    /* enginehost eh_host.h */
{
    int t = tag < 8 ? tag : s < 3 ? 7 : tag - 7;

    return (int16_t)((t << 12) | (v & 0x0fff));
}

static int eh_tag(const int16_t *k)             /* enginehost.c */
{
    int t = (k[4] >> 12) & 7, t8 = (k[7] >> 12) & 7;

    if (t == 0 || k[4] < 0 || k[5] < 0 || k[6] < 0 || k[7] < 0
        || ((k[5] >> 12) & 7) != t || ((k[6] >> 12) & 7) != t)
        return 0;
    if (t8 == t)
        return t;
    return t == 7 && t8 >= 1 && t8 <= 6 ? 7 + t8 : 0;
}

/* ACID 3 (acid.c): 20 parameters, page order */
enum { P_CUTOFF, P_RES, P_ENVMOD, P_DECAY, P_ACCENT, P_ACCDEC, P_SWEEP, P_SPEED, P_WAVE, P_SUB, P_TUNE,
       P_SLIDE, P_SOFT, P_TRACK, P_FM, P_DRIVE, P_MUFF, P_ACCS, P_SLIDES, P_LEVEL, AC_NP };
static const uint8_t ac_pmax[AC_NP] = { 255, 255, 255, 255, 63, 31, 2, 2, 1, 3, 24, 31, 31, 31, 31, 31, 2, 5, 2, 15 };
static const struct { uint8_t c, sh, bits; } ac_fields[AC_NP] = {
    [P_ACCENT] = { 0, 0, 6 },
    [P_ACCDEC] = { 1, 0, 5 }, [P_WAVE] = { 1, 5, 1 },
    [P_SWEEP] = { 2, 0, 2 }, [P_SPEED] = { 2, 2, 2 }, [P_SUB] = { 2, 4, 2 },
    [P_TUNE] = { 3, 0, 5 },
    [P_SLIDE] = { 4, 0, 5 }, [P_SOFT] = { 4, 5, 5 }, [P_MUFF] = { 4, 10, 2 },
    [P_TRACK] = { 5, 0, 5 }, [P_FM] = { 5, 5, 5 }, [P_SLIDES] = { 5, 10, 2 },
    [P_DRIVE] = { 6, 0, 5 }, [P_ACCS] = { 6, 5, 3 }, [P_LEVEL] = { 6, 8, 4 },
};

static void ac_set(int16_t *k, int id, int v)
{
    v = v < 0 ? 0 : v > ac_pmax[id] ? ac_pmax[id] : v;
    if (id <= P_DECAY) {
        k[id] = (int16_t)((v << 6) | (k[id] & 63));
    } else {
        int c = ac_fields[id].c, m = ((1 << ac_fields[id].bits) - 1) << ac_fields[id].sh;
        int cur = c < 4 ? k[c] & 63 : k[c] & 0x0fff, raw = (cur & ~m) | ((v << ac_fields[id].sh) & m);

        if (c < 4)
            k[c] = (int16_t)((k[c] & ~63) | raw);
        else
            k[c] = eh_p2s(8, c - 4, raw);
    }
}

static void acid_knobs(int16_t *k, const uint8_t v[AC_NP])
{
    int i;

    for (i = 0; i < 8; i++)
        k[i] = i < 4 ? 0 : eh_p2s(8, i - 4, 0);
    for (i = 0; i < AC_NP; i++)
        ac_set(k, i, v[i]);
}

/* STATION 1 (m1.c bank_load, the sound library: 100 multisounds; m1_bank.inc) */
#define M1_NW   100
#define M1_OFF  (-1)

static int16_t zone1(int z, int n) { return (int16_t)(((2 * z + 1) * 32768 / (2 * n)) >> 1); }
static int16_t m1_pack(int hi, int lo) { return eh_p2s(3, 0, hi << 6 | lo); }

typedef struct { int a, b, mix, cut, bpitch, det, fenv, fdec, res, ktrk, adec, bfade; } m1_entry;

static void m1_knobs(int16_t *k, const m1_entry *b)
{
    k[0] = zone1(b->a, M1_NW);
    k[1] = zone1(b->b == M1_OFF ? 0 : b->b + 1, M1_NW + 1);
    k[2] = (int16_t)(b->mix * 16383 / 100);
    k[3] = (int16_t)(b->cut * 16383 / 100);
    k[4] = m1_pack(b->bpitch + 24, b->res * 63 / 100);
    k[5] = m1_pack(b->det, b->ktrk * 63 / 100);
    k[6] = m1_pack(b->fenv * 63 / 100, b->adec);
    k[7] = m1_pack(b->fdec * 63 / 100, b->bfade);
}

/* m1_bank.inc entries (multisound numbers of the sound library) */
static const m1_entry M1_PIANO   = { 0, 0, 100, 100, -12, 0, 0, 0, 0, 0, 0, 0 };
static const m1_entry M1_UNIVERSE = { 30, 42, 48, 85, 0, 0, 20, 60, 0, 30, 0, 20 };
static const m1_entry M1_HORGAN  = { 6, 6, 100, 100, -12, 0, 0, 0, 0, 0, 0, 0 };
static const m1_entry M1_EPIANO  = { 1, 1, 100, 100, -12, 0, 0, 0, 0, 0, 0, 0 };
static const m1_entry M1_HARPSI  = { 4, 4, 100, 100, -12, 0, 0, 0, 0, 0, 0, 0 };
static const m1_entry M1_SBASS   = { 17, 17, 100, 40, -12, 0, 60, 25, 35, 30, 0, 0 };
static const m1_entry M1_STRINGS = { 31, M1_OFF, 0, 100, 0, 0, 0, 0, 0, 0, 0, 0 };
static const m1_entry M1_BELLPAD = { 22, 31, 50, 90, 0, 0, 0, 0, 0, 0, 34, 36 };
static const m1_entry M1_SAWLEAD = { 91, M1_OFF, 0, 55, 0, 0, 40, 45, 25, 40, 0, 0 };
static const m1_entry M1_DIGIBELL = { 20, 20, 100, 100, 12, 0, 0, 0, 0, 0, 34, 0 };

/* TE's envelope (JSON "adsr"): attack, decay, sustain, release (higher = shorter), play mode << 12,
 * portamento, 2 unknown (as the presets) */
static const int16_t ENV_KEYS[8]  = { 64, 9000, 16000, 3000, 4000, 64, 4000, 4000 };   /* piano, e.piano */
static const int16_t ENV_PAD[8]   = { 5000, 8128, 32767, 1800, 4000, 64, 4000, 4000 };  /* slow in, long out */
static const int16_t ENV_BASS[8]  = { 64, 8128, 32767, 6000, 4000, 64, 4000, 4000 };
static const int16_t ENV_LEAD[8]  = { 300, 8128, 26000, 3500, 4000, 64, 4000, 4000 };
static const int16_t ENV_BELL[8]  = { 64, 10000, 6000, 2500, 4000, 64, 4000, 4000 };
static const int16_t ENV_PLUCK[8] = { 64, 8128, 24575, 4000, 4000, 64, 4000, 4000 };   /* the M1 preset's */

static uint8_t sounds[PSL_SOUNDS][PSL_SND];
static uint8_t snd_set;                         /* bit s: sounds[s] was given */
static const int16_t *pv_env[PSL_SOUNDS];       /* preview: the timbre (by envelope) */
static int8_t pv_shift[PSL_SOUNDS];             /* preview: the engine's own transposition */

static void put16(uint8_t *b, int16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)((uint16_t)v >> 8); }

static void sound(int s, int octave, const int16_t *k, const int16_t *env)
{
    uint8_t *r = sounds[s];
    int i;

    memset(r, 0, PSL_SND);
    r[0] = DRWAVE;
    r[1] = (uint8_t)(int8_t)octave;
    r[2] = (uint8_t)eh_tag(k);
    for (i = 0; i < 8; i++) {
        put16(r + 4 + 2 * i, k[i]);
        put16(r + 20 + 2 * i, env[i]);
    }
    snd_set |= (uint8_t)(1u << s);
    pv_env[s] = env;
    pv_shift[s] = 0;
}

static void sound_m1(int s, int octave, const m1_entry *e, const int16_t *env)
{
    int16_t k[8];

    m1_knobs(k, e);
    sound(s, octave, k, env);
    pv_shift[s] = (int8_t)(e->mix >= 50 ? e->bpitch : 0);   /* (mostly layer B sounds) */
}

/* ------------------------------------------------------------------ patterns + song */

static ps_song song;
static nr_pattern npat[PSL_PATS], n_empty;
static dr_pattern dpat[PSL_PATS], d_empty;
static uint8_t swing[2];

static int note(const char *name)               /* "C4" = 60, "F#3", "Bb2" */
{
    static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 };     /* A B C D E F G */
    int n = base[name[0] - 'A'], i = 1;

    if (name[1] == '#') { n++; i++; }
    else if (name[1] == 'b') { n--; i++; }
    return n + 12 * (atoi(name + i) + 1);
}

/* NOTE ROLL lengths (nr_len8 index): 1/2 step .. 16 steps */
enum { L_HALF = 2, L1 = 4, L15 = 5, L2 = 6, L3 = 7, L4 = 8, L6 = 9, L8 = 10, L12 = 11, L16 = 12 };

static void npat_len(int p, int steps) { nr_pattern_init(&npat[p]); npat[p].len = (uint8_t)steps; }

static void n_add(int p, int step, int key, int vel, int len)
{
    if (nr_add(&npat[p].s[step], key, vel, len) < 0) {
        fprintf(stderr, "pattern %d step %d: no room for %d\n", p, step, key);
        exit(1);
    }
}

static void n_chord(int p, int step, const int *keys, int n, int vel, int len)
{
    int i;

    for (i = 0; i < n; i++)
        n_add(p, step, keys[i], vel, len);
}

/* drum kit keys ("m1 drums", m1_drumkit.py) */
enum { KICK = 0, KICK2 = 1, STICK = 3, SNARE = 4, SNARE2 = 5, CLAP = 8, CH = 9, CH2 = 10, OH = 11,
       SHAKER = 13, TAMB = 14, TOM1 = 15, TOM2 = 16, ETOM = 17, RIDE = 18, CRASH = 19, CONGA1 = 21, CONGA2 = 22 };

static void dpat_new(int p, int steps, const int *keys, int nkeys)
{
    int l;

    dr_pattern_init(&dpat[p]);
    for (l = 0; l < DR_LANES; l++) {
        dr_lane_init(&dpat[p].lane[l], l < nkeys ? keys[l] : l);
        dpat[p].lane[l].len = (uint8_t)steps;
    }
}

static void hit(int p, int lane, int step, int vel)
{
    dr_step *s = &dpat[p].lane[lane].s[step];

    s->on = 1;
    s->p[DR_VEL] = (int8_t)vel;
}

static void hit_r(int p, int lane, int step, int vel, int ratch)
{
    hit(p, lane, step, vel);
    dpat[p].lane[lane].s[step].p[DR_RATCH] = (int8_t)ratch;
}

static void hits(int p, int lane, int from, int every, int vel)     /* every `every` steps */
{
    int s;

    for (s = from; s < dpat[p].lane[lane].len; s += every)
        hit(p, lane, s, vel);
}

static void chain(int c, const int *pats, const int *tsp, int n)    /* pats < 0 = none */
{
    int r;

    for (r = 0; r < n; r++) {
        song.chain[c][r].pat = (uint8_t)pats[r];
        song.chain[c][r].tsp = (int8_t)(tsp ? tsp[r] : 0);
        song.chain[c][r].snd = PS_EMPTY;
    }
}

static void chain1(int c, int p, int times)
{
    int pats[PS_CROWS], i;

    for (i = 0; i < times; i++)
        pats[i] = p;
    chain(c, pats, 0, times);
}

static void track(int t, int type, int snd, int level, int voices)
{
    ps_track *k = &song.trk[t];

    k->type = (uint8_t)type;
    k->sound = (uint8_t)snd;
    k->level = (int8_t)level;
    k->voices = (uint8_t)voices;
    k->eco = 0;
    k->loop = 1;
}

static void rows(const int *cells, int nrows, int ntracks)          /* cells[r * ntracks + t] */
{
    int r, t;

    for (r = 0; r < nrows; r++)
        for (t = 0; t < ntracks; t++)
            song.cell[r][t] = (uint8_t)cells[r * ntracks + t];
}

static void song_new(void)
{
    int p;

    py_song_clear(&song);
    for (p = 0; p < PSL_PATS; p++) {
        nr_pattern_init(&npat[p]);
        dr_pattern_init(&dpat[p]);
    }
    memset(sounds, 0, sizeof(sounds));
    snd_set = 0;
    swing[0] = swing[1] = 0;
}

#define SIL     13                              /* pattern 14: left empty, one silent bar */

/* ------------------------------------------------------------------ 1: WAREHOUSE (techno) */

static void demo_techno(void)
{
    static const int kit[8] = { KICK, CH, OH, CLAP, RIDE, SNARE, SHAKER, CRASH };
    enum { LK, LCH, LOH, LCL, LRD, LSN, LSH, LCR };
    static const uint8_t acid[AC_NP] = { 100, 175, 150, 150, 45, 13, 1, 1, 0, 0, 12, 16, 15, 0, 0, 4, 0, 2, 0, 10 };
    int16_t k[8];
    int p, i;

    song_new();
    /* sounds: 5 = ACID 3 (ACID LINE, accents from velocity), 6 = STATION 1 UNIVERSE pad,
     * 7 = STATION 1 DIGI BELL */
    acid_knobs(k, acid);
    sound(SND0, 0, k, ENV_PLUCK);
    sound_m1(SND0 + 1, 0, &M1_UNIVERSE, ENV_PAD);
    sound_m1(SND0 + 2, -1, &M1_DIGIBELL, ENV_BELL);   /* (its layer plays an octave up) */

    /* drums (1 bar each): 0 intro, 1 main, 2 main + ride, 3 break, 4 fill, 5 main + crash */
    for (p = 0; p < 6; p++)
        dpat_new(p, 16, kit, 8);
    hits(0, LK, 0, 4, 100);
    hits(0, LCH, 2, 4, 70);
    for (p = 1; p <= 5; p++) {
        if (p != 3)
            hits(p, LK, 0, 4, 100);
        hits(p, LCH, 0, 1, 35);
        for (i = 0; i < 16; i += 2)
            hit(p, LCH, i, 50);                 /* 16ths, the 8ths louder */
        if (p != 3)
            hits(p, LOH, 2, 4, 75);
        hit(p, LCL, 4, 90);
        hit(p, LCL, 12, 90);
    }
    hits(2, LRD, 0, 4, 55);
    hits(3, LSH, 0, 1, 40);
    hits(3, LOH, 14, 16, 70);
    for (i = 0; i < 16; i++)                    /* 4: the snare rolls in over the last beat */
        dpat[4].lane[LCH].s[i].on = i < 12 && dpat[4].lane[LCH].s[i].on;
    dpat[4].lane[LK].s[12].on = 0;
    hit(4, LSN, 10, 60);
    hit_r(4, LSN, 12, 70, 2);
    hit_r(4, LSN, 13, 80, 2);
    hit_r(4, LSN, 14, 90, 3);
    hit_r(4, LSN, 15, 100, 4);
    hit(5, LCR, 0, 90);

    /* 303 lines (1 bar): accents vel 100, slides: a note held into the next one (legato) */
    npat_len(0, 16);
    n_add(0, 0, note("A2"), 100, L1);
    n_add(0, 2, note("A2"), 70, L1);
    n_add(0, 3, note("A3"), 85, L15);           /* slides into ... */
    n_add(0, 4, note("G3"), 70, L1);
    n_add(0, 6, note("E3"), 100, L1);
    n_add(0, 8, note("A2"), 70, L1);
    n_add(0, 10, note("C3"), 100, L1);
    n_add(0, 12, note("D#3"), 75, L15);
    n_add(0, 13, note("E3"), 75, L1);
    n_add(0, 15, note("G2"), 70, L_HALF);
    npat_len(1, 16);
    n_add(1, 0, note("A2"), 100, L1);
    n_add(1, 1, note("A2"), 65, L_HALF);
    n_add(1, 3, note("C3"), 75, L1);
    n_add(1, 4, note("A2"), 70, L1);
    n_add(1, 6, note("A3"), 100, L15);
    n_add(1, 7, note("G3"), 80, L1);
    n_add(1, 8, note("A2"), 70, L1);
    n_add(1, 10, note("F3"), 85, L15);
    n_add(1, 11, note("E3"), 75, L1);
    n_add(1, 12, note("A2"), 100, L1);
    n_add(1, 14, note("C4"), 80, L1);
    n_add(1, 15, note("A2"), 65, L_HALF);

    /* pad (4 bars): Am9 Am9 Fmaj7 G6 */
    {
        static const int am9[4] = { 57, 60, 64, 71 }, fmaj7[4] = { 53, 57, 60, 64 }, g6[4] = { 55, 59, 62, 64 };

        npat_len(2, 64);
        n_chord(2, 0, am9, 4, 60, L16);
        n_chord(2, 16, am9, 4, 55, L16);
        n_chord(2, 32, fmaj7, 4, 60, L16);
        n_chord(2, 48, g6, 4, 60, L16);
    }
    /* bell motif (4 bars), answered an octave down */
    npat_len(3, 64);
    n_add(3, 0, note("E5"), 80, L2);
    n_add(3, 3, note("D5"), 60, L2);
    n_add(3, 6, note("A4"), 75, L4);
    n_add(3, 14, note("C5"), 60, L2);
    n_add(3, 16, note("B4"), 70, L4);
    n_add(3, 22, note("G4"), 60, L6);
    n_add(3, 32, note("E5"), 80, L2);
    n_add(3, 35, note("D5"), 60, L2);
    n_add(3, 38, note("C5"), 75, L4);
    n_add(3, 46, note("A4"), 60, L2);
    n_add(3, 48, note("B4"), 70, L4);
    n_add(3, 54, note("E4"), 65, L8);

    /* chains (each 4 bars) */
    chain1(0, 0, 4);                                             /* drums intro */
    { static const int c[4] = { 1, 1, 1, 4 }; chain(1, c, 0, 4); }
    { static const int c[4] = { 2, 2, 2, 4 }; chain(2, c, 0, 4); }
    { static const int c[4] = { 3, 3, 3, 4 }; chain(3, c, 0, 4); }
    { static const int c[4] = { 5, 1, 1, 4 }; chain(4, c, 0, 4); }
    { static const int c[4] = { 0, 0, 0, 1 }, t[4] = { 0, 0, 0, 0 }; chain(10, c, t, 4); }   /* acid */
    { static const int c[4] = { 0, 1, 0, 1 }, t[4] = { 0, 0, -4, -2 }; chain(11, c, t, 4); } /* A A F G */
    chain1(20, SIL, 4);                                          /* 4 silent bars (any track type) */
    chain1(30, 2, 1);                                            /* pad */
    chain1(40, 3, 1);                                            /* bell */

    track(0, PS_DRUM, KIT, -4, 0);
    track(1, PS_SYNTH, SND0, -6, 1);
    track(2, PS_SYNTH, SND0 + 1, -12, 4);
    track(3, PS_SYNTH, SND0 + 2, -12, 2);
    {
        static const int cells[] = {
            /* drums  acid  pad  bell */
            0,  20, 30, 20,
            0,  10, 20, 20,
            1,  10, 20, 20,
            2,  11, 30, 20,
            3,  20, 30, 40,
            4,  10, 30, 40,
            2,  11, 30, 40,
            1,  10, 20, 20,
        };
        rows(cells, 8, 4);
    }
    swing[0] = swing[1] = 0;
}

/* ------------------------------------------------------------------ 2: PIANO HOUSE */

static void demo_house(void)
{
    static const int kit[8] = { KICK, CLAP, OH, CH, SHAKER, TAMB, CONGA1, SNARE };
    enum { LK, LCL, LOH, LCH, LSH, LTB, LCG, LSN };
    /* Bbmaj7 C Am7 Dm7 (F major: IV V iii vi), close voicings */
    static const int ch[4][4] = { { 58, 62, 65, 69 }, { 60, 64, 67, 72 }, { 57, 60, 64, 67 }, { 57, 60, 62, 65 } };
    static const int root[4] = { 46, 48, 45, 50 };   /* Bb2 C3 A2 D3 */
    int p, b, i;

    song_new();
    sound_m1(SND0, 1, &M1_PIANO, ENV_KEYS);       /* 5 = M1 PIANO (its layer an octave down: +1) */
    sound_m1(SND0 + 1, 0, &M1_HORGAN, ENV_BASS);  /* 6 = HOUSE ORGAN as the bass (an octave down) */
    sound_m1(SND0 + 2, 0, &M1_STRINGS, ENV_PAD);  /* 7 = STRINGS */

    /* drums: 0 intro (kick + shaker), 1 main, 2 main + percussion, 3 break, 4 fill, 5 crash
     * (pattern 5's tambourine lane plays the crash instead) */
    for (p = 0; p < 6; p++)
        dpat_new(p, 16, kit, 8);
    hits(0, LK, 0, 4, 100);
    hits(0, LSH, 0, 1, 35);
    for (p = 1; p <= 5; p++) {
        if (p != 3)
            hits(p, LK, 0, 4, 100);
        hit(p, LCL, 4, 85);
        hit(p, LCL, 12, 85);
        hits(p, LOH, 2, 4, 70);
        hits(p, LCH, 0, 2, 30);
        hits(p, LSH, 1, 2, 30);
    }
    hit(2, LTB, 4, 55);
    hit(2, LTB, 12, 55);
    hit(2, LCG, 3, 60);
    hit(2, LCG, 7, 50);
    hit(2, LCG, 10, 60);
    hit(2, LCG, 14, 45);
    dpat[4].lane[LK].s[12].on = 0;
    for (i = 12; i < 16; i++)
        hit_r(4, LSN, i, 60 + 10 * (i - 12), 2);
    dpat[5].lane[LTB].key = CRASH;                /* 5: a crash on the one (lane 6 = crash) */
    hit(5, LTB, 0, 85);
    swing[1] = 20;                                /* DRUM ROLL swing: a little shuffle */

    /* piano stabs (4 bars): 1, the "and" of 1 ... syncopated, a pickup into the next bar */
    npat_len(0, 64);
    for (b = 0; b < 4; b++) {
        static const int at[5] = { 0, 3, 6, 10, 14 }, len[5] = { L2, L2, L3, L2, L1 }, vel[5] = { 90, 70, 85, 75, 60 };

        for (i = 0; i < 5; i++)
            n_chord(0, b * 16 + at[i], ch[i == 4 ? (b + 1) % 4 : b], 4, vel[i], len[i]);
    }
    /* organ bass (4 bars): root, octave, fifth, octave jumps */
    npat_len(1, 64);
    for (b = 0; b < 4; b++) {
        static const int at[7] = { 0, 3, 6, 8, 10, 11, 14 }, off[7] = { 0, 12, 0, 7, 12, 0, 12 },
                         len[7] = { L2, L1, L2, L1, L1, L1, L1 }, vel[7] = { 100, 75, 90, 70, 80, 65, 75 };

        for (i = 0; i < 7; i++)
            n_add(1, b * 16 + at[i], root[b] + off[i], vel[i], len[i]);
    }
    /* strings (4 bars): the chords held, an octave down */
    npat_len(2, 64);
    for (b = 0; b < 4; b++)
        for (i = 0; i < 4; i++)
            n_add(2, b * 16, ch[b][i] - 12, 55, L16);

    chain1(0, 0, 4);
    { static const int c[4] = { 1, 1, 1, 4 }; chain(1, c, 0, 4); }
    { static const int c[4] = { 2, 2, 2, 4 }; chain(2, c, 0, 4); }
    { static const int c[4] = { 5, 2, 2, 4 }; chain(3, c, 0, 4); }
    chain1(4, 3, 4);
    { static const int c[4] = { 3, 3, 3, 4 }; chain(5, c, 0, 4); }
    chain1(10, 0, 1);                             /* piano */
    chain1(11, 1, 1);                             /* bass */
    chain1(12, 2, 1);                             /* strings */
    chain1(20, SIL, 4);

    track(0, PS_DRUM, KIT, -4, 0);
    track(1, PS_SYNTH, SND0, -10, 4);
    track(2, PS_SYNTH, SND0 + 1, -6, 1);
    track(3, PS_SYNTH, SND0 + 2, -14, 4);
    {
        static const int cells[] = {
            /* drums piano bass strings */
            0, 20, 20, 12,
            1, 10, 20, 20,
            2, 10, 11, 20,
            3, 10, 11, 12,
            4, 20, 20, 12,
            5, 10, 20, 12,
            3, 10, 11, 12,
            2, 10, 11, 20,
        };
        rows(cells, 8, 4);
    }
    swing[0] = 20;
}

/* ------------------------------------------------------------------ 3: RADIO (pop) */

static void demo_pop(void)
{
    static const int kit[8] = { KICK, SNARE, CH, OH, CLAP, TOM1, TOM2, ETOM };
    enum { LK, LSN, LCH, LOH, LCL, LT1, LT2, LT3 };
    /* G D Em C (I V vi IV) */
    static const int ch[4][3] = { { 55, 59, 62 }, { 54, 57, 62 }, { 55, 59, 64 }, { 55, 60, 64 } };
    static const int root[4] = { 43, 38, 40, 36 };   /* G2 D2 E2 C2 */
    int p, b, i;

    song_new();
    sound_m1(SND0, 1, &M1_EPIANO, ENV_KEYS);      /* 5 = E.PIANO (+1: its layer an octave down) */
    sound_m1(SND0 + 1, 0, &M1_SBASS, ENV_BASS);   /* 6 = SYNTH BASS */
    sound_m1(SND0 + 2, 0, &M1_SAWLEAD, ENV_LEAD); /* 7 = SAW LEAD */
    sound_m1(SND0 + 3, 0, &M1_BELLPAD, ENV_PAD);  /* 8 = BELL PAD */

    /* drums: 0 verse, 1 chorus, 2 fill (toms), 3 intro (hats), 4 chorus + crash */
    for (p = 0; p < 5; p++)
        dpat_new(p, 16, kit, 8);
    for (p = 0; p <= 4; p++) {
        if (p != 3) {
            hit(p, LK, 0, 100);
            hit(p, LK, 8, 95);
            hit(p, LSN, 4, 90);
            hit(p, LSN, 12, 90);
        }
        hits(p, LCH, 0, 2, p == 3 ? 45 : 55);
    }
    hit(0, LK, 10, 70);
    for (p = 1; p <= 4; p += 3) {               /* chorus: more push, clap layered */
        hit(p, LK, 6, 75);
        hit(p, LK, 10, 80);
        hit(p, LCL, 4, 70);
        hit(p, LCL, 12, 70);
        hit(p, LOH, 14, 60);
    }
    dpat[2].lane[LSN].s[12].on = 0;
    for (i = 0; i < 4; i++)
        dpat[2].lane[LCH].s[8 + 2 * i].on = 0;
    hit(2, LT1, 8, 80); hit(2, LT1, 9, 70);
    hit(2, LT2, 10, 80); hit(2, LT2, 11, 70);
    hit(2, LT3, 12, 85); hit(2, LT3, 13, 75);
    hit_r(2, LSN, 14, 90, 2);
    hit_r(2, LSN, 15, 100, 2);
    dpat[4].lane[LT1].key = CRASH;              /* 4: lane 6 = crash on the one */
    hit(4, LT1, 0, 85);

    /* e.piano (4 bars): pushed 8ths */
    npat_len(0, 64);
    for (b = 0; b < 4; b++) {
        static const int at[6] = { 0, 3, 6, 8, 11, 14 }, len[6] = { L3, L3, L2, L3, L3, L2 },
                         vel[6] = { 85, 65, 70, 80, 65, 70 };

        for (i = 0; i < 6; i++)
            n_chord(0, b * 16 + at[i], ch[b], 3, vel[i], len[i]);
    }
    /* bass (4 bars): 8ths on the root, a passing note into the next chord */
    npat_len(1, 64);
    for (b = 0; b < 4; b++) {
        for (i = 0; i < 14; i += 2)
            n_add(1, b * 16 + i, root[b] + (i == 6 ? 12 : 0), i % 4 ? 70 : 90, L15);
        n_add(1, b * 16 + 14, root[(b + 1) % 4] + (root[(b + 1) % 4] > root[b] ? -1 : 2), 75, L15);
    }
    /* verse melody (4 bars) */
    npat_len(2, 64);
    {
        static const struct { int8_t at, len; const char *n; } v[] = {
            { 0, L2, "B4" }, { 2, L2, "D5" }, { 4, L2, "D5" }, { 6, L2, "B4" }, { 8, L2, "A4" }, { 10, L2, "G4" },
            { 12, L4, "A4" },
            { 16, L2, "F#4" }, { 18, L2, "A4" }, { 20, L2, "D5" }, { 22, L2, "C#5" }, { 24, L4, "A4" },
            { 28, L4, "F#4" },
            { 32, L2, "G4" }, { 34, L2, "B4" }, { 36, L4, "E5" }, { 40, L2, "D5" }, { 42, L2, "B4" },
            { 44, L4, "G4" },
            { 48, L2, "E4" }, { 50, L2, "G4" }, { 52, L4, "C5" }, { 56, L2, "B4" }, { 58, L2, "A4" },
            { 60, L4, "G4" },
        };
        for (i = 0; i < (int)(sizeof(v) / sizeof(v[0])); i++)
            n_add(2, v[i].at, note(v[i].n), 80, v[i].len);
    }
    /* chorus melody (4 bars), higher */
    npat_len(3, 64);
    {
        static const struct { int8_t at, len; const char *n; } v[] = {
            { 0, L4, "D5" }, { 4, L2, "D5" }, { 6, L2, "E5" }, { 8, L4, "D5" }, { 12, L4, "B4" },
            { 16, L4, "A4" }, { 20, L2, "A4" }, { 22, L2, "B4" }, { 24, L4, "A4" }, { 28, L4, "F#4" },
            { 32, L2, "G4" }, { 34, L2, "A4" }, { 36, L4, "B4" }, { 40, L4, "E5" }, { 44, L4, "D5" },
            { 48, L4, "C5" }, { 52, L2, "B4" }, { 54, L2, "A4" }, { 56, L8, "G4" },
        };
        for (i = 0; i < (int)(sizeof(v) / sizeof(v[0])); i++)
            n_add(3, v[i].at, note(v[i].n), 90, v[i].len);
    }
    /* bell pad (4 bars): the chords held, an octave up */
    npat_len(4, 64);
    for (b = 0; b < 4; b++)
        for (i = 0; i < 3; i++)
            n_add(4, b * 16, ch[b][i] + 12, 50, L16);

    chain1(0, 3, 4);
    { static const int c[4] = { 0, 0, 0, 2 }; chain(1, c, 0, 4); }
    { static const int c[4] = { 4, 1, 1, 2 }; chain(2, c, 0, 4); }
    chain1(10, 0, 1);
    chain1(11, 1, 1);
    chain1(12, 2, 1);
    chain1(13, 3, 1);
    chain1(14, 4, 1);
    chain1(20, SIL, 4);

    track(0, PS_DRUM, KIT, -4, 0);
    track(1, PS_SYNTH, SND0, -11, 4);
    track(2, PS_SYNTH, SND0 + 1, -7, 1);
    track(3, PS_SYNTH, SND0 + 2, -9, 1);
    track(4, PS_SYNTH, SND0 + 3, -16, 4);
    {
        static const int cells[] = {
            /* drums ep  bass lead pad */
            0, 10, 20, 20, 20,
            1, 10, 11, 12, 20,
            1, 10, 11, 12, 20,
            2, 10, 11, 13, 14,
            2, 10, 11, 13, 14,
            0, 10, 20, 20, 14,
            2, 10, 11, 13, 14,
            2, 10, 11, 13, 14,
        };
        rows(cells, 8, 5);
    }
}

/* ------------------------------------------------------------------ 4: PRELUDE (Bach, BWV 846) */

static void demo_classic(void)
{
    /* bars 1-19: the five notes of each bar's broken chord (the score's left hand two, right hand
     * three), then the closing C major chord */
    static const char *const bar[20][5] = {
        { "C4", "E4", "G4", "C5", "E5" }, { "C4", "D4", "A4", "D5", "F5" }, { "B3", "D4", "G4", "D5", "F5" },
        { "C4", "E4", "G4", "C5", "E5" }, { "C4", "E4", "A4", "E5", "A5" }, { "C4", "D4", "F#4", "A4", "D5" },
        { "B3", "D4", "G4", "D5", "G5" }, { "B3", "C4", "E4", "G4", "C5" }, { "A3", "C4", "E4", "G4", "C5" },
        { "D3", "A3", "D4", "F#4", "C5" }, { "G3", "B3", "D4", "G4", "B4" }, { "G3", "Bb3", "E4", "G4", "C#5" },
        { "F3", "A3", "D4", "A4", "D5" }, { "F3", "Ab3", "D4", "F4", "B4" }, { "E3", "G3", "C4", "G4", "C5" },
        { "E3", "F3", "A3", "C4", "F4" }, { "D3", "F3", "A3", "C4", "F4" }, { "G2", "D3", "G3", "B3", "F4" },
        { "C3", "E3", "G3", "C4", "E4" }, { "C3", "G3", "C4", "E4", "G4" },
    };
    int b, h, i, k[5];

    song_new();
    sound_m1(SND0, 1, &M1_HARPSI, ENV_PLUCK);      /* 5 = HARPSICHORD (+1: its layer an octave down) */
    sound_m1(SND0 + 1, 0, &M1_STRINGS, ENV_PAD);   /* 6 = STRINGS */

    /* harpsichord: 5 patterns of 4 bars; each half bar n1 (held) n2 (held) n3 n4 n5 n3 n4 n5 */
    for (b = 0; b < 20; b++) {
        int p = b / 4, s0 = (b % 4) * 16;

        if (b % 4 == 0)
            npat_len(p, 64);
        for (i = 0; i < 5; i++)
            k[i] = note(bar[b][i]);
        if (b == 19) {                          /* the last bar: the chord, rolled */
            for (i = 0; i < 5; i++)
                n_add(p, s0 + i, k[i], 85 - 3 * i, L12);
            continue;
        }
        for (h = 0; h < 2; h++) {
            int s = s0 + 8 * h;

            n_add(p, s, k[0], 80, L8);
            n_add(p, s + 1, k[1], 70, L6);
            for (i = 0; i < 6; i++)
                n_add(p, s + 2 + i, k[2 + i % 3], i % 3 ? 68 : 75, L1);
        }
    }
    /* strings: each bar's bass note an octave down + the fifth note, the whole bar */
    for (b = 0; b < 20; b++) {
        int p = 5 + b / 4, s0 = (b % 4) * 16;

        if (b % 4 == 0)
            npat_len(p, 64);
        n_add(p, s0, note(bar[b][0]) - 12, 50, L16);
        n_add(p, s0, note(bar[b][1]), 40, L16);
    }
    {
        static const int c[5] = { 0, 1, 2, 3, 4 }, s[5] = { 5, 6, 7, 8, 9 };

        chain(0, c, 0, 5);
        chain(1, s, 0, 5);
    }
    track(0, PS_SYNTH, SND0, -6, 4);
    track(1, PS_SYNTH, SND0 + 1, -16, 4);
    {
        static const int cells[] = { 0, 1 };

        rows(cells, 1, 2);
    }
}

/* ------------------------------------------------------------------ the files */

static uint8_t fbuf[200000];
static uint32_t flen;
static nr_pattern scratch_n;                    /* (the larger kind: dr_pattern) */
static dr_pattern scratch_d;
static nr_pattern got_n[PSL_PATS];
static dr_pattern got_d[PSL_PATS];
static uint8_t got_s[PSL_SOUNDS][PSL_SND];
static uint8_t got_smask;
static uint8_t songbuf[PS_FILE_BYTES];

static int io_get(void *ctx, int kind, int p, void *out)
{
    (void)ctx;
    memcpy(out, kind == PSL_DR ? (void *)&dpat[p] : (void *)&npat[p], kind == PSL_DR ? sizeof(dr_pattern) : sizeof(nr_pattern));
    return 0;
}

static void io_put(void *ctx, int kind, int p, const void *in)
{
    (void)ctx;
    if (kind == PSL_DR)
        memcpy(&got_d[p], in, sizeof(dr_pattern));
    else
        memcpy(&got_n[p], in, sizeof(nr_pattern));
}

static int io_write(void *ctx, const void *buf, uint32_t n)
{
    (void)ctx;
    if (flen + n > sizeof(fbuf))
        return -1;
    memcpy(fbuf + flen, buf, n);
    flen += n;
    return 0;
}

static int io_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    (void)ctx;
    if (off >= flen)
        return 0;
    if (n > flen - off)
        n = flen - off;
    memcpy(buf, fbuf + off, n);
    return (int)n;
}

static int io_sget(void *ctx, int s, uint8_t *rec)
{
    (void)ctx;
    memcpy(rec, sounds[s], PSL_SND);
    return 0;
}

static void io_sput(void *ctx, int s, const uint8_t *rec)
{
    (void)ctx;
    memcpy(got_s[s], rec, PSL_SND);
    got_smask |= (uint8_t)(1u << s);
}

static int io_sok(void *ctx, int s, const uint8_t *rec)
{
    (void)ctx;
    (void)s;
    return rec[0] == DRWAVE && rec[2] != 0;
}

static int write_demo(const char *dir, const char *file, const char *title)
{
    static ps_song back;
    uint8_t sw[2];
    psl_io io;
    psl_info info;
    char path[512];
    FILE *f;
    int r, p, s, notes = 0, drum_hits = 0;

    memset(&io, 0, sizeof(io));
    io.size[PSL_NR] = sizeof(nr_pattern);
    io.size[PSL_DR] = sizeof(dr_pattern);
    io.empty[PSL_NR] = &n_empty;
    io.empty[PSL_DR] = &d_empty;
    io.pat = sizeof(dr_pattern) >= sizeof(nr_pattern) ? (void *)&scratch_d : (void *)&scratch_n;
    io.get = io_get;
    io.put = io_put;
    io.write = io_write;
    io.read = io_read;
    io.cur_sound = 0;
    io.sget = io_sget;
    io.sput = io_sput;
    io.sok = io_sok;
    flen = 0;
    r = py_slot_save(&song, swing, &io, songbuf);
    if (r != PSL_OK) {
        fprintf(stderr, "%s: save %d\n", file, r);
        return 1;
    }
    /* read back as PLAY does: check, then load (nothing kept) and compare */
    memset(got_n, 0, sizeof(got_n));
    memset(got_d, 0, sizeof(got_d));
    memset(got_s, 0, sizeof(got_s));
    got_smask = 0;
    r = py_slot_check(&io, &info);
    if (r == PSL_OK)
        r = py_slot_load(&back, sw, &io, songbuf, 0);
    if (r != PSL_OK || memcmp(&back, &song, sizeof(song)) || sw[0] != swing[0] || sw[1] != swing[1]
        || memcmp(got_n, npat, sizeof(npat)) || memcmp(got_d, dpat, sizeof(dpat)) || info.smask != snd_set
        || got_smask != snd_set) {
        fprintf(stderr, "%s: read back differs (r %d, smask %02x/%02x/%02x)\n", file, r, info.smask, got_smask, snd_set);
        return 1;
    }
    for (s = 0; s < PSL_SOUNDS; s++) {
        if (!(snd_set >> s & 1))
            continue;
        if (memcmp(got_s[s], sounds[s], PSL_SND) || !got_s[s][2]) {
            fprintf(stderr, "%s: sound %d\n", file, s + 1);
            return 1;
        }
        {   /* drwave's knob ranges (TE's boot check): slot 1 0..24575, 2-3 0..16383, 4-8 0..32767 */
            static const int16_t max[8] = { 24575, 16383, 16383, 32767, 32767, 32767, 32767, 32767 };
            int i;

            for (i = 0; i < 8; i++) {
                int16_t v = (int16_t)(got_s[s][4 + 2 * i] | got_s[s][5 + 2 * i] << 8);

                if (v < 0 || v > max[i]) {
                    fprintf(stderr, "%s: sound %d knob %d = %d out of range\n", file, s + 1, i + 1, v);
                    return 1;
                }
            }
        }
    }
    for (p = 0; p < PSL_PATS; p++) {
        int st, v, l;

        for (st = 0; st < NR_STEPS; st++)
            notes += nr_voices(&npat[p].s[st]);
        for (l = 0; l < DR_LANES; l++)
            for (v = 0; v < DR_STEPS; v++)
                drum_hits += dpat[p].lane[l].s[v].on;
    }
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    f = fopen(path, "wb");
    if (!f || fwrite(fbuf, 1, flen, f) != flen || fclose(f)) {
        fprintf(stderr, "%s: cannot write\n", path);
        return 1;
    }
    printf("%s  %-12s %6u bytes, %d song rows, %d chains, notes %d, drum steps %d, sounds %02x\n",
           file, title, flen, info.rows, info.chains, notes, drum_hits, snd_set);
    return 0;
}

/* ------------------------------------------------------------------ preview (rough WAV)
 *
 * Not PLAY's sound: plain waveforms by the sound's role (pluck / keys / pad / bass / lead / bell)
 * and noise / sine drums by kit key, to hear the notes, harmony and arrangement on a computer. The
 * song walked as PLAY walks it: every column its chains, each chain row its pattern once. */
#define PV_SR   44100
#define PV_MAX  (PV_SR * 150)
static float pv[PV_MAX];
static uint32_t pv_len, pv_seed = 1;

static float frand(void) { pv_seed = pv_seed * 1664525u + 1013904223u; return (float)(int32_t)pv_seed / 2147483648.0f; }

static void pv_note(double t0, double dur, int key, int vel, const int16_t *env, float gain)
{
    double f = 440.0 * pow(2.0, (key - 69) / 12.0), ph = 0;
    uint32_t i0 = (uint32_t)(t0 * PV_SR), n = (uint32_t)((dur + 0.6) * PV_SR), i;
    float a = gain * vel / 100.0f, lp = 0;
    int pad = env == ENV_PAD, bass = env == ENV_BASS, lead = env == ENV_LEAD, bell = env == ENV_BELL,
        keys = env == ENV_KEYS;

    for (i = 0; i < n && i0 + i < PV_MAX; i++) {
        double t = (double)i / PV_SR, e;
        float x;

        e = pad ? (t < 0.25 ? t / 0.25 : 1.0) : bell ? exp(-t * 3.0) : keys ? exp(-t * 1.6) : exp(-t * (bass ? 1.0 : 2.5));
        if (t > dur)
            e *= exp(-(t - dur) * (pad ? 3.0 : 12.0));
        ph += f / PV_SR;
        ph -= floor(ph);
        if (bell)
            x = (float)(sin(2 * M_PI * ph) + 0.3 * sin(2 * M_PI * 3.01 * ph * 1.0));
        else if (keys)
            x = (float)(sin(2 * M_PI * ph) + 0.25 * sin(4 * M_PI * ph));
        else if (lead)
            x = ph < 0.5 ? 0.6f : -0.6f;
        else {
            x = (float)(2 * ph - 1);            /* saw, low-passed (pluck: by the envelope) */
            lp += (x - lp) * (float)(pad ? 0.08 : bass ? 0.12 : 0.05 + 0.4 * e);
            x = lp;
        }
        pv[i0 + i] += a * (float)e * x * 0.25f;
    }
    if (i0 + i > pv_len)
        pv_len = i0 + i;
}

static void pv_drum(double t0, int key, int vel, float gain)
{
    uint32_t i0 = (uint32_t)(t0 * PV_SR), n = PV_SR / 2, i;
    float a = gain * vel / 100.0f, hp = 0, prev = 0;
    double ph = 0;

    for (i = 0; i < n && i0 + i < PV_MAX; i++) {
        double t = (double)i / PV_SR;
        float x = 0, nz = frand(), h = nz - prev;

        prev = nz;
        hp += (h - hp) * 0.5f;
        if (key <= KICK2) {
            ph += (45 + 120 * exp(-t * 30)) / PV_SR;
            x = (float)(sin(2 * M_PI * ph) * exp(-t * 7));
        } else if (key == SNARE || key == SNARE2 || key == STICK) {
            ph += 190.0 / PV_SR;
            x = (float)(0.5 * sin(2 * M_PI * ph) * exp(-t * 20) + 0.6 * nz * exp(-t * 15));
        } else if (key == CLAP) {
            x = (float)(nz * (exp(-t * 18) + (t > 0.01 ? 0.6 * exp(-(t - 0.01) * 25) : 0)) * 0.7);
        } else if (key == CH || key == CH2 || key == SHAKER || key == TAMB) {
            x = (float)(hp * exp(-t * (key == SHAKER ? 45 : 60)) * 0.8);
        } else if (key == OH || key == RIDE || key == CRASH) {
            x = (float)(hp * exp(-t * (key == CRASH ? 3 : 9)) * 0.6);
        } else {                                /* toms, congas */
            ph += (key == CONGA1 || key == CONGA2 ? 300.0 : 110.0 + 25 * (key - TOM1)) / PV_SR;
            x = (float)(sin(2 * M_PI * ph) * exp(-t * 9));
        }
        pv[i0 + i] += a * x * 0.5f;
    }
    if (i0 + i > pv_len)
        pv_len = i0 + i;
}

static double db(int level) { return pow(10.0, level / 20.0); }

static void preview(const char *dir, const char *file, int bpm)
{
    double step = 60.0 / bpm / 4, t_row = 0;
    char path[512];
    FILE *f;
    int r, t;
    uint32_t i;
    float peak = 0;

    memset(pv, 0, sizeof(pv));
    pv_len = 0;
    for (r = 0; r < PS_ROWS; r++) {
        double longest = 0;
        int any = 0;

        for (t = 0; t < PS_TRACKS; t++) {
            const ps_track *k = &song.trk[t];
            int c = song.cell[r][t], cr;
            double at = t_row;

            if (k->type != PS_SYNTH && k->type != PS_DRUM)
                continue;
            if (c == PS_EMPTY)
                continue;
            any = 1;
            if (c == PS_REST) {
                at += 16 * step;
            } else {
                for (cr = 0; cr < PS_CROWS && song.chain[c][cr].pat != PS_EMPTY; cr++) {
                    int p = song.chain[c][cr].pat, tsp = song.chain[c][cr].tsp, st, v, l, len;

                    if (k->type == PS_SYNTH) {
                        int sn = k->sound, oct = (int8_t)sounds[sn][1];

                        len = npat[p].len;
                        for (st = 0; st < len; st++)
                            for (v = 0; v < NR_VOICES; v++) {
                                const nr_note *n = &npat[p].s[st].n[v];

                                if (n->key == NR_NONE)
                                    continue;
                                pv_note(at + st * step, nr_len8[n->len] / 8.0 * step,
                                        n->key + 12 * oct + tsp + pv_shift[sn], n->vel, pv_env[sn],
                                        (float)db(k->level + 9));
                            }
                    } else {
                        const dr_pattern *d = &dpat[p];

                        len = 1;
                        for (l = 0; l < DR_LANES; l++)
                            if (d->lane[l].len > len)
                                len = d->lane[l].len;
                        for (l = 0; l < DR_LANES; l++)
                            for (st = 0; st < d->lane[l].len; st++) {
                                const dr_step *s = &d->lane[l].s[st];
                                int h, rt = s->p[DR_RATCH] < 1 ? 1 : s->p[DR_RATCH];

                                if (!s->on)
                                    continue;
                                for (h = 0; h < rt; h++)
                                    pv_drum(at + (st + (double)h / rt) * step, d->lane[l].key, s->p[DR_VEL],
                                            (float)db(k->level + 9) * d->lane[l].level / 100.0f);
                            }
                    }
                    at += len * step;
                }
            }
            if (at - t_row > longest)
                longest = at - t_row;
        }
        if (!any)
            break;
        t_row += longest;
    }
    for (i = 0; i < pv_len; i++)
        if (fabsf(pv[i]) > peak)
            peak = fabsf(pv[i]);
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    f = fopen(path, "wb");
    if (!f)
        return;
    {
        uint32_t bytes = pv_len * 2, v32;
        uint16_t v16;

        fwrite("RIFF", 1, 4, f); v32 = 36 + bytes; fwrite(&v32, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
        v32 = 16; fwrite(&v32, 4, 1, f); v16 = 1; fwrite(&v16, 2, 1, f); fwrite(&v16, 2, 1, f);
        v32 = PV_SR; fwrite(&v32, 4, 1, f); v32 = PV_SR * 2; fwrite(&v32, 4, 1, f);
        v16 = 2; fwrite(&v16, 2, 1, f); v16 = 16; fwrite(&v16, 2, 1, f);
        fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
        for (i = 0; i < pv_len; i++) {
            int16_t o = (int16_t)(pv[i] / (peak > 0 ? peak : 1) * 29000);

            fwrite(&o, 2, 1, f);
        }
    }
    fclose(f);
    printf("  preview %s (%.0f s at %d BPM)\n", path, (double)pv_len / PV_SR, bpm);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".", *pdir = argc > 2 ? argv[2] : 0;
    int bad = 0;

    nr_pattern_init(&n_empty);
    dr_pattern_init(&d_empty);
    demo_techno();
    bad |= write_demo(dir, "SONG13.PSL", "WAREHOUSE");
    if (pdir)
        preview(pdir, "warehouse.wav", 128);
    demo_house();
    bad |= write_demo(dir, "SONG14.PSL", "PIANO HOUSE");
    if (pdir)
        preview(pdir, "piano_house.wav", 124);
    demo_pop();
    bad |= write_demo(dir, "SONG15.PSL", "RADIO");
    if (pdir)
        preview(pdir, "radio.wav", 112);
    demo_classic();
    bad |= write_demo(dir, "SONG16.PSL", "PRELUDE");
    if (pdir)
        preview(pdir, "prelude.wav", 76);
    return bad;
}
