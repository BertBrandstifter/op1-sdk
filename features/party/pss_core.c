/* PSS voice (see pss_core.h, README.md). Own code; the instrument structure: two multisound
 * oscillators, a low-pass VDF with its envelope. The AWM part (GEW 8/12-bit samples, the GEW7
 * envelope and LFO) follows MAME's gew.cpp / gew7.cpp (BSD-3-Clause, see NOTICE): the same rate
 * steps, levels and LFO shapes, computed once per 32 output samples instead of per chip sample
 * (gen_pss11.py gew_eg() is the per-sample reference the tests compare with).
 *
 * Per voice block: osc A and B decode the source samples the block needs (ADPCM: 16 samples per
 * 9-byte block, predictor history restored at the loop start), then interpolate them linearly at
 * 88.2 kHz, at the block's mean TE increment. High notes play an octave-down copy of the sample
 * (pre-rendered by gen_pss.py, chosen at note on), so the step stays <= 1 (single cycles) / 2 there. Levels (A decay,
 * B fade-in, crossfaded mix) ramp per 32 samples; a voice peaks at about half scale, so chords leave
 * headroom in TE's voice mix, and the output has a soft knee (as WHEEL) rather than a hard clip.
 * Filter: trapezoidal state-variable low-pass (Simper), coefficients four
 * times per block; off when the cutoff is fully open with no envelope and no resonance. */
#include "pss_core.h"
#include "pss_tables.h"

/* the multisounds: from the PSS-11 ROM when gen_pss11.py --pss-rom made one; the benches use the built-in.
 * The sample data is a file (PSS_ROM_FILE) that pss.c loads at runtime (pss_set_rom); the
 * host tests (not __bfin__) and the sample bench (-DPSS_ROM_INLINE) build it in instead. */
#if !defined(__bfin__) && !defined(PSS_ROM_INLINE)
#define PSS_ROM_INLINE
#endif
#if defined(PSS_DATA_INC)                       /* another data set (run_awmtest.py: a synthetic GEW7 ROM) */
#include PSS_DATA_INC
#ifdef PSS_ROM_INLINE
#include PSS_ROM_INC
#endif
#define PSS_DATA_DONE
#elif !defined(SDK_BENCH) && defined(__has_include)
#if __has_include("build/pss_data.inc")
#include "build/pss_data.inc"
#ifdef PSS_ROM_INLINE
#include "build/pss_rom.inc"
#endif
#define PSS_DATA_DONE
#endif
#endif
#ifndef PSS_DATA_DONE
#include "pss_builtin.inc"
#endif
#if defined(PSS_ROM_FILE) && !defined(PSS_ROM_INLINE)
#define PSS_ROM_RUNTIME
static const uint8_t *volatile pss_rom;          /* 0 until loaded: notes stay silent */
#endif

#define FMT_ADPCM   0
#define FMT_PCPSS6   1
#define FMT_GEW8    2                           /* the chip's: signed 8-bit */
#define FMT_GEW12   3                           /* the chip's: 12-bit, two samples in three bytes */
#define EG_TOP      (0x3ff << 16)
#define EG_TARGET   (0x817 << 15)               /* the attack's exponential target, above the top */
#define EG_START    ((0x3ff - 0x2a0) << 16)     /* where a note starts: about -63 dB */
#define SUB         32
#define CLAMP       60000
#define INC69       21426141u                   /* 440 Hz at 88.2 kHz, 2^32 per cycle */
#define GAIN_ONE    16000                       /* a voice's waves together, Q15 (peak ~0.5 FS) */
#define SEMI_CUT    301                         /* one semitone on the CUTOFF scale (9 octaves / 32767) */
#define KNEE        24576                       /* soft knee 3/4 up, as WHEEL: never a hard clip */

static const int8_t filt[8][2] = {              /* ADPCM predictors, /64 (as gen_pss.py) */
    { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 }, { 30, 0 }, { 90, -24 }, { 126, -63 },
};

/* key thresholds: INC69 * 2^((j - 0.5) / 12) */
static const uint32_t key_th[13] = {
    20816180, 22053975, 23365372, 24754749, 26226743, 27786267, 29438524, 31189030, 33043626,
    35008502, 37090216, 39295715, 41632360,
};

static int zone(int16_t knob, int n)
{
    int k = knob < 0 ? 0 : knob, z = k * n >> 15;

    return z >= n ? n - 1 : z;
}

