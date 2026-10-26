/* FXPACK effect core, see fxpack_core.h and README.md. Tables: fp_tables.h (gen_tables.py). */
#include "fxpack_core.h"
#include "fp_tables.h"

#define DMASK       (FP_DL_LEN - 1)
#define SVF_CLAMP   60000                       /* SVF states: two Q14 products always fit */

static const char *const names[FP_TYPES] = {
    "PHASER", "FLANGER", "CHORUS", "COMB", "RING MOD", "TREMOLO", "FOLD", "FUZZ", "BITCRUSH", "DJ FILTER",
    "LADDER", "AUTO-WAH", "VOWEL", "TRANSIENT", "TEXTURE",
};
static const char *const pnames[FP_TYPES][8] = {
    { "RATE", "DEPTH", "FEEDBACK", "MIX", "TYPE", "CENTER", "STAGES", "SPREAD" },
    { "RATE", "DEPTH", "FEEDBACK", "MIX", "TYPE", "DELAY", "DAMP", "SPREAD" },
    { "RATE", "DEPTH", "VOICES", "MIX", "TYPE", "DELAY", "TONE", "SPREAD" },
    { "FREQ", "FEEDBACK", "DAMP", "MIX", "TYPE", "DEPTH", "RATE", "SPREAD" },
    { "FREQ", "DEPTH", "RATE", "MIX", "TYPE", "SHAPE", "TONE", "SPREAD" },
    { "RATE", "DEPTH", "SHAPE", "MIX", "TYPE", "SPREAD", "-", "-" },
    { "DRIVE", "BIAS", "TONE", "MIX", "TYPE", "SHAPE", "LEVEL", "-" },
    { "DRIVE", "TONE", "CHAR", "MIX", "TYPE", "LEVEL", "BIAS", "GATE" },
    { "BITS", "RATE", "JITTER", "MIX", "TYPE", "TONE", "DRIVE", "-" },
    { "FILTER", "RESO", "DRIVE", "MIX", "TYPE", "SLOPE", "LFO", "WOBBLE" },
    { "CUTOFF", "RESO", "DRIVE", "MIX", "TYPE", "ENV", "LFO", "WOBBLE" },
    { "SENS", "RESO", "FREQ", "MIX", "TYPE", "SPEED", "MODE", "DIR" },
    { "VOWEL", "RESO", "SHIFT", "MIX", "TYPE", "LFO", "DEPTH", "SPREAD" },
    { "ATTACK", "SUSTAIN", "OUTPUT", "MIX", "TYPE", "SPEED", "CLIP", "-" },
    { "KIND", "LEVEL", "TONE", "BREATH", "TYPE", "RATE", "DENSITY", "DUCK" },
};
static const char *const curves[FP_CURVES] = { "SOFT", "HARD", "DIODE", "FUZZ", "CHEBY" };
static const char *const kinds[FP_KINDS] = { "VINYL", "TAPE", "RAIN", "WIND", "RADIO", "ROOM" };
static const char *const stages[3] = { "4", "6", "8" };
static const char *const voices[3] = { "1", "2", "3" };
static const char *const modes[3] = { "LP", "BP", "HP" };
static const char *const dirs[2] = { "UP", "DOWN" };
static const char *const slopes[2] = { "12 DB", "24 DB" };

static int32_t knob(const int16_t *p, int i)
{
    return p[i] < 0 ? 0 : p[i];
}

static int32_t cen(const int16_t *p, int i)
{
    return knob(p, i) - 16384;                  /* -16384 .. 16383 */
}

