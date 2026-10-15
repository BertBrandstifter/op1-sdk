/* FXPACK effect core, see fxpack_core.h and README.md. Tables: fp_tables.h (gen_tables.py). */
#include "fxpack_core.h"
#include "fp_tables.h"

#define DMASK       (FP_DL_LEN - 1)
#define SVF_CLAMP   60000                       /* SVF states: two Q14 products always fit */

static const char *const names[FP_TYPES] = {
    "PHASER", "FLANGER", "FOLD", "FUZZ", "DJ FILTER", "TRANSIENT", "TEXTURE",
};
static const char *const pnames[FP_TYPES][8] = {
    { "RATE", "DEPTH", "FEEDBACK", "MIX", "TYPE", "CENTER", "STAGES", "SPREAD" },
    { "RATE", "DEPTH", "FEEDBACK", "MIX", "TYPE", "DELAY", "DAMP", "SPREAD" },
    { "DRIVE", "BIAS", "TONE", "MIX", "TYPE", "SHAPE", "LEVEL", "-" },
    { "DRIVE", "TONE", "CHAR", "MIX", "TYPE", "LEVEL", "BIAS", "GATE" },
    { "FILTER", "RESO", "DRIVE", "MIX", "TYPE", "SLOPE", "LFO", "WOBBLE" },
    { "ATTACK", "SUSTAIN", "OUTPUT", "MIX", "TYPE", "SPEED", "CLIP", "-" },
    { "KIND", "LEVEL", "TONE", "BREATH", "TYPE", "RATE", "DENSITY", "DUCK" },
};
static const char *const curves[FP_CURVES] = { "SOFT", "HARD", "DIODE", "FUZZ", "CHEBY" };
static const char *const kinds[FP_KINDS] = { "VINYL", "TAPE", "RAIN", "WIND", "RADIO", "ROOM" };
static const char *const stages[3] = { "4", "6", "8" };
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

int fp_type_of(int16_t v)
{
    return step_of(v < 0 ? 0 : v, FP_TYPES);
}

const char *fp_name(int type)
{
    return names[type < 0 || type >= FP_TYPES ? 0 : type];
}

const char *fp_param_name(int type, int i)
{
    return pnames[type < 0 || type >= FP_TYPES ? 0 : type][i & 7];
}

const char *fp_choice(int type, int i, int16_t v)
{
    int32_t k = v < 0 ? 0 : v;

    if (i == 4)
        return names[fp_type_of(v)];
    if (type == FP_PHASER && i == 6)
        return stages[step_of(k, 3)];
    if (type == FP_FUZZ && i == 2)
        return curves[step_of(k, FP_CURVES)];
    if (type == FP_DJ && i == 5)
        return slopes[step_of(k, 2)];
    if (type == FP_TEXTURE && i == 0)
        return kinds[step_of(k, FP_KINDS)];
    return 0;
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

/* DC blocker (bias, asymmetric curves) */
static int32_t dcb(fp_state *s, int32_t x)
{
    s->hpy = x - s->hpx + ((s->hpy * 32604) >> 15);
    s->hpx = x;
    return s->hpy;
}

static void fold(fp_state *s, int16_t *b, int m, const int16_t *p)
{
    int32_t g = 256 + ((knob(p, 0) * 3840) >> 15), bias = cen(p, 1), tc = tone_coef(knob(p, 2));
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
    int32_t g = 256 + ((knob(p, 0) * 7936) >> 15), tc = tone_coef(knob(p, 1)), c = step_of(knob(p, 2), FP_CURVES);
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
        n = (s->lp * amp) >> 15;
        if (duck)                               /* DUCK: louder input, quieter texture */
            n = (n * (32767 - clampi(((env >> 4) * duck) >> 14, 0, 32767))) >> 15;
        b[i] = sat16(x + n);
    }
    s->envs = env;
}

void fp_process(fp_state *s, int16_t *buf, int n, const int16_t *p)
{
    while (n > 0) {
        int m = n < 64 ? n : 64, i;
        int32_t pk = s->level;

        switch (s->type) {
        case FP_PHASER:     phaser(s, buf, m, p); break;
        case FP_FLANGER:    flanger(s, buf, m, p); break;
        case FP_FOLD:       fold(s, buf, m, p); break;
        case FP_FUZZ:       fuzz(s, buf, m, p); break;
        case FP_DJ:         dj(s, buf, m, p); break;
        case FP_TRANSIENT:  transient(s, buf, m, p); break;
        default:            texture(s, buf, m, p); break;
        }
        for (i = 0; i < m; i++) {
            int32_t a = buf[i] < 0 ? -buf[i] : buf[i];

            s->scope[i] = buf[i];
            pk = a > pk ? a : pk;
        }
        s->level = pk - (pk >> 4);
        buf += m;
        n -= m;
    }
}