int pss_nwaves(void) { return PSS_NMULTI; }
int pss_have_rom(void) { return PSS_HAVE_ROM; }
uint32_t pss_rom_bytes(void) { return PSS_ROM_BYTES; }
const uint8_t *pss_rom_data(void) { return pss_rom; }
#ifdef PSS_ROM_RUNTIME
const char *pss_rom_file(void) { return PSS_ROM_FILE; }
uint32_t pss_rom_file_bytes(void) { return PSS_ROM_FILE_BYTES; }
uint32_t pss_rom_file_sum(void) { return PSS_ROM_SUM; }
void pss_set_rom(const uint8_t *data) { pss_rom = data; }
int pss_rom_ready(void) { return pss_rom != 0; }
#else
const char *pss_rom_file(void) { return 0; }
uint32_t pss_rom_file_bytes(void) { return 0; }
uint32_t pss_rom_file_sum(void) { return 0; }
void pss_set_rom(const uint8_t *data) { (void)data; }
int pss_rom_ready(void) { return 1; }
#endif
int pss_wave_of(int16_t knob) { return zone(knob, PSS_NMULTI); }
int pss_waveb_of(int16_t knob) { return zone(knob, PSS_NMULTI + 1) - 1; }
int pss_semis_of(int16_t knob) { return zone(knob, 49) - 24; }

int pss_steps_of(int16_t knob)                   /* knob = steps x 520 (+ rounding) */
{
    int s = ((knob < 0 ? 0 : knob) * 2017 + 524288) >> 20;

    return s > 63 ? 63 : s;
}

const char *pss_wave_name(int w)
{
    return w >= 0 && w < PSS_NMULTI ? pss_multis[w].name : "OFF";
}

int pss_key_of_inc(uint32_t inc)
{
    int key = 69, j;

    if (!inc)
        return 0;
    while (inc < key_th[0]) {
        inc <<= 1;
        key -= 12;
    }
    while (inc >= key_th[12]) {
        inc >>= 1;
        key += 12;
    }
    for (j = 0; j < 11 && inc >= key_th[j + 1]; j++)
        ;
    key += j;
    return key < 0 ? 0 : key > 127 ? 127 : key;
}

uint32_t pss_inc_of_key(int key)
{
    int d = key - 69, o = d >= 0 ? d / 12 : -((11 - d) / 12), s = d - 12 * o;
    uint32_t v = (uint32_t)(((uint64_t)INC69 * pss_semi[s]) >> 16);

    return o >= 0 ? v << o : v >> -o;
}

const pss_sample *pss_pick(int w, int key)
{
    const pss_multi *m = &pss_multis[w < 0 ? 0 : w >= PSS_NMULTI ? PSS_NMULTI - 1 : w];
    const pss_zone *z = &pss_zones[m->zone];
    int i, best = 0, dist = 1000;

    for (i = 0; i < m->nzones; i++) {
        int d = key < z[i].lo ? z[i].lo - key : key > z[i].hi ? key - z[i].hi : 0;

        if (d < dist) {
            dist = d;
            best = i;
        }
    }
    return &pss_samples[z[best].sample];
}

static uint32_t raw_step(uint32_t inc, uint32_t ratio, const pss_sample *s)
{
    uint64_t st = ((((uint64_t)inc * ratio) >> 16) * s->k) >> 32;

    return st > 0xffffffffu ? 0xffffffffu : (uint32_t)st;
}

/* the copy for this pitch: PCPSS6 (single cycles, rich in harmonics) at a step <= 1, so nothing folds
 * above 44.1 kHz; ADPCM (sampled sounds, little up there) <= 2 */
const pss_sample *pss_level(const pss_sample *s, uint32_t inc, uint32_t ratio)
{
    while (s->mip != PSS_NOMIP && raw_step(inc, ratio, s) > (s->fmt != FMT_ADPCM ? 1u << 16 : 2u << 16))
        s = &pss_samples[s->mip];
    return s;
}

/* ------------------------------------------------------------------ decoder */

/* ADPCM sample i of a sample's data (base): 16 samples per 9 bytes (header: shift | predictor << 4) */
static inline __attribute__((always_inline)) int32_t dec1(const uint8_t *base, uint32_t i, int32_t *h1, int32_t *h2,
                                                          uint8_t *hdr)
{
    const uint8_t *b = base + (i >> 4) * 9;
    const int8_t *f;
    int sh, q;
    int32_t v;

    if ((i & 15) == 0)
        *hdr = b[0];
    sh = *hdr & 15;
    f = filt[*hdr >> 4];
    q = (((b[1 + ((i & 15) >> 1)] >> ((i & 1) << 2)) & 15) ^ 8) - 8;   /* signed nibble */
    v = ((q * (1 << sh)) >> 1) + ((f[0] * *h1 + f[1] * *h2) >> 6);
    v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    *h2 = *h1;
    *h1 = v;
    return v;
}