static int16_t sat16(int32_t x)
{
    return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

static int32_t clampi(int32_t x, int32_t lo, int32_t hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

static int step_of(int32_t v, int n)
{
    int32_t i = (v * n) >> 15;

    return i >= n ? n - 1 : i;
}

/* TYPE slot (v >> 10) -> effect. Stored values (type_v) sit in: v0.4's PHASER 2 FLANGER 6 FOLD 11
 * FUZZ 16 DJ 20 TRANSIENT 25 TEXTURE 29, v0.5's CHORUS 9 BITCRUSH 19; new effects in slot centres
 * (COMB 4 RING 13 TREMOLO 14 LADDER 22 AUTO-WAH 23 VOWEL 27); the rest fall to a neighbour */
static const uint8_t slot_type[32] = {
    FP_PHASER, FP_PHASER, FP_PHASER, FP_PHASER, FP_COMB, FP_FLANGER, FP_FLANGER, FP_FLANGER,
    FP_FLANGER, FP_CHORUS, FP_CHORUS, FP_FOLD, FP_FOLD, FP_RING, FP_TREMOLO, FP_FUZZ,
    FP_FUZZ, FP_FUZZ, FP_FUZZ, FP_BITCRUSH, FP_DJ, FP_DJ, FP_LADDER, FP_WAH,
    FP_TRANSIENT, FP_TRANSIENT, FP_TRANSIENT, FP_VOWEL, FP_TEXTURE, FP_TEXTURE, FP_TEXTURE, FP_TEXTURE,
};
static const int16_t type_v[FP_TYPES] = {
    2340, 7020, 9216, 4608, 13824, 14848, 11704, 16384, 19456, 21068, 23040, 24064, 28160, 25748, 30432,
};

int fp_type_of(int16_t v)
{
    return slot_type[(v < 0 ? 0 : v) >> 10];
}

int16_t fp_type_value(int type)
{
    return type_v[type < 0 || type >= FP_TYPES ? 0 : type];
}

const char *fp_name(int type)
{
    return names[type < 0 || type >= FP_TYPES ? 0 : type];
}

const char *fp_param_name(int type, int i)
{
    return pnames[type < 0 || type >= FP_TYPES ? 0 : type][i & 7];
}

int fp_steps(int type, int i)
{
    if (i == 4)
        return FP_TYPES;
    if ((type == FP_PHASER && i == 6) || (type == FP_CHORUS && i == 2))
        return 3;
    if (type == FP_FUZZ && i == 2)
        return FP_CURVES;
    if ((type == FP_DJ && i == 5) || (type == FP_WAH && i == 7))
        return 2;
    if (type == FP_WAH && i == 6)
        return 3;
    if (type == FP_TEXTURE && i == 0)
        return FP_KINDS;
    return 0;
}

int fp_step(int type, int i, int16_t v)
{
    int n = fp_steps(type, i);

    if (i == 4)
        return fp_type_of(v);
    return n ? step_of(v < 0 ? 0 : v, n) : 0;
}

int16_t fp_step_value(int type, int i, int step)
{
    int n = fp_steps(type, i);

    if (!n)
        return 0;
    step = step < 0 ? 0 : step >= n ? n - 1 : step;
    if (i == 4)
        return fp_type_value(step);
    return (int16_t)(((2 * step + 1) * 16384) / n);
}

const char *fp_choice(int type, int i, int16_t v)
{
    const char *const *t;

    if (!fp_steps(type, i))
        return 0;
    if (i == 4)
        t = names;
    else if (type == FP_PHASER)
        t = stages;
    else if (type == FP_CHORUS)
        t = voices;
    else if (type == FP_FUZZ)
        t = curves;
    else if (type == FP_DJ)
        t = slopes;
    else if (type == FP_WAH)
        t = i == 6 ? modes : dirs;
    else
        t = kinds;
    return t[fp_step(type, i, v)];
}


/* sine of a 32-bit phase, Q15 */
static int32_t sin32(uint32_t ph)
{
    int32_t i = (int32_t)(ph >> 24), f = (int32_t)((ph >> 9) & 0x7fff), a = fp_sin[i], b = fp_sin[i + 1];

    return a + (((b - a) * f) >> 15);
}

/* 2^x, x in 16.16 (-15 .. +15), result 16.16 */
static uint32_t exp2q(int32_t x)
{
    int32_t n = x >> 16, f = (x & 0xffff) >> 12, r = x & 0xfff;
    uint32_t a = fp_exp2[f], b = f < 15 ? fp_exp2[f + 1] : 65536, m = a + (((b - a) * (uint32_t)r) >> 12);

    m <<= 1;                                    /* Q15 -> Q16 mantissa (1 .. 2) */
    if (n >= 0)
        return n > 14 ? 0xffffffffu : m << n;
    return n < -31 ? 0 : m >> -n;
}

/* LFO increment per sample for u = 0..32767: 0.05 Hz * 2^(7.3 u) (0.05 .. ~8 Hz) */
static uint32_t lfo_inc(int32_t u)
{
    return (uint32_t)(((uint64_t)4870u * exp2q((u * 14950) >> 10)) >> 16);
}

/* one-pole lowpass coefficient for TONE-style knobs: 0 -> dark (~250 Hz), full -> open */
static int32_t tone_coef(int32_t u)
{
    return 1200 + ((u * 31567) >> 15);
}

static int32_t curve(int c, int32_t v)
{
    const int16_t *t = fp_curve + c * 257;
    int32_t x = clampi(v, -32768, 32767) + 32768, i = x >> 8, f = x & 255;

    return t[i] + (((t[i + 1] - t[i]) * f) >> 8);
}

/* interpolated read d (16.16) samples behind w */
static int32_t tap(const int16_t *ln, uint32_t w, uint32_t d)
{
    uint32_t di = d >> 16;
    int32_t fr = (int32_t)((d & 0xffff) >> 1), a = ln[(w - di) & DMASK], b = ln[(w - di - 1) & DMASK];

    return a + (((b - a) * fr) >> 15);
}

static uint32_t rnd(fp_state *s)
{
    s->rnd = s->rnd * 1664525u + 1013904223u;
    return s->rnd;
}

void fp_reset(fp_state *s, int type, int ch)
{
    int i;

    for (i = 0; i < 8; i++)
        s->apx[i] = s->apy[i] = s->tx[i] = 0;
    for (i = 0; i < FP_DL_LEN; i++)
        s->dl[i] = 0;
    for (i = 0; i < 4; i++)
        s->ic[i] = 0;
    for (i = 0; i < 64; i++)
        s->scope[i] = 0;
    s->type = type < 0 || type >= FP_TYPES ? 0 : type;
    s->ch = ch;
    s->lfo = 0;
    s->rnd = 0x1234567u + (uint32_t)ch * 0x9e3779b9u;
    s->w = 0;
    s->fb = s->pa = 0;
    s->dcur = 0;
    s->lp = s->lp2 = s->hpx = s->hpy = 0;
    s->envf = s->envs = 0;
    s->gain = 4096;
    s->gate = 32767;
    s->level = 0;
}

/* ------------------------------------------------------------------ effects (one chunk, m <= 64) */

static void phaser(fp_state *s, int16_t *b, int m, const int16_t *p)
{
    int32_t depth = (knob(p, 1) * 128) >> 15, fbg = (knob(p, 2) * 29491) >> 15, mix = knob(p, 3) >> 1;
    int32_t ci = (knob(p, 5) * 255) >> 15, ns = 4 + 2 * step_of(knob(p, 6), 3), a, da, i, k;
    uint32_t inc = lfo_inc(knob(p, 0)), ph = s->lfo + (s->ch ? (uint32_t)knob(p, 7) << 15 : 0);

    a = fp_ap[clampi(ci + ((sin32(ph + inc * (uint32_t)m) * depth) >> 15), 0, 255)];
    da = (a - s->pa) / m;
    a = s->pa;
    for (i = 0; i < m; i++) {
        int32_t x = b[i], v = sat16(x + ((s->fb * fbg) >> 15));

        a += da;
        for (k = 0; k < ns; k++) {
            int32_t y = ((a * (v - s->apy[k])) >> 15) + s->apx[k];

            s->apx[k] = v;
            s->apy[k] = y = sat16(y);
            v = y;
        }
        s->fb = v;
        b[i] = sat16((x * (32768 - mix) + v * mix) >> 15);
    }
    s->pa = a;
    s->lfo += inc * (uint32_t)m;
}

static void flanger(fp_state *s, int16_t *b, int m, const int16_t *p)
{
    int32_t depth = (knob(p, 1) * 220) >> 15, fbg = (cen(p, 2) * 58982) >> 15, mix = knob(p, 3) >> 1;
    int32_t damp = tone_coef(32767 - knob(p, 6)), i, dd;
    uint32_t inc = lfo_inc(knob(p, 0)), ph = s->lfo + (s->ch ? (uint32_t)knob(p, 7) << 16 : 0), end, d, w = s->w;

    s->lfo += inc * (uint32_t)m;
    end = (uint32_t)(4 + ((knob(p, 5) * 437) >> 15)) << 16;          /* 0.1 .. 10 ms */
    end += (uint32_t)((sin32(ph + inc * (uint32_t)m) + 32768) * depth);          /* + 0 .. 5 ms */
    d = s->dcur ? s->dcur : end;
    dd = (int32_t)(end - d) / m;
    for (i = 0; i < m; i++, w++) {
        int32_t x = b[i], y;

        d += (uint32_t)dd;
        y = tap(s->dl, w, d);
        s->lp += ((y - s->lp) * damp) >> 15;
        s->dl[w & DMASK] = sat16(x + ((s->lp * fbg) >> 15));
        b[i] = sat16((x * (32768 - mix) + y * mix) >> 15);
    }
    s->dcur = d;
    s->w = w;
}

/* triangle of a 32-bit phase, -32768 .. 32768 */
static int32_t tri32(uint32_t ph)
{
    int32_t u = (int32_t)(ph >> 15);

    return u < 65536 ? u - 32768 : 98304 - u;
}

/* 1-3 voices on the flanger's line, triangle LFOs 120 degrees apart, no feedback, darkened.
 * MIX: dry 1 - m^2, wet 2m - m^2 (the voices drift apart from the dry: a plain crossfade would
 * lose ~4 dB at half mix) */
static void chorus(fp_state *s, int16_t *b, int m, const int16_t *p)
{
    int32_t depth = (knob(p, 1) * 264) >> 15, nv = 1 + step_of(knob(p, 2), 3), m2 = (knob(p, 3) * knob(p, 3)) >> 15;
    int32_t wg = 2 * knob(p, 3) - m2;
    int32_t base = 66 + ((knob(p, 5) * 816) >> 15), tc = tone_coef(knob(p, 6)), d[3], dd[3], i, k;
    uint32_t inc = lfo_inc(knob(p, 0)), ph = s->lfo + (s->ch ? (uint32_t)knob(p, 7) << 16 : 0), w = s->w;

    s->lfo += inc * (uint32_t)m;
    ph += inc * (uint32_t)m;
    for (k = 0; k < nv; k++) {                  /* 1.5 .. 20 ms, +- 0 .. 3 ms */
        int32_t end = (base << 16) + tri32(ph + (uint32_t)k * 0x55555555u) * depth;

        end = end < (2 << 16) ? 2 << 16 : end;
        d[k] = s->apx[k] ? s->apx[k] : end;
        dd[k] = (end - d[k]) / m;
    }
    for (i = 0; i < m; i++, w++) {
        int32_t x = b[i], y = 0;

        for (k = 0; k < nv; k++) {
            d[k] += dd[k];
            y += tap(s->dl, w, (uint32_t)d[k]);
        }
        y = nv == 1 ? y : nv == 2 ? y >> 1 : (y * 10923) >> 15;
        s->lp += ((y - s->lp) * tc) >> 15;
        s->dl[w & DMASK] = (int16_t)x;
        b[i] = sat16(x + ((s->lp * wg - x * m2) >> 15));
    }
    for (k = 0; k < nv; k++)
        s->apx[k] = d[k];
    s->w = w;
}

/* soft limit: unity at 0, x - 4/27 x^3 up to 1.5x full scale (= full scale), clamped beyond */
static int32_t soft(int32_t v)
{
    int32_t a = clampi(v, -49152, 49152), t = ((((a >> 1) * (a >> 1)) >> 13) * 4855) >> 15;

    return a - ((a * t) >> 15);
}

/* carrier / LFO pitch for u = 0..32767: 10 Hz * 2^(8.6 u) (10 Hz .. ~3.9 kHz), 32-bit phase step */
static uint32_t ring_inc(int32_t u)
{
    return (uint32_t)(((uint64_t)973800u * exp2q((clampi(u, 0, 32767) * 1101) >> 6)) >> 16);
}

/* tuned comb on the flanger's line: FREQ 20 Hz .. 5 kHz (delay 1/f), +-0.92 feedback, damped,
 * LFO on the pitch, right channel up to half an octave higher (SPREAD) */
static void comb(fp_state *s, int16_t *b, int m, const int16_t *p)
{
    int32_t fbg = (cen(p, 1) * 3769) >> 11, damp = tone_coef(32767 - knob(p, 2)), mix = knob(p, 3);
    int32_t afb = fbg < 0 ? -fbg : fbg, in = 32768 - ((afb * 3) >> 2), u, i, dd;
    uint32_t inc = lfo_inc(knob(p, 6)), end, d, w = s->w;

    s->lfo += inc * (uint32_t)m;
    u = knob(p, 0) + ((sin32(s->lfo) * (knob(p, 5) >> 3)) >> 15) + (s->ch ? knob(p, 7) >> 4 : 0);
    end = (uint32_t)2205 * exp2q(-clampi(u, 0, 32767) * 16);         /* 44100 / 20 Hz, 16.16 */
    end = end > (2040u << 16) ? 2040u << 16 : end < (8u << 16) ? 8u << 16 : end;
    d = s->dcur ? s->dcur : end;
    dd = (int32_t)(end - d) / m;
    for (i = 0; i < m; i++, w++) {
        int32_t x = b[i], y;

        d += (uint32_t)dd;
        y = tap(s->dl, w, d);
        s->lp += ((y - s->lp) * damp) >> 15;
        s->dl[w & DMASK] = (int16_t)soft(((x * in) >> 15) + ((s->lp * fbg) >> 15));
        b[i] = sat16(x + (((y - x) * mix) >> 15));
    }
    s->dcur = d;
    s->w = w;
}

/* ring modulator: sine carrier (SHAPE drives it toward a square), LFO on its pitch, toned */
static void ring(fp_state *s, int16_t *b, int m, const int16_t *p)
{
    int32_t mix = knob(p, 3), sh = 256 + ((knob(p, 5) * 1792) >> 15), tc = tone_coef(knob(p, 6)), i;
    uint32_t inc = lfo_inc(knob(p, 2)), ph = (uint32_t)s->tx[0], ci;

    s->lfo += inc * (uint32_t)m;
    ci = ring_inc(knob(p, 0) + ((sin32(s->lfo) * (knob(p, 1) >> 2)) >> 15) + (s->ch ? knob(p, 7) >> 4 : 0));
    for (i = 0; i < m; i++, ph += ci) {
        int32_t x = b[i], c = sin32(ph), y;

        if (sh > 256)
            c = clampi((c * sh) >> 8, -32767, 32767);
        y = (x * c) >> 15;
        s->lp += ((y - s->lp) * tc) >> 15;
        b[i] = sat16(x + (((s->lp - x) * mix) >> 15));
    }
    s->tx[0] = (int32_t)ph;
}

/* tremolo: 0.1 .. 16 Hz sine LFO (SHAPE toward square) on the level; SPREAD offsets the right
 * channel's LFO up to 180 degrees = auto-pan on the master */
static void tremolo(fp_state *s, int16_t *b, int m, const int16_t *p)
{
    int32_t depth = knob(p, 1), sh = 256 + ((knob(p, 2) * 1792) >> 15), mix = knob(p, 3), i;
    uint32_t inc = lfo_inc(knob(p, 0)) * 2, ph = s->lfo + (s->ch ? (uint32_t)knob(p, 5) << 16 : 0);

    for (i = 0; i < m; i++, ph += inc) {
        int32_t x = b[i], l = sin32(ph), g;

        if (sh > 256)
            l = clampi((l * sh) >> 8, -32767, 32767);
        g = 32767 - ((depth * (32767 - ((l + 32767) >> 1))) >> 15);
        b[i] = sat16(x + (((((x * g) >> 15) - x) * mix) >> 15));
    }
    s->lfo += inc * (uint32_t)m;
}

/* DC blocker (bias, asymmetric curves) */
static int32_t dcb(fp_state *s, int32_t x)
{
    s->hpy = x - s->hpx + ((s->hpy * 32604) >> 15);
    s->hpx = x;
    return s->hpy;
}

static void fold(fp_state *s, int16_t *b, int m, const int16_t *p)
{
    int32_t g = 256 + ((knob(p, 0) * 1792) >> 15), bias = cen(p, 1), tc = tone_coef(knob(p, 2));
    int32_t mix = knob(p, 3), shape = knob(p, 5), lvl = knob(p, 6) * 2, i;

    for (i = 0; i < m; i++) {
        int32_t x = b[i], v = ((x * g) >> 8) + bias, u, tri, sn, y;

        u = (v + 32768) & 0x1ffff;              /* triangle fold, period 4 x full scale */
        tri = u < 65536 ? u - 32768 : 98304 - u;
        sn = sin32((uint32_t)v << 15);          /* sine fold: sin(pi/2 * v) */
        y = tri + (((sn - tri) * (shape >> 1)) >> 14);
        y = sat16(dcb(s, y));
        s->lp += ((y - s->lp) * tc) >> 15;
        y = (s->lp * lvl) >> 16;                /* LEVEL 0 .. 1x (folding is loud) */
        b[i] = sat16(x + (((sat16(y) - x) * mix) >> 15));
    }
}

static void fuzz(fp_state *s, int16_t *b, int m, const int16_t *p)
{
    int32_t g = 256 + ((knob(p, 0) * 3840) >> 15), tc = tone_coef(knob(p, 1)), c = step_of(knob(p, 2), FP_CURVES);
    int32_t mix = knob(p, 3), lvl = knob(p, 5) * 2, bias = cen(p, 6) >> 1, thr = (knob(p, 7) * knob(p, 7)) >> 18;
    int32_t gate, dg, i;

    /* GATE: follower of the input; below the threshold the effect fades out (~10 ms) */
    gate = s->envs >> 4 < thr ? 0 : 32767;
    dg = (gate - s->gate) / 16;
    for (i = 0; i < m; i++) {
        int32_t x = b[i], ax = x < 0 ? -x : x, y;

        s->envs += ((ax << 4) - s->envs) >> 9;
        y = curve(c, ((x * g) >> 8) + bias);
        y = sat16(dcb(s, y));
        s->lp += ((y - s->lp) * tc) >> 15;
        y = (((s->lp * lvl) >> 16) * s->gate) >> 15;   /* LEVEL 0 .. 1x (curves are loud) */
        s->gate += dg;
        if (!dg || (dg > 0 && s->gate > gate) || (dg < 0 && s->gate < gate))
            s->gate = gate;
        b[i] = sat16(x + (((sat16(y) - x) * mix) >> 15));
    }
}

/* drive -> sample and hold (RATE ~490 Hz .. 44.1 kHz, JITTER = random early clocks) -> BITS
 * (16 .. 1, rounded to zero) -> tone */
static void bitcrush(fp_state *s, int16_t *b, int m, const int16_t *p)
{
    int32_t step = (int32_t)(exp2q((32767 - knob(p, 0)) * 30) >> 16), half, i;
    int32_t jit = knob(p, 2), mix = knob(p, 3), tc = tone_coef(knob(p, 5)), dr = 256 + ((knob(p, 6) * 768) >> 15);
    uint32_t inc = exp2q(-(32767 - knob(p, 1)) * 13), inv;

    step = step < 1 ? 1 : step;
    half = step >> 1;
    inv = 65536u / (uint32_t)step;
    for (i = 0; i < m; i++) {
        int32_t x = b[i], y;

        s->tx[1] += (int32_t)inc;
        if (s->tx[1] >= 65536) {
            s->tx[1] -= 65536;
            if (jit)
                s->tx[1] += (int32_t)(((rnd(s) >> 17) * (uint32_t)jit) >> 15);
            y = dr > 256 ? curve(0, (x * dr) >> 8) : x;
            if (step > 1) {
                uint32_t a = (uint32_t)(y < 0 ? -y : y), q = ((a + (uint32_t)half) * inv) >> 16;

                y = y < 0 ? -(int32_t)(q * (uint32_t)step) : (int32_t)(q * (uint32_t)step);
            }
            s->tx[0] = sat16(y);
        }
        s->lp += ((s->tx[0] - s->lp) * tc) >> 15;
        b[i] = sat16(x + (((s->lp - x) * mix) >> 15));
    }
}

/* Simper trapezoidal SVF, coefficients Q14; returns low (hp = 0) or high (hp = 1) */
static int32_t svf(int32_t *ic, int32_t v0, int32_t a1, int32_t a2, int32_t a3, int32_t k, int hp)
{
    int32_t v3 = v0 - ic[1], v1 = (a1 * ic[0] + a2 * v3) >> 14, v2 = ic[1] + ((a2 * ic[0] + a3 * v3) >> 14);

    ic[0] = clampi(2 * v1 - ic[0], -SVF_CLAMP, SVF_CLAMP);
    ic[1] = clampi(2 * v2 - ic[1], -SVF_CLAMP, SVF_CLAMP);
    return hp ? v0 - ((k * v1) >> 14) - v2 : v2;
}

static void dj(fp_state *s, int16_t *b, int m, const int16_t *p)
{
    int32_t pos = knob(p, 0), k = 32768 - ((knob(p, 1) * 31130) >> 15), dr = 256 + ((knob(p, 2) * 1792) >> 15);
    int32_t mix = knob(p, 3), two = step_of(knob(p, 5), 2), wob = (knob(p, 7) * 48) >> 15, idx, hp, g, a1, a2, a3, i;
    uint32_t inc = lfo_inc(knob(p, 6));

    s->lfo += inc * (uint32_t)m;
    hp = pos >= 16384;
    idx = hp ? ((pos - 16384) * 200) >> 14 : 72 + (((pos) * 183) >> 14);   /* LP 150 Hz..19 kHz, HP 20 Hz..4.6 kHz */
    idx = clampi(idx + ((sin32(s->lfo) * wob) >> 15), 0, 255);
    g = fp_svf_g[idx];                                                   /* Q16 */
    a1 = (int32_t)(((int64_t)1 << 28) / ((1 << 14) + (int32_t)(((int64_t)g * (g + (int64_t)k * 4)) >> 18)));
    a2 = (int32_t)(((int64_t)g * a1) >> 16);
    a3 = (int32_t)(((int64_t)g * a2) >> 16);
    for (i = 0; i < m; i++) {
        int32_t x = b[i], v = dr > 256 ? curve(0, (x * dr) >> 8) : x;

        v = svf(s->ic, v, a1, a2, a3, k, hp);
        if (two)
            v = svf(s->ic + 2, v, a1, a2, a3, k, hp);
        b[i] = sat16(x + (((sat16(v) - x) * mix) >> 15));
    }
}

/* 4-pole ladder: four TPT one-poles (states 8 bits finer, steps from whole-LSB differences: at
 * most 1 LSB left standing at low cutoffs),
 * unit-delay feedback 0 .. 3.5 through a soft limit, cut toward the top (the delay would make it
 * ring on); passband loss half made up; cutoff from the knob, the input's envelope (ENV) and an
 * LFO (WOBBLE) */
static void ladder(fp_state *s, int16_t *b, int m, const int16_t *p)
{
    int32_t k = (knob(p, 1) * 14336) >> 15, dr = 256 + ((knob(p, 2) * 768) >> 15), mix = knob(p, 3);
    int32_t mk = 4096 + (k >> 1), idx, g, G, env = s->envs, i, j;
    uint32_t inc = lfo_inc(knob(p, 6));

    s->lfo += inc * (uint32_t)m;
    idx = 40 + ((knob(p, 0) * 215) >> 15) + (((env >> 4) * ((knob(p, 5) * 128) >> 15)) >> 15)
          + ((sin32(s->lfo) * ((knob(p, 7) * 64) >> 15)) >> 15);
    g = fp_svf_g[clampi(idx, 0, 250)];
    G = (int32_t)(((int64_t)g << 14) / (65536 + g));                  /* g / (1 + g), Q14 */
    k = clampi(k - ((k * ((G * 22938) >> 14)) >> 14), 0, 14336);      /* k (1 - 1.4 G) */
    for (i = 0; i < m; i++) {
        int32_t x = b[i], ax = x < 0 ? -x : x, v = dr > 256 ? curve(0, (((x * dr) >> 8) * 101) >> 8) : x, y;

        env += ((ax << 4) - env) >> 8;                  /* drive: / 2.53 = SOFT's own gain, 1 .. 4x */
        y = soft(v - ((k * s->fb) >> 12)) * 256;
        for (j = 0; j < 4; j++) {
            int32_t d = (((y - s->apy[j]) >> 8) * G) >> 6;            /* 32-bit: diff in whole LSBs */

            y = d + s->apy[j];
            s->apy[j] = clampi(y + d, -(49152 << 8), 49152 << 8);
        }
        y >>= 8;
        s->fb = y;
        b[i] = sat16(x + (((sat16((y * mk) >> 12) - x) * mix) >> 15));
    }
    s->envs = env;
}

/* envelope filter: the input's level (SENS) sweeps a Chamberlin SVF (40 Hz .. 6.4 kHz, states 8
 * bits finer: no stuck offset at the bottom) up or down 5 octaves from FREQ; SPEED = follower
 * attack / release; MODE LP / BP (peak 2: a narrow band is quiet) / HP */
static void wah(fp_state *s, int16_t *b, int m, const int16_t *p)
{
    int32_t q = 23170 - ((knob(p, 1) * 21532) >> 15), mix = knob(p, 3), sp = knob(p, 5);
    int32_t mode = step_of(knob(p, 6), 3), down = step_of(knob(p, 7), 2), ca = 74 + ((sp * 1400) >> 15);
    int32_t cr = 2 + ((sp * 35) >> 15), env = s->envs, lo = s->ic[0], bd = s->ic[1], sw, idx, g, g2, f, i;

    sw = clampi((int32_t)(((int64_t)(env >> 4) * knob(p, 0)) >> 12), 0, 32767);
    idx = 26 + ((knob(p, 2) * 150) >> 15) + (((sw * 129) >> 15) * (down ? -1 : 1));
    g = fp_svf_g[clampi(idx, 0, 215)];                                 /* tan(pi fc / fs), Q16 */
    g2 = (g * g) >> 16;
    f = (int32_t)(((int64_t)2 * g * (65536 - (g2 >> 1) + ((3 * ((g2 * g2) >> 16)) >> 3))) >> 18);  /* 2 sin, Q14 */
    for (i = 0; i < m; i++) {
        int32_t x = b[i], ax = (x < 0 ? -x : x) << 4, hi, v;

        env += (int32_t)(((int64_t)(ax - env) * (ax > env ? ca : cr)) >> 15);
        hi = x * 256 - lo - (int32_t)(((int64_t)q * bd) >> 14);
        bd = clampi(bd + (int32_t)(((int64_t)f * hi) >> 14), -(90000 << 8), 90000 << 8);
        lo = clampi(lo + (int32_t)(((int64_t)f * bd) >> 14), -(90000 << 8), 90000 << 8);
        v = mode == 0 ? lo : mode == 1 ? (int32_t)(((int64_t)q * bd) >> 13) : hi;   /* BP x2 */
        b[i] = sat16(x + (((sat16(v >> 8) - x) * mix) >> 15));
    }
    s->envs = env;
    s->ic[0] = lo;
    s->ic[1] = bd;
}

/* Chamberlin band-pass, f and q Q14, input pre-scaled by q (peak gain 1) */
static int32_t cbp(int32_t *lo, int32_t *bd, int32_t in, int32_t f, int32_t q)
{
    int32_t hi = in - *lo - ((q * *bd) >> 14);

    *bd = clampi(*bd + ((f * hi) >> 14), -90000, 90000);
    *lo = clampi(*lo + ((f * *bd) >> 14), -90000, 90000);
    return *bd;
}

/* formant filter: three band-passes morphing A E I O U (VOWEL, plus LFO), SHIFT +-1 octave,
 * x3 made up (the bands pass little of a full spectrum) */
static void vowel(fp_state *s, int16_t *b, int m, const int16_t *p)
{
    static const int16_t fmt[5][3] = {
        { 800, 1150, 2900 }, { 400, 1700, 2600 }, { 280, 2250, 2900 }, { 450, 800, 2830 }, { 325, 700, 2530 },
    };
    static const int16_t gain[3] = { 32767, 16384, 8192 };
    int32_t q = 4096 - ((knob(p, 1) * 3440) >> 15), mix = knob(p, 3), sh = (int32_t)exp2q(cen(p, 2) * 4);
    int32_t f[3], vp, vi, fr, i, k;
    uint32_t inc = lfo_inc(knob(p, 5)), ph = s->lfo + (s->ch ? (uint32_t)knob(p, 7) << 16 : 0);

    s->lfo += inc * (uint32_t)m;
    vp = clampi(knob(p, 0) * 4 + ((sin32(ph) * knob(p, 6)) >> 15), 0, 4 * 32767);
    vi = vp >> 15 > 3 ? 3 : vp >> 15;
    fr = vp - (vi << 15);
    for (k = 0; k < 3; k++) {
        int32_t hz = fmt[vi][k] + (((fmt[vi + 1][k] - fmt[vi][k]) * fr) >> 15);

        hz = (hz * sh) >> 16;
        f[k] = clampi((hz * 2390) >> 10, 1, 13000);                    /* 2 pi f / fs, Q14 */
    }
    for (i = 0; i < m; i++) {
        int32_t x = b[i], in = (x * q) >> 14, y = 0;

        for (k = 0; k < 3; k++)
            y += (cbp(&s->tx[2 * k], &s->tx[2 * k + 1], in, f[k], q) * gain[k]) >> 15;
        b[i] = sat16(x + (((sat16(y * 3) - x) * mix) >> 15));
    }
}

static void transient(fp_state *s, int16_t *b, int m, const int16_t *p)
{
    int32_t sp = step_of(knob(p, 5), 3), fa = 4 + sp, sa = 8 + sp, rel = 11, t, e, target, dg, i;
    int32_t out = knob(p, 2) >> 2, mix = knob(p, 3), clip = knob(p, 6), f = s->envf, sl = s->envs;

    /* gain for this chunk from where the followers stood: t = how much louder the fast one is */
    t = f > (64 << 12) ? (int32_t)(((int64_t)(f - sl) << 15) / f) : 0;
    t = clampi(t, 0, 32767);
    e = (((cen(p, 0) * t) >> 15) + ((cen(p, 1) * (32767 - t)) >> 15)) * 8;     /* +-2 octaves */
    target = (int32_t)(exp2q(e) >> 4);                                        /* Q12 */
    dg = (target - s->gain) / m;
    for (i = 0; i < m; i++) {
        int32_t x = b[i], ax = (x < 0 ? -x : x) << 12, y;

        f += (ax - f) >> (ax > f ? fa : rel);
        sl += (ax - sl) >> (ax > sl ? sa : rel);
        s->gain += dg;
        y = sat16((((x * s->gain) >> 12) * out) >> 12);     /* OUTPUT 0 .. 2x */
        if (clip)
            y += ((curve(0, y) - y) * clip) >> 15;
        b[i] = sat16(x + (((sat16(y) - x) * mix) >> 15));
    }
    s->gain = target;
    s->envf = f;
    s->envs = sl;
}

/* Chamberlin SVF band-pass (texture noise colours), f and q Q15 */
static int32_t bp(int32_t *lo, int32_t *bd, int32_t in, int32_t f, int32_t q)
{
    int32_t hi = in - *lo - ((q * *bd) >> 15);

    *bd = clampi(*bd + ((f * hi) >> 15), -200000, 200000);
    *lo = clampi(*lo + ((f * *bd) >> 15), -200000, 200000);
    return *bd;
}

static void texture(fp_state *s, int16_t *b, int m, const int16_t *p)
{
    int kind = step_of(knob(p, 0), FP_KINDS);
    int32_t lvl = knob(p, 1) >> 2, tc = tone_coef(knob(p, 2)), br = knob(p, 3), dens = knob(p, 6), duck = knob(p, 7);
    int32_t amp, env, i, thr = 2 + ((dens * 60) >> 15);
    uint32_t inc = lfo_inc(knob(p, 5) >> 1);    /* breath 0.05 .. ~0.6 Hz */

    /* breath: slow sine on the texture's level */
    s->lfo += inc * (uint32_t)m;
    amp = lvl - ((((sin32(s->lfo) + 32768) >> 1) * ((lvl * br) >> 15)) >> 15);
    /* slow random walk for gusts / flutter / whistle drift (per chunk) */
    s->tx[4] = clampi(s->tx[4] + (int32_t)(rnd(s) >> 24) - 128, -16384, 16384);
    env = s->envs;
    for (i = 0; i < m; i++) {
        int32_t x = b[i], ax = x < 0 ? -x : x, wn = (int32_t)rnd(s) >> 16, n = 0, d;
        uint32_t r = s->rnd;

        env += ((ax << 4) - env) >> 8;
        switch (kind) {
        case 0:                                 /* VINYL: soft hiss, crackle, rare pops */
            s->tx[0] += (wn - s->tx[0]) >> 2;
            if (((r >> 8) & 0xffff) < (uint32_t)thr)
                s->tx[1] = (int32_t)(r >> 16) - 32768;
            if (((r >> 3) & 0x1fffff) < 3)
                s->tx[1] = (int32_t)((r & 1) ? 32767 : -32768);
            s->tx[1] -= s->tx[1] >> 2;
            n = (s->tx[0] >> 4) + s->tx[1];
            break;
        case 1:                                 /* TAPE: bright hiss, flutter */
            s->tx[0] += (wn - s->tx[0]) >> 3;
            n = (((wn - s->tx[0]) >> 2) * (24576 + (s->tx[4] >> 1))) >> 15;
            break;
        case 2:                                 /* RAIN: dense droplets */
            if (((r >> 8) & 0xffff) < (uint32_t)(thr * 24))
                s->tx[1] += ((int32_t)(r >> 16) - 32768) >> 1;
            s->tx[1] -= s->tx[1] >> 3;
            d = s->tx[1] - s->tx[2];            /* high-passed ticks */
            s->tx[2] = s->tx[1];
            s->tx[0] += (wn - s->tx[0]) >> 1;
            n = d + (s->tx[0] >> 5);
            break;
        case 3:                                 /* WIND: brown noise through a wandering band-pass */
            s->tx[3] += (wn >> 4) - (s->tx[3] >> 7);
            n = bp(&s->tx[5], &s->tx[6], s->tx[3], 2600 + ((s->tx[4] + 16384) >> 3), 9000);
            n = (n * (16384 + (s->tx[4] >> 1))) >> 15;
            break;
        case 4:                                 /* RADIO: narrow band noise, drifting whistle, crackle */
            n = bp(&s->tx[5], &s->tx[6], wn, 7800, 2600) >> 1;
            s->tx[7] = (int32_t)((uint32_t)s->tx[7] + 214000000u + (uint32_t)(s->tx[4] * 1024));   /* ~2.2 kHz +- drift */
            n += sin32((uint32_t)s->tx[7]) >> 4;
            if (((r >> 8) & 0xffff) < (uint32_t)thr)
                s->tx[1] = (int32_t)(r >> 17) - 16384;
            s->tx[1] -= s->tx[1] >> 2;
            n += s->tx[1];
            break;
        default:                                /* ROOM: low rumble, a little air */
            s->tx[3] += (wn >> 5) - (s->tx[3] >> 9);
            s->tx[0] += (s->tx[3] - s->tx[0]) >> 6;
            n = s->tx[0] * 2 + (wn >> 7);
            break;
        }
        s->lp += ((clampi(n, -32768, 32767) - s->lp) * tc) >> 15;
        n = (((s->lp * amp) >> 15) * 45875) >> 15;     /* 1.4x */
        if (duck)                               /* DUCK: louder input, quieter texture */
            n = (n * (32767 - clampi(((env >> 4) * duck) >> 14, 0, 32767))) >> 15;
        b[i] = sat16(x + n);
    }
    s->envs = env;
}

/* soft knee for the output gain: linear up to half scale, then v - 4/27 v^3 (in units of half
 * scale) up to full scale at 1.5x the knee, clamped beyond */
static int16_t knee(int32_t v)
{
    int32_t a = v < 0 ? -v : v, e;

    if (a <= 16384)
        return (int16_t)v;
    e = a - 16384 > 24576 ? 24576 : a - 16384;
    a = 16384 + e - ((((((e * e) >> 14) * e) >> 14) * 4855) >> 15);
    a = a > 32767 ? 32767 : a;
    return (int16_t)(v < 0 ? -a : a);
}

void fp_process(fp_state *s, int16_t *buf, int n, const int16_t *p)
{
    /* output gain 1.4, faded in over the first quarter of MIX (MIX 0 stays bit-exact); FUZZ is
     * loud already, TEXTURE's fourth knob is BREATH (its bed is made 1.4x louder itself) */
    int32_t og = s->type == FP_FUZZ || s->type == FP_TEXTURE ? 0
                 : 32768 + ((clampi(knob(p, 3) * 4, 0, 32767) * 13107) >> 15);

    while (n > 0) {
        int m = n < 64 ? n : 64, i;
        int32_t pk = s->level;

        switch (s->type) {
        case FP_PHASER:     phaser(s, buf, m, p); break;
        case FP_FLANGER:    flanger(s, buf, m, p); break;
        case FP_CHORUS:     chorus(s, buf, m, p); break;
        case FP_COMB:       comb(s, buf, m, p); break;
        case FP_RING:       ring(s, buf, m, p); break;
        case FP_TREMOLO:    tremolo(s, buf, m, p); break;
        case FP_FOLD:       fold(s, buf, m, p); break;
        case FP_FUZZ:       fuzz(s, buf, m, p); break;
        case FP_BITCRUSH:   bitcrush(s, buf, m, p); break;
        case FP_DJ:         dj(s, buf, m, p); break;
        case FP_LADDER:     ladder(s, buf, m, p); break;
        case FP_WAH:        wah(s, buf, m, p); break;
        case FP_VOWEL:      vowel(s, buf, m, p); break;
        case FP_TRANSIENT:  transient(s, buf, m, p); break;
        default:            texture(s, buf, m, p); break;
        }
        for (i = 0; i < m; i++) {
            int32_t a;

            if (og > 32768)
                buf[i] = knee((buf[i] * og) >> 15);
            a = buf[i] < 0 ? -buf[i] : buf[i];
            s->scope[i] = buf[i];
            pk = a > pk ? a : pk;
        }
        s->level = pk - (pk >> 4);
        buf += m;
        n -= m;
    }
}
