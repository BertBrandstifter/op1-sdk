/* PALETTE voice (see pl_core.h, README.md). Own code: a four-tone layered digital synth voice.
 *
 * WG: live band-limited waves (polyBLEP: saw, fat saw = two saws +-8 cents, square, pulses, PWM
 * moved by LFO 1), additive single cycles from mip-mapped tables (one level per octave, harmonics
 * below 20 kHz, linear interpolation), noise (white, pink). TVF: Chamberlin state-variable filter
 * (88.2 kHz, f <= 14 kHz: stable), LPF = two in a row (the second without resonance) = 24 dB, BPF
 * (unity peak), HPF; states limited (a soft ceiling under high resonance). TVA: the envelope squared
 * x LEVEL x LFO, ramped per sample. Envelopes: linear segments per block (128 samples, 1.45 ms),
 * times 0..100 = 0 ms .. 20 s, scaled by the palette's TIME. LFOs per note per block. Output: the
 * tones' sum, a tanh knee above 20000 (up to 28000). */
#include "pl_core.h"
#include "pl_tables.h"

#define ONE24       (1 << 24)
#define BLK_US      1451u                       /* 128 samples at 88.2 kHz */
#define INC_C4      12740343u                   /* 261.63 Hz */
#define TONE_G      26214                       /* a full tone: peak ~11.6k */
#define KNEE        20000
#define S_MAX       (121 * 256)                 /* cutoff position cap, semitones above 20 Hz Q8 */
#define LIM(x, m)   ((x) > (m) ? (m) : (x) < -(m) ? -(m) : (x))

const char *const pl_wave_names[PL_WAVES] = {
    "SAW", "FAT SAW", "SQUARE", "PULSE 25", "PULSE 12", "PWM", "TRIANGLE", "SINE", "DIGI 1", "DIGI 2",
    "WIRE", "VOX A", "VOX O", "ORGAN", "E.PIANO", "CLAV", "SYN BASS", "GLASS", "REED", "BRASS",
    "SOFT PAD", "SPECTRAL", "WHITE", "PINK",
};
const char *const pl_wave_short[PL_WAVES] = {
    "SAW", "FSAW", "SQR", "P25", "P12", "PWM", "TRI", "SINE", "DIG1", "DIG2",
    "WIRE", "VOXA", "VOXO", "ORGN", "EPNO", "CLAV", "SBAS", "GLAS", "REED", "BRAS",
    "SPAD", "SPEC", "WHIT", "PINK",
};

#include "pl_bank.inc"

const pl_patch *pl_test_patch;                  /* tests: play this patch in place of the bank's */