/* a whole ADPCM block (16 samples) at b into d: header once */
static void dec16(const uint8_t *b, int32_t *ph1, int32_t *ph2, int16_t *d)
{
    int32_t h1 = *ph1, h2 = *ph2, m = 1 << (b[0] & 15), a1 = filt[b[0] >> 4][0], a2 = filt[b[0] >> 4][1], v;
    int j;

    for (j = 1; j <= 8; j++) {
        int x = b[j];

        v = ((((x & 15) ^ 8) - 8) * m >> 1) + ((a1 * h1 + a2 * h2) >> 6);
        v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        h2 = h1;
        h1 = v;
        *d++ = (int16_t)v;
        v = ((((x >> 4) ^ 8) - 8) * m >> 1) + ((a1 * h1 + a2 * h2) >> 6);
        v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        h2 = h1;
        h1 = v;
        *d++ = (int16_t)v;
    }
    *ph1 = h1;
    *ph2 = h2;
}

/* GEW sample i (gew.cpp): 8-bit in the high byte; 12-bit: ab.c / ..C. AB, two samples in 3 bytes */
static inline __attribute__((always_inline)) int16_t gew1(const uint8_t *b, uint32_t i, int fmt)
{
    if (fmt == FMT_GEW8)
        return (int16_t)(b[i] << 8);
    b += (i >> 1) * 3;
    return (int16_t)(i & 1 ? b[2] << 8 | (b[1] & 0xf0) : b[0] << 8 | (b[1] & 15) << 4);
}

int16_t pss_osc_next(pss_osc *o, const uint8_t *rom)
{
    const pss_sample *s = o->s;

    if (!o->on)
        return 0;
    if (s->fmt >= FMT_GEW8) {                   /* the chip loops every sample (end -> loop) */
        if (o->i >= s->len) {
            o->i = s->loop;
            o->rev = 0;
        }
        o->i++;
        return gew1(rom + s->off, o->rev ? s->len - o->i : o->i - 1, s->fmt);
    }
    if (o->i >= s->len) {
        if (s->loop == PSS_NOLOOP) {
            o->on = 0;
            return 0;
        }
        if (s->fmt == FMT_PCPSS6) {
            o->i = s->loop;
        } else {                                /* back to the loop's block, replay up to the start */
            o->i = s->loop & ~15u;
            o->h1 = s->h1;
            o->h2 = s->h2;
            for (; o->i < s->loop; o->i++)
                dec1(rom + s->off, o->i, &o->h1, &o->h2, &o->hdr);
        }
    }
    if (s->fmt == FMT_PCPSS6)
        return ((const int16_t *)(const void *)(rom + s->off))[o->i++];
    return (int16_t)dec1(rom + s->off, o->i++, &o->h1, &o->h2, &o->hdr);
}

void pss_osc_start(pss_osc *o, const pss_sample *s)
{
    o->s = s;
    o->i = 0;
    o->frac = 0;
    o->h1 = o->h2 = 0;
    o->hdr = 0;
    o->rev = s && s->fmt >= FMT_GEW8 && (s->lfo[0] >> 7);
    o->on = s != 0 && pss_rom_ready();
    o->direct = 0;
    if (o->on) {
        o->x0 = pss_osc_next(o, pss_rom);
        o->x1 = pss_osc_next(o, pss_rom);
    } else {
        o->x0 = o->x1 = 0;
    }
}

/* output stage: linear up to 3/4 of full scale, then a smooth knee approaching +-32767
 * (y = T + R d / (d + R)) instead of a hard clip; the divide only runs above the knee */
static inline int16_t soft(int32_t x)
{
    int32_t ax = x < 0 ? -x : x, d, y;

    if (ax <= KNEE)
        return (int16_t)x;
    d = ax - KNEE;
    y = KNEE + (int32_t)(((uint32_t)(32767 - KNEE) * (uint32_t)d) / (uint32_t)(d + 32767 - KNEE));
    return (int16_t)(x < 0 ? -y : y);
}

/* ------------------------------------------------------------------ GEW7 envelope + LFO */

/* get_rate: register 0 = no movement, 15 = instant, else 4 x reg + key scaling */
static uint8_t rate_index(int reg, int rate)
{
    int r = 4 * reg + rate;

    return (uint8_t)(reg == 0 ? 0 : reg == 15 ? 63 : r < 0 ? 0 : r > 63 ? 63 : r);
}

void pss_eg_start(pss_eg *e, const uint8_t *eg, const uint8_t *lfo, int on, uint32_t st)
{
    uint32_t y;
    int ex, oct, bit9, rate, krs = eg[2] >> 4;

    e->vib = e->trem = 0;
    if (!on) {
        e->st = 3;
        e->v = EG_TOP;
        e->g = 65536;
        return;
    }
    /* the chip's octave and pitch bit 9 for this step: y = 2 x (source samples per chip sample),
     * 16.16 = st x 2 x 88200 / 111111.1 (the chip halves its step for 2x oversampling) */
    y = (uint32_t)(((uint64_t)st * 104046) >> 16);
    y = y ? y : 1;
    for (ex = 0; (y >> ex) > 1; ex++)
        ;
    bit9 = ex ? (y >> (ex - 1)) & 1 : 0;
    ex -= 16;                                   /* step = 2^ex x (1 + F / 1024), octave reg = ex + 1 */
    ex = ex < -8 ? -8 : ex > 7 ? 7 : ex;
    oct = ex + 1 > 7 ? ex + 1 - 16 : ex + 1;    /* the register is 4 bits, read signed */
    rate = krs == 15 ? 0 : (oct + krs) * 2 + bit9;
    e->ar = rate_index(eg[0] >> 4, rate);
    e->d1 = rate_index(eg[0] & 15, rate);
    e->d2 = rate_index(eg[1] >> 4, rate);
    e->th = (16 - (eg[2] & 15)) << 22;          /* decay level 15 - DL: v >> 22 <= it */
    e->v = EG_START;
    e->st = 0;
    e->g = pss_eg_exp[EG_START >> 16];
    e->lf = lfo[1] & 7;
    e->vib = lfo[0] & 7;
    e->trem = (lfo[0] >> 3) & 7;
}

/* 32 output samples: the level index 0..1023 (tremolo included) */
int pss_eg_next(pss_eg *e)
{
    int32_t v = e->v, d;
    int idx, i, p;

    switch (e->st) {
    case 0:                                     /* attack: exponential towards EG_TARGET */
        v = EG_TARGET - (int32_t)(((int64_t)(EG_TARGET - v) * pss_eg_keep[e->ar]) >> 30);
        if (v >= EG_TOP) {
            v = EG_TOP;
            e->st = 1;
        }
        break;
    case 1:                                     /* decay 1 (linear in dB) to the decay level */
        d = (int32_t)pss_eg_dec[e->d1];
        if (v - d >= e->th) {
            v -= d;
        } else {                                /* crossed in these 32: chip samples of decay 1 until below
                                                 * the level (it overshoots by up to a step), decay 2 for the rest */
            int32_t d1 = (int32_t)pss_eg_dec1[e->d1], n = 0, rest;

            if (v >= e->th)
                n = (v - e->th) / d1 + 1;
            v -= n * d1;
            rest = (int32_t)pss_eg_dec[e->d2] - n * (int32_t)pss_eg_dec1[e->d2];
            v -= rest > 0 ? rest : 0;
            e->st = 2;
        }
        v = v < 0 ? 0 : v;
        break;
    case 2:                                     /* decay 2 */
        v -= (int32_t)pss_eg_dec[e->d2];
        v = v < 0 ? 0 : v;
        break;
    default:
        return 1023;
    }
    e->v = v;
    idx = v >> 16;
    if (e->trem) {                              /* tremolo: 0 .. -limit dB, triangle */
        i = (e->ph >> 8) & 0xff;
        p = i < 128 ? 255 - 2 * i : 2 * i - 256;
        idx -= (int)((pss_trem_q4[e->trem] * (uint32_t)p) >> 12);
        idx = idx < 0 ? 0 : idx;
    }
    e->ph = (uint16_t)(e->ph + pss_lfo_inc[e->lf]);
    return idx;
}

/* vibrato: 2^(cents / 1200) at the LFO's phase, Q16 (65536 = none) */
static uint32_t vibrato(const pss_eg *e)
{
    int i = (e->ph >> 8) & 0xff, p;
    int32_t y;

    if (!e->vib)
        return 65536;
    p = i < 64 ? 2 * i + 128 : i < 128 ? 383 - 2 * i : i < 192 ? 384 - 2 * i : 2 * i - 383;
    /* y = cents x ln2 / 1200, Q16 (|y| <= 3002); e^y to the square term: 0.03 cents off at most */
    y = (int32_t)(((int64_t)pss_vib_mc[e->vib] * (p - 128) * 2481) >> 23);
    return (uint32_t)(65536 + y + ((y * y) >> 17));
}