static const pl_patch *patch_of(const pl_params *p)
{
    if (pl_test_patch)
        return pl_test_patch;
    return &pl_patches[p->patch < pl_npatches ? p->patch : pl_npatches - 1];
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int32_t pl_log2(uint32_t x)
{
    int e = 31;
    uint32_t m;
    int idx, fr;

    if (!x)
        return 0;
    while (!(x & 0x80000000u)) {
        x <<= 1;
        e--;
    }
    m = x >> 15;
    idx = (int)((m - 65536) >> 8);
    fr = (int)(m & 255);
    return (e << 16) + (int32_t)(pl_tlog2[idx] + (((pl_tlog2[idx + 1] - pl_tlog2[idx]) * fr) >> 8));
}

uint32_t pl_exp2(int32_t l)
{
    int e = l >> 16, idx = (l >> 8) & 255, fr = l & 255;
    uint32_t m = pl_texp2[idx] + (((pl_texp2[idx + 1] - pl_texp2[idx]) * fr) >> 8);

    if (e < 0)
        return 0;
    return e >= 16 ? m << (e - 16) : m >> (16 - e);
}

int pl_pitch_semis(int step) { return pl_pitch_st[clampi(step, 0, PL_PITCHES - 1)]; }
int pl_level_pct(int level) { return (clampi(level, 0, PL_LEVELS - 1) * 100 + 15) / 31; }
int pl_cut_offset(int cut) { return (clampi(cut, 0, PL_CUTS - 1) - PL_CUT0) * 5; }
int pl_time_x100(int t) { return (pl_tscale[clampi(t, 0, PL_TIMES - 1)] * 100 + 128) >> 8; }

void pl_patch_params(int patch, pl_params *p)
{
    int i;

    patch = clampi(patch, 0, pl_npatches - 1);
    p->patch = (uint8_t)patch;
    for (i = 0; i < PL_TONES; i++) {
        const pl_tone_def *d = &pl_patches[patch].t[i];

        p->level[i] = (uint8_t)((d->level * 31 + 50) / 100);
        p->wave[i] = d->wave;
        p->pitch[i] = d->pitch;
        p->cut[i] = PL_CUT0;
        p->time[i] = PL_TIME0;
    }
}

/* ------------------------------------------------------------------ envelopes, LFOs */

/* ramp step per block for a time 0..100 scaled by sc (Q8) */
static uint32_t seg_rate(int t, int sc)
{
    uint64_t d = (uint64_t)pl_time_us[clampi(t, 0, 100)] * (uint32_t)sc;

    if (d <= (uint64_t)BLK_US * 256)
        return ONE24;
    return (uint32_t)(((uint64_t)BLK_US << 32) / d);
}

static void go(pl_env *e, int stage, int32_t to, uint32_t rate)
{
    e->stage = (uint8_t)stage;
    e->from = e->e;
    e->to = to;
    e->r = 0;
    e->rate = rate;
}

/* one block of a segment: 1 = it reached its level */
static int tick(pl_env *e)
{
    e->r += e->rate;
    if (e->r >= ONE24) {
        e->e = e->to;
        return 1;
    }
    e->e = e->from + (((e->to - e->from) * (int32_t)(e->r >> 12)) >> 12);
    return 0;
}

static int32_t lva(int l) { return clampi(l, 0, 100) * 32767 / 100; }
static int32_t lvf(int l) { return clampi(l, -50, 50) * 32767 / 50; }

/* TVA (f = 0) / TVF (f = 1) envelope: stages 1-3 = the segments to L1, L2, SUS; 4 holds; 5 = release
 * (T4) to 0 / L4; 6 = the end */
static void env_run(pl_env *e, const pl_tone_def *d, int sc, int f)
{
    const uint8_t *t = f ? d->ft : d->at;

    switch (e->stage) {
    case 1:
        if (tick(e))
            go(e, 2, f ? lvf(d->fl[1]) : lva(d->al[1]), seg_rate(t[1], sc));
        break;
    case 2:
        if (tick(e))
            go(e, 3, f ? lvf(d->fl[2]) : lva(d->al[2]), seg_rate(t[2], sc));
        break;
    case 3:
        if (tick(e))
            e->stage = 4;
        break;
    case 5:
        if (tick(e))
            e->stage = 6;
        break;
    default:
        break;
    }
}

static void pitch_run(pl_env *e, const pl_tone_def *d)
{
    if (e->stage == 1) {
        if (tick(e))
            go(e, 2, 0, seg_rate(d->pe_t2, 256));
    } else if (e->stage == 2) {
        if (tick(e))
            e->stage = 4;
    }
}

static uint32_t rnd(uint32_t *s)
{
    uint32_t r = *s;

    r ^= r << 13;
    r ^= r >> 17;
    r ^= r << 5;
    return *s = r;
}

/* LFO i's value this block, Q15 (delay, fade, offset applied) */
static int32_t lfo_run(pl_voice *v, int i, const pl_lfo_def *d)
{
    uint32_t old = v->lph[i], ph, us, del, fade;
    int32_t y, a = 32767;

    v->lph[i] += pl_lfo_inc[clampi(d->rate, 0, 100)];
    if (v->lph[i] < old) {                      /* a new cycle: S&H / random take a new value */
        v->lold[i] = v->lnow[i];
        v->lnow[i] = (int32_t)(rnd(&v->lfsr) >> 17) - 16384;
        v->lnow[i] *= 2;
    }
    ph = v->lph[i] >> 16;
    switch (d->wave) {
    case PL_LSAW: y = (int32_t)ph - 32768; break;
    case PL_LSQR: y = ph < 32768 ? 32767 : -32767; break;
    case PL_LSH: y = v->lnow[i]; break;
    case PL_LRND: y = v->lold[i] + (((v->lnow[i] - v->lold[i]) * (int32_t)(ph >> 1)) >> 15); break;
    default: y = ph < 32768 ? -32767 + 2 * (int32_t)ph : 32767 - 2 * ((int32_t)ph - 32768); break;
    }
    if (d->offset > 0)
        y = (y + 32767) >> 1;
    else if (d->offset < 0)
        y = (y - 32767) >> 1;
    us = v->blocks * BLK_US;
    del = pl_time_us[clampi(d->delay, 0, 100)];
    if (us < del)
        return 0;
    if (d->fade) {
        fade = pl_time_us[clampi(d->fade < 0 ? -2 * d->fade : 2 * d->fade, 0, 100)];
        us -= del;
        a = us >= fade || !fade ? 32767 : (int32_t)(((uint64_t)us * 32767) / fade);
        if (d->fade < 0)
            a = 32767 - a;
    }
    return (y * a) >> 15;
}

/* ------------------------------------------------------------------ notes */

void pl_voice_init(pl_voice *v)
{
    static uint32_t seeds;
    int i;
    uint8_t *b = (uint8_t *)v;

    for (i = 0; i < (int)sizeof(*v); i++)
        b[i] = 0;
    v->lfsr = 0x2545f491u + 0x9e3779b9u * ++seeds;
    v->key = 60 << 8;
}

static int32_t c4_log(void)
{
    static int32_t c4;

    if (!c4)
        c4 = pl_log2(INC_C4);
    return c4;
}

void pl_note(pl_voice *v, uint32_t inc, const pl_params *p)
{
    const pl_patch *pa = patch_of(p);
    int i;

    v->on = 1;
    v->blocks = 0;
    v->key = (60 << 8) + (((pl_log2(inc) - c4_log()) * 3072) >> 16);
    for (i = 0; i < 2; i++) {
        v->lph[i] = pa->lfo[i].ktrig ? 0x40000000u : rnd(&v->lfsr);
        v->lold[i] = v->lnow[i] = 0;
    }
    for (i = 0; i < PL_TONES; i++) {
        pl_tone_st *s = &v->t[i];
        const pl_tone_def *d = &pa->t[i];
        int sc = pl_tscale[clampi(p->time[i], 0, PL_TIMES - 1)];

        s->ph = 0;
        s->ph2 = 0x80000000u;
        s->lp = s->bp = s->lp2 = s->bp2 = s->pink = s->pw = 0;
        s->gain = 0;
        s->ae.e = 0;
        go(&s->ae, 1, lva(d->al[0]), seg_rate(d->at[0], sc));
        s->fe.e = 0;
        go(&s->fe, 1, lvf(d->fl[0]), seg_rate(d->ft[0], sc));
        s->pe.e = d->pe_l0 * 16;
        go(&s->pe, 1, d->pe_l1 * 16, seg_rate(d->pe_t1, 256));
    }
}

void pl_release(pl_voice *v, const pl_params *p)
{
    const pl_patch *pa = patch_of(p);
    int i;

    for (i = 0; i < PL_TONES; i++) {
        pl_tone_st *s = &v->t[i];
        const pl_tone_def *d = &pa->t[i];
        int sc = pl_tscale[clampi(p->time[i], 0, PL_TIMES - 1)];

        if (s->ae.stage >= 1 && s->ae.stage <= 4)
            go(&s->ae, 5, 0, seg_rate(d->at[3], sc));
        if (s->fe.stage >= 1 && s->fe.stage <= 4)
            go(&s->fe, 5, lvf(d->fl[3]), seg_rate(d->ft[3], sc));
    }
}

int pl_sounding(const pl_voice *v)
{
    int i;

    if (!v->on)
        return 0;
    for (i = 0; i < PL_TONES; i++)
        if ((v->t[i].ae.stage >= 1 && v->t[i].ae.stage <= 5) || v->t[i].gain)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ the wave generator */

/* polyBLEP residual at t (phase Q16) for step dt (Q16), rdt = 2^30 / dt; +-1 = +-65536 */
static inline __attribute__((always_inline)) int32_t blep(int32_t t, int32_t dt, int32_t rdt)
{
    int32_t x;

    if (t < dt) {
        x = (t * rdt) >> 15;
        return 4 * x - ((x * x) >> 14) - 65536;
    }
    if (t > 65536 - dt) {
        x = ((t - 65536) * rdt) >> 15;
        return ((x * x) >> 14) + 4 * x + 65536;
    }
    return 0;
}

static int msb(uint32_t x)
{
    int n = 0;

    if (x >> 16) { x >>= 16; n += 16; }
    if (x >> 8) { x >>= 8; n += 8; }
    if (x >> 4) { x >>= 4; n += 4; }
    if (x >> 2) { x >>= 2; n += 2; }
    if (x >> 1) n += 1;
    return n;
}

#define SAT16(y)    ((int16_t)((y) > 32767 ? 32767 : (y) < -32767 ? -32767 : (y)))
#define M16(a, b)   ((int32_t)(int16_t)(a) * (int16_t)(b))     /* 16 x 16 multiply */

static void saw(int16_t *x, uint32_t *php, uint32_t inc, int n, int add)
{
    uint32_t ph = *php;
    int32_t dt = (int32_t)(inc >> 16), rdt, t, y;
    int i;

    dt = dt < 1 ? 1 : dt;
    rdt = (int32_t)((1u << 30) / (uint32_t)dt);
    for (i = 0; i < n; i++, ph += inc) {
        t = (int32_t)(ph >> 16);
        y = (32767 - t) + (blep(t, dt, rdt) >> 1);
        x[i] = add ? (int16_t)((x[i] + SAT16(y)) >> 1) : SAT16(y);
    }
    *php = ph;
}

/* pulse width pw0 -> pw1 over the block (Q16; PWM moves it smoothly: no block-rate sidebands) */
static void pulse(int16_t *x, uint32_t *php, uint32_t inc, int32_t pw0, int32_t pw1, int n)
{
    uint32_t ph = *php;
    int32_t dt = (int32_t)(inc >> 16), rdt, t, y, pwq = pw0 << 8, dpw = ((pw1 - pw0) << 8) / n, pw;
    int i;

    dt = dt < 1 ? 1 : dt;
    rdt = (int32_t)((1u << 30) / (uint32_t)dt);
    for (i = 0; i < n; i++, ph += inc, pwq += dpw) {
        t = (int32_t)(ph >> 16);
        pw = pwq >> 8;
        y = (t < pw ? 32767 : -32768) + (blep(t, dt, rdt) >> 1) - (blep((t - pw) & 0xffff, dt, rdt) >> 1)
            + (32768 - pw);
        x[i] = SAT16((y * 9) >> 4);             /* 9/16: the narrow pulse's top fits; square rms = saw's */
    }
    *php = ph;
}

static void table(int16_t *x, int w, uint32_t *php, uint32_t inc, int n)
{
    uint32_t ph = *php;
    int o = msb(inc) - 20, bits, sh, i;
    uint32_t mask;
    const int16_t *b;

    o = o < 0 ? 0 : o >= PL_MIPS ? PL_MIPS - 1 : o;
    bits = pl_wbits[o];
    sh = 32 - bits;
    mask = (1u << bits) - 1;
    b = pl_wt + pl_woff[w][o];
    for (i = 0; i < n; i++, ph += inc) {        /* (c - a) / 2 x frac Q14: 16 x 16 */
        uint32_t k = ph >> sh;
        int32_t a = b[k], c = b[(k + 1) & mask];

        x[i] = (int16_t)(a + (M16((c - a) >> 1, (ph >> (sh - 14)) & 0x3fff) >> 13));
    }
    *php = ph;
}

static void wg(pl_voice *v, pl_tone_st *s, int w, uint32_t inc, int32_t lfo1, int16_t *x, int n)
{
    int i;

    switch (w) {
    case PL_SAW:
        saw(x, &s->ph, inc, n, 0);
        break;
    case PL_FATSAW:                             /* +-8 cents */
        saw(x, &s->ph, inc + (uint32_t)(((uint64_t)inc * 303) >> 16), n, 0);
        saw(x, &s->ph2, inc - (uint32_t)(((uint64_t)inc * 302) >> 16), n, 1);
        break;
    case PL_SQUARE: pulse(x, &s->ph, inc, 32768, 32768, n); break;
    case PL_PULSE25: pulse(x, &s->ph, inc, 16384, 16384, n); break;
    case PL_PULSE12: pulse(x, &s->ph, inc, 8192, 8192, n); break;
    case PL_PWM: {                              /* 10 .. 90 % */
        int32_t pw = 32768 + ((lfo1 * 26214) >> 15);

        pulse(x, &s->ph, inc, s->pw ? s->pw : pw, pw, n);
        s->pw = pw;
        break;
    }
    case PL_WHITE:
    case PL_PINK: {
        uint32_t r = v->lfsr;
        int32_t pk = s->pink;

        for (i = 0; i < n; i++) {
            r ^= r << 13;
            r ^= r >> 17;
            r ^= r << 5;
            if (w == PL_WHITE) {
                x[i] = (int16_t)((int32_t)(r >> 16) - 32768);
            } else {
                pk += M16(((int32_t)(r >> 17) - 16384) - (pk >> 1), 8696) >> 15;
                x[i] = SAT16(pk * 2);
            }
        }
        v->lfsr = r;
        s->pink = pk;
        break;
    }
    default:
        table(x, clampi(w - PL_TRI, 0, PL_TWAVES - 1), &s->ph, inc, n);
        break;
    }
}

/* ------------------------------------------------------------------ TVF + TVA */

/* Blackfin fractional ops (single cycle, saturating); the same arithmetic on the host for the tests.
 * fr16 = Q15, fr32 = Q31 */
#ifdef __bfin__
#define MULT(a, b)  __builtin_bfin_mult_fr1x32((a), (b))      /* fr16 x fr16 -> fr32 */
#define ADD(a, b)   __builtin_bfin_add_fr1x32((a), (b))
#define SUB(a, b)   __builtin_bfin_sub_fr1x32((a), (b))
#else
static int32_t sat32(int64_t x) { return x > 0x7fffffff ? 0x7fffffff : x < -0x7fffffff - 1 ? -0x7fffffff - 1 : (int32_t)x; }
static int32_t MULT(int16_t a, int16_t b) { return a == -32768 && b == -32768 ? 0x7fffffff : ((int32_t)a * b) * 2; }
static int32_t ADD(int32_t a, int32_t b) { return sat32((int64_t)a + b); }
static int32_t SUB(int32_t a, int32_t b) { return sat32((int64_t)a - b); }
#endif

/* the TVF (Chamberlin SVF, states Q31 saturating at +-1: a soft ceiling under resonance; f Q15,
 * qh = damping / 2 Q15) on x / 4, then the TVA ramp from the last gain to g1 (Q15), added into acc.
 * One pass of single-cycle fractional ops. LPF = a second SVF (damping 1.3, no peak): 24 dB */
#define SVF(in, lp, bp, f, qh) do {                                 \
        int32_t t_ = MULT(qh, (int16_t)(bp >> 16));                 \
        lp = ADD(lp, MULT(f, (int16_t)(bp >> 16)));                 \
        hp = SUB(SUB(SUB(in, lp), t_), t_);                         \
        bp = ADD(bp, MULT(f, (int16_t)(hp >> 16)));                 \
    } while (0)

static void tvf_tva(pl_tone_st *s, int mode, int32_t pos, int reso, const int16_t *x, int32_t *acc, int n,
                    int32_t g1)
{
    int ci = pos >> 8, i;
    int16_t f = (int16_t)(pl_svf_f[ci] + (((pl_svf_f[ci + 1] - pl_svf_f[ci]) * (pos & 255)) >> 8));
    int16_t qh = (int16_t)pl_svf_q[clampi(reso, 0, 100)];
    int32_t lp = s->lp, bp = s->bp, lp2 = s->lp2, bp2 = s->bp2, hp, in;
    int32_t g = s->gain, dl = n == 128 ? (g1 - g) >> 7 : (g1 - g) / n;

    switch (mode) {
    case PL_LPF:
        for (i = 0; i < n; i++) {
            in = (int32_t)(x[i] >> 2) << 16;
            SVF(in, lp, bp, f, qh);
            SVF(lp, lp2, bp2, f, 21299);
            g += dl;
            acc[i] += M16(lp2 >> 16, g) >> 14;
        }
        break;
    case PL_BPF:                                /* unity at the peak: bp x damping */
        for (i = 0; i < n; i++) {
            in = (int32_t)(x[i] >> 2) << 16;
            SVF(in, lp, bp, f, qh);
            in = MULT(qh, (int16_t)(bp >> 16));
            in = ADD(in, in);
            g += dl;
            acc[i] += M16(in >> 16, g) >> 14;
        }
        break;
    case PL_HPF:
        for (i = 0; i < n; i++) {
            in = (int32_t)(x[i] >> 2) << 16;
            SVF(in, lp, bp, f, qh);
            g += dl;
            acc[i] += M16(hp >> 16, g) >> 14;
        }
        break;
    default:                                    /* no filter */
        for (i = 0; i < n; i++) {
            g += dl;
            acc[i] += M16(x[i] >> 2, g) >> 14;
        }
        break;
    }
    s->lp = lp;
    s->bp = bp;
    s->lp2 = lp2;
    s->bp2 = bp2;
    s->gain = g1;
}

/* ------------------------------------------------------------------ render */

static inline __attribute__((always_inline)) int32_t tnh(int32_t x)
{
    int32_t u, i;

    x = x < -65536 ? -65536 : x > 65535 ? 65535 : x;
    u = x + 65536;
    i = u >> 8;
    return pl_tanh[i] + (((pl_tanh[i + 1] - pl_tanh[i]) * (u & 255)) >> 8);
}

void pl_render(pl_voice *v, int16_t *out, int n, const uint32_t *inc, const pl_params *p)
{
    const pl_patch *pa = patch_of(p);
    int32_t acc[128], lfo[2], base, rel;
    int16_t x[128];
    int i, t, live = 0;

    n = n > 128 ? 128 : n;
    if (!v->on) {
        for (i = 0; i < n; i++)
            out[i] = 0;
        return;
    }
    base = pl_log2((inc[0] >> 1) + (inc[n - 1] >> 1));
    rel = base - c4_log();
    lfo[0] = lfo_run(v, 0, &pa->lfo[0]);
    lfo[1] = lfo_run(v, 1, &pa->lfo[1]);
    for (i = 0; i < n; i++)
        acc[i] = 0;

    for (t = 0; t < PL_TONES; t++) {
        pl_tone_st *s = &v->t[t];
        const pl_tone_def *d = &pa->t[t];
        int sc = pl_tscale[clampi(p->time[t], 0, PL_TIMES - 1)];
        int32_t g1, pk, pos, l, m;
        uint32_t ti;

        env_run(&s->ae, d, sc, 0);
        env_run(&s->fe, d, sc, 1);
        pitch_run(&s->pe, d);
        /* TVA: envelope squared x LEVEL x LFO x the tone gain */
        g1 = (s->ae.e * s->ae.e) >> 15;
        g1 = (g1 * (clampi(p->level[t], 0, 31) * 1057)) >> 15;
        if (d->a_lfo) {
            m = 32768 + (lfo[(d->lfo_sel >> 2) & 1] * d->a_lfo) / 50;
            g1 = (g1 * clampi(m, 0, 65535)) >> 15;
        }
        g1 = (g1 * TONE_G) >> 15;
        g1 = g1 > 32767 ? 32767 : g1;
        if (!g1 && !s->gain)
            continue;                           /* silent: no oscillator, no filter */
        live = 1;

        /* WG: key follow, PITCH + fine, the pitch envelope, LFO */
        pk = d->p_kf ? base + (rel * d->p_kf) / 100 : base;
        pk += pl_pitch_semis(p->pitch[t]) * 5461 + d->fine * 55 + ((s->pe.e * 3495) >> 10);
        if (d->p_lfo)
            pk += (lfo[d->lfo_sel & 1] * d->p_lfo * 218) >> 15;
        ti = pl_exp2(pk);
        ti = ti > 0x60000000u ? 0x60000000u : ti < 1024 ? 1024 : ti;
        wg(v, s, clampi(p->wave[t], 0, PL_WAVES - 1), ti, lfo[0], x, n);

        /* TVF: cutoff (+ palette), envelope, key follow, LFO -> semitones above 20 Hz, Q8 */
        pos = 0;
        if (d->mode != PL_FOFF) {
            l = clampi(d->cutoff + pl_cut_offset(p->cut[t]), 0, 100);
            pos = l * 121 * 256 / 100;
            pos += (s->fe.e * d->f_depth * 368) >> 15;
            if (d->f_kf)
                pos += ((v->key - (60 << 8)) * d->f_kf) / 100;
            if (d->f_lfo)
                pos += (lfo[(d->lfo_sel >> 1) & 1] * d->f_lfo * 184) >> 15;
            pos = clampi(pos, 0, S_MAX - 1);
        }
        tvf_tva(s, d->mode, pos, d->reso, x, acc, n, g1);
    }
    if (v->blocks < 2000000u)
        v->blocks++;
    if (!live) {
        if (!pl_sounding(v))
            v->on = 0;
        for (i = 0; i < n; i++)
            out[i] = 0;
        return;
    }
    for (i = 0; i < n; i++) {                   /* linear below 20000, a tanh knee above */
        int32_t o = acc[i];

        if (o > KNEE)
            o = KNEE + ((tnh(((o - KNEE) * 2097) >> 10) * 1000) >> 11);
        else if (o < -KNEE)
            o = -KNEE - ((tnh(((-KNEE - o) * 2097) >> 10) * 1000) >> 11);
        out[i] = (int16_t)(o > 32767 ? 32767 : o < -32768 ? -32768 : o);
    }
}