/* ------------------------------------------------------------------ voice */

static uint32_t semi_ratio(int s);

static int eg_on(const pss_sample *s)
{
    return s && (s->fmt >= FMT_GEW8 || s->eg[0] || s->eg[1] || s->eg[2]);
}

void pss_note_on(pss_voice *v, uint32_t inc, const int16_t *p)
{
    int key = pss_key_of_inc(inc), wa = pss_wave_of(p[PSS_WAVE_A]), wb = pss_waveb_of(p[PSS_WAVE_B]);
    int fa = pss_multis[wa].fixed, kb, sb = pss_semis_of(p[PSS_BPITCH]);

    v->key = key;
    v->a.fixed = fa != 0xff;
    v->fixa = v->a.fixed ? pss_inc_of_key(fa) : 0;
    pss_osc_start(&v->a, pss_level(pss_pick(wa, v->a.fixed ? fa : key), v->a.fixed ? v->fixa : inc, 65536));
    if (wb >= 0) {
        int fb = pss_multis[wb].fixed;

        kb = key + sb;
        kb = kb < 0 ? 0 : kb > 127 ? 127 : kb;
        v->b.fixed = fb != 0xff;
        v->fixb = v->b.fixed ? pss_inc_of_key(fb) : 0;
        pss_osc_start(&v->b, pss_level(pss_pick(wb, v->b.fixed ? fb : kb), v->b.fixed ? v->fixb : inc,
                                     semi_ratio(sb)));
    } else {
        pss_osc_start(&v->b, 0);
    }
    pss_eg_start(&v->a.eg, v->a.s ? v->a.s->eg : (const uint8_t *)"\0\0\0", v->a.s ? v->a.s->lfo : (const uint8_t *)"\0\0",
                 eg_on(v->a.s), v->a.s ? raw_step(v->a.fixed ? v->fixa : inc, 65536, v->a.s) : 0);
    pss_eg_start(&v->b.eg, v->b.s ? v->b.s->eg : (const uint8_t *)"\0\0\0", v->b.s ? v->b.s->lfo : (const uint8_t *)"\0\0",
                 eg_on(v->b.s), v->b.s ? raw_step(v->b.fixed ? v->fixb : inc, semi_ratio(sb), v->b.s) : 0);
    v->lp = v->bp = 0;
    v->env = 32767;
    v->la = 32767;
    v->lb = pss_steps_of(p[PSS_BFADE]) ? 0 : 32767;
}

/* 16.16 source samples per output sample, bounded so a block's work is too: ADPCM (decoded) at
 * most 4, PCPSS6 (copied) 16. With the octave-down copies (up to C8) only bends / LFO far above
 * that reach the cap; the pitch then stays there. */
#define STEP_ADPCM  (4u << 16)
#define STEP_GEW_FILL (3u << 16)                /* GEW above: osc_gew reads only the samples it needs */
#define STEP_PCM    (16u << 16)

static uint32_t step_of(uint32_t inc, uint32_t ratio, const pss_sample *s)
{
    uint32_t st = raw_step(inc, ratio, s), max = s->fmt >= FMT_GEW8 ? 0xffffffu : s->fmt != FMT_ADPCM ? STEP_PCM
                                                                                                    : STEP_ADPCM;

    return st > max ? max : st;
}

/* the source samples one block of one oscillator needs (step <= 16), and the mix; the audio thread
 * renders one voice at a time */
static int16_t src[16 * 128 + 4];
static int32_t acc[128];

/* decode the next m samples of o into d (loops wrap; after a one-shot's end: zeros) */
static void fill(pss_osc *o, int16_t *d, uint32_t m)
{
    const pss_sample *s = o->s;
    const uint8_t *base = pss_rom + s->off;
    uint32_t i = o->i, len = s->len, loop = s->loop, q = 0, c, e;
    int32_t h1 = o->h1, h2 = o->h2;
    uint8_t hdr = o->hdr;

    if (o->on && s->fmt >= FMT_GEW8) {          /* GEW: decoded as read, looping end -> loop */
        int fmt = s->fmt;

        while (q < m) {
            if (i >= len) {
                i = loop;
                o->rev = 0;
            }
            c = len - i < m - q ? len - i : m - q;
            e = i + c;
            if (o->rev) {
                for (; i < e; i++)
                    d[q++] = gew1(base, len - 1 - i, fmt);
            } else if (fmt == FMT_GEW8) {
                const int8_t *b8 = (const int8_t *)base;

                for (; i < e; i++)
                    d[q++] = (int16_t)(b8[i] * 256);
            } else {                            /* 12-bit: whole triplets, two samples each */
                const uint8_t *t;

                if (i & 1)
                    d[q++] = gew1(base, i++, FMT_GEW12);
                for (t = base + (i >> 1) * 3; i + 1 < e; i += 2, t += 3) {
                    d[q++] = (int16_t)(t[0] << 8 | (t[1] & 15) << 4);
                    d[q++] = (int16_t)(t[2] << 8 | (t[1] & 0xf0));
                }
                if (i < e)
                    d[q++] = gew1(base, i++, FMT_GEW12);
            }
        }
        o->i = i;
        return;
    }
    while (q < m) {
        if (!o->on || i >= len) {
            if (!o->on || loop == PSS_NOLOOP) {
                o->on = 0;
                for (; q < m; q++)
                    d[q] = 0;
                break;
            }
            if (s->fmt == FMT_PCPSS6) {
                i = loop;
            } else {
                i = loop & ~15u;                /* the loop's block, replayed up to the start */
                h1 = s->h1;
                h2 = s->h2;
                for (; i < loop; i++)
                    dec1(base, i, &h1, &h2, &hdr);
            }
        }
        c = len - i < m - q ? len - i : m - q;
        e = i + c;
        if (s->fmt == FMT_PCPSS6) {
            const int16_t *pcm = (const int16_t *)(const void *)base;

            for (; i < e; i++)
                d[q++] = pcm[i];
        } else {
            while (i < e) {
                if ((i & 15) == 0 && e - i >= 16) {     /* whole blocks: header once */
                    dec16(base + (i >> 4) * 9, &h1, &h2, d + q);
                    q += 16;
                    i += 16;
                } else {
                    d[q++] = (int16_t)dec1(base, i++, &h1, &h2, &hdr);
                }
            }
        }
    }
    o->i = i;
    o->h1 = h1;
    o->h2 = h2;
    o->hdr = hdr;
}

/* a GEW oscillator: the chip's samples are read where they are needed (no decoder state), so a block
 * costs the same at any pitch: x(i) .. x(i + 1) interpolated, end -> loop, the reversed first pass.
 * One loop per format, everything in locals (x may alias the oscillator). */
static void osc_gew(pss_osc *o, int16_t *x, int n, uint32_t st)
{
    const pss_sample *s = o->s;
    const uint8_t *b = pss_rom + s->off;
    const int8_t *b8 = (const int8_t *)b;
    uint32_t i = o->i, f = o->frac, len = s->len, lp = s->loop, L = len - lp, last = len - 1, j, q;
    int k = 0;
    int32_t a, c;

    for (; k < n && o->rev; k++) {              /* the reversed first pass (rare): sample by sample */
        a = gew1(b, last - i, s->fmt);
        c = i < last ? gew1(b, last - 1 - i, s->fmt) : gew1(b, lp, s->fmt);
        x[k] = (int16_t)(a + (((c - a) * (int32_t)(f >> 2)) >> 14));
        f += st;
        i += f >> 16;
        f &= 0xffff;
        if (i >= len) {
            do
                i -= L;
            while (i >= len);
            o->rev = 0;
        }
    }
    if (s->fmt == FMT_GEW8) {
        for (; k < n; k++) {
            j = i < last ? i + 1 : lp;
            a = b8[i];
            c = b8[j];
            x[k] = (int16_t)((a << 8) + (((c - a) * (int32_t)f) >> 8));
            f += st;
            i += f >> 16;
            f &= 0xffff;
            while (i >= len)
                i -= L;
        }
    } else {
        for (; k < n; k++) {
            const uint8_t *t = b + (i >> 1) * 3;

            j = i < last ? i + 1 : lp;
            if (i & 1) {                        /* ..C. AB: this one, then the next triplet's first */
                a = (int16_t)(t[2] << 8 | (t[1] & 0xf0));
                q = (j >> 1) * 3;
                c = j & 1 ? (int16_t)(b[q + 2] << 8 | (b[q + 1] & 0xf0)) : (int16_t)(b[q] << 8 | (b[q + 1] & 15) << 4);
            } else {                            /* ab.c: both in this triplet unless at the end */
                a = (int16_t)(t[0] << 8 | (t[1] & 15) << 4);
                c = j == i + 1 ? (int16_t)(t[2] << 8 | (t[1] & 0xf0)) : gew1(b, j, FMT_GEW12);
            }
            x[k] = (int16_t)(a + (((c - a) * (int32_t)(f >> 2)) >> 14));
            f += st;
            i += f >> 16;
            f &= 0xffff;
            while (i >= len)
                i -= L;
        }
    }
    o->i = i;
    o->frac = f;
}

/* one oscillator, interpolated, into x[0..n): st = 16.16 source samples per output sample (constant
 * for the block, <= STEP_PCM) */
static void osc_run(pss_osc *o, int16_t *x, int n, uint32_t st)
{
    uint32_t pos = o->frac, m = (pos + st * (uint32_t)n) >> 16;
    int k;

    if (!o->on && !o->x0 && !o->x1) {
        for (k = 0; k < n; k++)
            x[k] = 0;
        return;
    }
    if (o->s->fmt >= FMT_GEW8 && st > STEP_GEW_FILL) {  /* high up: read only what is needed */
        if (!o->direct) {                       /* from the decoded run: x0 is source sample i - 2 (just
                                                 * after a loop wrap: a sample off, once) */
            o->i = o->i >= 2 ? o->i - 2 : 0;
            o->direct = 1;
        }
        osc_gew(o, x, n, st);
        return;
    }
    if (o->direct) {                            /* back to decoding runs: x0, x1 at i, i + 1 */
        o->direct = 0;
        o->x0 = pss_osc_next(o, pss_rom);
        o->x1 = pss_osc_next(o, pss_rom);
    }
    src[0] = o->x0;
    src[1] = o->x1;
    fill(o, src + 2, m);
    for (k = 0; k < n; k++, pos += st) {
        const int16_t *q = src + (pos >> 16);

        x[k] = (int16_t)(q[0] + (((q[1] - q[0]) * (int32_t)((pos & 0xffff) >> 2)) >> 14));
    }
    o->frac = pos & 0xffff;
    o->x0 = src[m];
    o->x1 = src[m + 1];
}

static uint32_t semi_ratio(int s)                /* 2^(s / 12), Q16, s = -24 .. +24 */
{
    int o = 0;

    for (; s < 0; s += 12)
        o--;
    for (; s >= 12; s -= 12)
        o++;
    return o >= 0 ? pss_semi[s] << o : pss_semi[s] >> -o;
}

void pss_render(pss_voice *v, int16_t *out, int n, const uint32_t *inc, const int16_t *p)
{
    int16_t xa[128], xb[128];
    int i, s, mix = p[PSS_MIX] < 0 ? 0 : p[PSS_MIX], on_b = v->b.s != 0;
    int det = (pss_steps_of(p[PSS_DETUNE]) * 969) >> 6;          /* 63 steps: 25 cents = 954 / 65536 */
    uint32_t ra = (uint32_t)(65536 - det), rb = (uint32_t)(((uint64_t)semi_ratio(pss_semis_of(p[PSS_BPITCH]))
                                                            * (uint32_t)(65536 + det)) >> 16);
    int32_t la0 = v->la, lb0 = v->lb, la1, lb1, ga0, ga1, gb0, gb1;
    int ad = pss_steps_of(p[PSS_ADECAY]), bf = pss_steps_of(p[PSS_BFADE]);
    int cut = p[PSS_CUTOFF] < 0 ? 0 : p[PSS_CUTOFF], fenv = p[PSS_FENV] < 0 ? 0 : p[PSS_FENV];
    int res = p[PSS_RES] < 0 ? 0 : p[PSS_RES], d = p[PSS_FDECAY] < 0 ? 0 : p[PSS_FDECAY];
    int di = d >> 10, df = d & 1023, env0 = v->env, env1;
    int32_t ic1 = v->bp, ic2 = v->lp;
    int k15 = 65536 - ((res * 55706) >> 15);                    /* damping 2 .. 0.3, Q15 */

    if (n > 128)
        n = 128;
    la1 = (la0 * pss_adecay[ad]) >> 15;
    lb1 = bf ? lb0 + (((32767 - lb0) * pss_bfade[bf]) >> 15) : 32767;
    v->la = la1;
    v->lb = lb1;
    if (on_b) {                                 /* a true A/B crossfade: they add to one wave's level */
        int wa = 32767 - mix, wb = mix;

        ga0 = (((GAIN_ONE * wa) >> 15) * la0) >> 15;
        ga1 = (((GAIN_ONE * wa) >> 15) * la1) >> 15;
        gb0 = (((GAIN_ONE * wb) >> 15) * lb0) >> 15;
        gb1 = (((GAIN_ONE * wb) >> 15) * lb1) >> 15;
    } else {
        ga0 = (GAIN_ONE * la0) >> 15;
        ga1 = (GAIN_ONE * la1) >> 15;
        gb0 = gb1 = 0;
    }
    {                                           /* pitch: the block's mean increment */
        uint32_t ia = v->a.fixed ? v->fixa : (inc[0] >> 1) + (inc[n - 1] >> 1);

        if (v->a.s)
            osc_run(&v->a, xa, n, step_of(ia, (uint32_t)(((uint64_t)ra * vibrato(&v->a.eg)) >> 16), v->a.s));
        else
            for (i = 0; i < n; i++)
                xa[i] = 0;
    }
    if (on_b) {
        uint32_t ib = v->b.fixed ? v->fixb : (inc[0] >> 1) + (inc[n - 1] >> 1);

        osc_run(&v->b, xb, n, step_of(ib, (uint32_t)(((uint64_t)rb * vibrato(&v->b.eg)) >> 16), v->b.s));
    }

    v->env = (env0 * (pss_fdecay[di] + (((pss_fdecay[di + 1] - pss_fdecay[di]) * df) >> 10))) >> 15;
    env1 = v->env;
    for (s = 0; s < n; s += SUB) {              /* levels and filter coefficients per 32 samples */
        int e = s + SUB < n ? s + SUB : n, mid = s + SUB / 2;
        int32_t ga = ga0 + (((ga1 - ga0) * mid) >> 7), gb = gb0 + (((gb1 - gb0) * mid) >> 7);
        int env = env0 + (((env1 - env0) * mid) >> 7);
        int32_t ea0 = v->a.eg.g, eb0 = v->b.eg.g, ea1, eb1, dea, deb, ea, eb;
        int c = cut + ((((fenv * env) >> 15) * 21700) >> 15)          /* up to 6 octaves */
                + (((v->key - 60) * SEMI_CUT * (p[PSS_KEYTRK] < 0 ? 0 : p[PSS_KEYTRK])) >> 15);
        int ci, gg, a1, a2, a3;

        /* the AWM envelopes: from the last gain to this one over the 32 samples */
        ea1 = v->a.eg.st == 3 ? 65536 : pss_eg_exp[pss_eg_next(&v->a.eg)];
        v->a.eg.g = ea1;
        dea = (((ea1 - ea0) * ga) >> 15) / SUB;
        ea = (ea0 * ga) >> 15;
        if (on_b) {
            eb1 = v->b.eg.st == 3 ? 65536 : pss_eg_exp[pss_eg_next(&v->b.eg)];
            v->b.eg.g = eb1;
            deb = (((eb1 - eb0) * gb) >> 15) / SUB;
            eb = (eb0 * gb) >> 15;
            for (i = s; i < e; i++, ea += dea, eb += deb)
                acc[i] = (xa[i] * ea + xb[i] * eb) >> 16;     /* ea + eb <= 2 x GAIN_ONE: fits */
        } else {
            for (i = s; i < e; i++, ea += dea)
                acc[i] = (xa[i] * ea) >> 16;
        }
        if (cut >= 32700 && !fenv && !res) {    /* filter fully open: the samples as they are */
            for (i = s; i < e; i++)
                out[i] = soft(acc[i]);
            ic1 = ic2 = 0;
            continue;
        }
        c = c > 32767 ? 32767 : c < 0 ? 0 : c;
        ci = c >> 9;
        gg = pss_cut[ci] + (((pss_cut[ci + 1] - pss_cut[ci]) * (c & 511)) >> 9);
        a1 = (int)((32768u << 15) / (uint32_t)(32768 + ((gg * (gg + k15)) >> 15)));   /* 1/(1+g(g+k)) */
        a2 = (gg * a1) >> 15;
        a3 = (gg * a2) >> 15;
        for (i = s; i < e; i++) {               /* trapezoidal SVF (Simper), low-pass */
            int32_t v3 = acc[i] - ic2, v1 = ((a1 * ic1) >> 15) + ((a2 * v3) >> 15);
            int32_t v2 = ic2 + ((a2 * ic1) >> 15) + ((a3 * v3) >> 15);

            ic1 = 2 * v1 - ic1;
            ic2 = 2 * v2 - ic2;
            ic1 = ic1 > CLAMP ? CLAMP : ic1 < -CLAMP ? -CLAMP : ic1;
            ic2 = ic2 > CLAMP ? CLAMP : ic2 < -CLAMP ? -CLAMP : ic2;
            out[i] = soft(v2);
        }
    }
    v->lp = ic2;
    v->bp = ic1;
}
