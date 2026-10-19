/* MONO voice (see mo_core.h, README.md). Own code: a classic 70s mono synth voice structure.
 *
 * Oscillators: naive waves plus a polyBLEP correction on the samples next to an edge (the block's
 * reciprocal step, no division per sample); the reference shapes and levels (triangle 1.25x the
 * others, the shark: up over half a cycle, a step, down faster). Mixer: linear. Filter: four linear
 * one-pole stages, feedback from the half-sample average of the 4th through a tanh (the level of
 * the self-oscillation; below it the filter is linear), the ladder's bass loss under
 * emphasis uncompensated. Cutoff in Hz = 41 * 2^(10 c) (+ keyboard control around F2) + the
 * contour in Hz (amount^2 x 4.95 kHz), capped at 12.3 kHz. Contours: attack 1 - (1 - t/T)^3.3,
 * decay (1 - t/T)^3.3 to the sustain level; the loudness contour multiplies the output. Coefficients
 * and contours per 32 samples, the loudness ramped per sample. Glide: constant time, linear in
 * pitch. */
#include "mo_core.h"
#include "mo_tables.h"

#define SUB         32
#define INC69       21426141u                   /* 440 Hz at 88.2 kHz */
#define ONE24       (1 << 24)
#define CUT_HZ16    (20000 * 16)                /* cutoff cap, Hz Q4: above it the filter is open */
/* ladder units = acc x 3/16; the feedback through tanh(y / L), L = 16384 = the tanh's unit: the
 * self-oscillation's level against the oscillators' as the reference */
#define OUT_GAIN    266667                      /* ladder -> output, x loudness Q15: a saw at full peaks
                                                 * ~12k, three ~37k (limited), as loud as TE's engines */
#define KNEE        20000                       /* output: linear below, above it tanh up to KNEE + 8000 */

int32_t mo_log2(uint32_t x)
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
    m = x >> 15;                                /* 1.xxx in Q16: 65536 .. 131071 */
    idx = (int)((m - 65536) >> 8);
    fr = (int)(m & 255);
    return (e << 16) + (int32_t)(mo_tlog2[idx] + (((mo_tlog2[idx + 1] - mo_tlog2[idx]) * fr) >> 8));
}

uint32_t mo_exp2(int32_t l)
{
    int e = l >> 16, idx = (l >> 8) & 255, fr = l & 255;
    uint32_t m = mo_texp2[idx] + (((mo_texp2[idx + 1] - mo_texp2[idx]) * fr) >> 8);   /* Q16 */

    if (e < 0)
        return 0;
    return e >= 16 ? m << (e - 16) : m >> (16 - e);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int mo_cents(int f) { return mo_tcents[f < 0 ? 0 : f >= MO_FREQS ? MO_FREQS - 1 : f]; }

uint32_t mo_time_10us(int t) { return mo_time[clampi(t, 0, MO_TIMES - 1)]; }
int mo_glide_ms(int g) { return mo_tglide_ms[clampi(g, 0, MO_GLIDES - 1)]; }
int mo_level_permille(int l) { return (mo_level[clampi(l, 0, MO_LEVELS - 1)] * 1000 + 16383) >> 15; }

void mo_voice_init(mo_voice *v)
{
    static uint32_t seeds;                      /* each voice its own noise (deterministic) */
    int i;

    v->ph[0] = v->ph[1] = v->ph[2] = 0;
    for (i = 0; i < 6; i++)
        v->lad[i] = 0;
    v->lfsr = 0x1d872b41u + 0x9e3779b9u * ++seeds;
    v->pink = 0;
    v->fenv.e = v->fenv.r = v->fenv.from = 0;
    v->fenv.stage = 0;
    v->lenv = v->fenv;
    v->glide = v->gstep = v->cur = 0;
    v->lgain = 0;
    v->key = 60;
    v->sounding = 0;
}

static void env_start(mo_env *e, int stage)
{
    e->stage = (uint8_t)stage;
    e->r = 0;
    e->from = e->e;
}

void mo_note(mo_voice *v, uint32_t inc, const mo_params *p)
{
    int32_t target = mo_log2(inc);

    v->glide = v->sounding && p->glide ? v->cur - target : 0;
    v->gstep = (int32_t)(((int64_t)(v->glide < 0 ? -v->glide : v->glide) * mo_glide[clampi(p->glide, 0, MO_GLIDES - 1)]) >> 16);
    if (v->glide && v->gstep < 1)
        v->gstep = 1;
    v->key = 69 + (((target - mo_log2(INC69)) * 12 + 32768) >> 16);
    v->ph[0] = v->ph[1] = v->ph[2] = 0x80000000u;   /* the oscillators start together, mid-cycle (as
                                                 * the reference: every note the same attack) */
    v->lad[0] += 2048;                          /* a kick: self-oscillation starts without input */
    env_start(&v->fenv, 1);                     /* from where the contours are (classic) */
    env_start(&v->lenv, 1);
    v->sounding = 1;
}

void mo_release(mo_voice *v)
{
    if (v->fenv.stage)
        env_start(&v->fenv, 3);
    if (v->lenv.stage)
        env_start(&v->lenv, 3);
}

/* the contour curve x^3.3, x Q15 */
static int32_t curve(int32_t x)
{
    int i = x >> 9, fr = x & 511;

    return mo_pow[i] + (((mo_pow[i + 1] - mo_pow[i]) * fr) >> 9);
}

/* one 32-sample step of a contour: attack / decay rates (Q24), sustain Q15, release rate */
static void env_run(mo_env *e, uint32_t ra, uint32_t rd, int32_t sus, uint32_t rr)
{
    int32_t x;

    switch (e->stage) {
    case 1:
        e->r += (int32_t)ra;
        if (e->r >= ONE24) {
            e->stage = 2;
            e->r = 0;
            e->e = 32767;
            break;
        }
        x = 32767 - (e->r >> 9);
        e->e = e->from + (((32767 - e->from) * (32767 - curve(x))) >> 15);
        break;
    case 2:
        e->r = e->r + (int32_t)rd > ONE24 ? ONE24 : e->r + (int32_t)rd;
        x = 32767 - (e->r >= ONE24 ? 32767 : e->r >> 9);
        e->e = sus + (((32767 - sus) * curve(x)) >> 15);
        break;
    case 3:
        e->r = e->r + (int32_t)rr > ONE24 ? ONE24 : e->r + (int32_t)rr;
        x = 32767 - (e->r >= ONE24 ? 32767 : e->r >> 9);
        e->e = (e->from * curve(x)) >> 15;
        break;
    default:
        e->e = 0;
        break;
    }
}

/* polyBLEP residual at t (phase Q16) for step dt (Q16) with rdt = 2^30 / dt; Q16, +-1 = +-65536 */
static inline __attribute__((always_inline)) int32_t blep(int32_t t, int32_t dt, int32_t rdt)
{
    int32_t x;

    if (t < dt) {                               /* x = t / dt in Q15: 2x - x^2 - 1 */
        x = (t * rdt) >> 15;
        return 4 * x - ((x * x) >> 14) - 65536;
    }
    if (t > 65536 - dt) {                       /* x = (t - 1) / dt: x^2 + 2x + 1 */
        x = ((t - 65536) * rdt) >> 15;
        return ((x * x) >> 14) + 4 * x + 65536;
    }
    return 0;
}

/* one oscillator added into acc: wave w (osc 3's shark = a rising saw), phase step inc, level Q15.
 * Waves: +-32768 = reference 0.1 (saw, square, pulses), triangle +-40960; acc = 1/4 of that
 * (MIX: 16 x 16-bit multiplies) */
#define MIX(y)      ((int32_t)(int16_t)((y) >> 2) * lv >> 15)
static void osc_add(int w, int rev, uint32_t *php, uint32_t inc, int32_t lvl, int32_t *acc, int n)
{
    uint32_t ph = *php;
    int32_t dt = (int32_t)(inc >> 16), rdt, t, y, pw;
    int16_t lv;
    int i;

    if (dt < 1)
        dt = 1;
    rdt = (int32_t)((1u << 30) / (uint32_t)dt);
    if (lvl <= 0) {
        *php = ph + inc * (uint32_t)n;
        return;
    }
    lv = (int16_t)lvl;
    switch (w) {
    case MO_TRI:
        for (i = 0; i < n; i++, ph += inc) {
            t = (int32_t)(ph >> 16);
            y = t < 32768 ? -40960 + ((t * 5) >> 1) : 40960 - (((t - 32768) * 5) >> 1);
            acc[i] += MIX(y);
        }
        break;
    case MO_SHARK:
        if (rev) {                              /* osc 3: reverse (rising) saw */
            for (i = 0; i < n; i++, ph += inc) {
                t = (int32_t)(ph >> 16);
                y = (t - 32768) - (blep(t, dt, rdt) >> 1);
                acc[i] += MIX(y);
            }
            break;
        }
        for (i = 0; i < n; i++, ph += inc) {    /* up 0.3 per half cycle, a step of 0.082, down 0.47 */
            t = (int32_t)(ph >> 16);
            y = t < 32768 ? -31539 + ((t * 1551) >> 10) : 44974 - (((t - 32768) * 2391) >> 10);
            y += (blep((t - 32768) & 0xffff, dt, rdt) * 26870) >> 17;
            acc[i] += MIX(y);
        }
        break;
    case MO_SAW:                                /* falling */
        for (i = 0; i < n; i++, ph += inc) {
            t = (int32_t)(ph >> 16);
            y = (32767 - t) + (blep(t, dt, rdt) >> 1);
            acc[i] += MIX(y);
        }
        break;
    default:                                    /* square, wide 1/3, narrow 0.23; minus the DC */
        pw = w == MO_SQUARE ? 32768 : w == MO_WIDE ? 21845 : 15073;
        for (i = 0; i < n; i++, ph += inc) {
            t = (int32_t)(ph >> 16);
            y = (t < pw ? 32767 : -32768) + (blep(t, dt, rdt) >> 1) - (blep((t - pw) & 0xffff, dt, rdt) >> 1)
                + (32768 - pw);
            acc[i] += MIX(y);
        }
        break;
    }
    *php = ph;
}

static uint32_t mul16(uint32_t a, uint32_t r) { return (uint32_t)(((uint64_t)a * r) >> 16); }

/* osc range shift: LO = -3 octaves .. 2' = +2 */
static uint32_t ranged(uint32_t inc, int r)
{
    r = clampi(r, 0, MO_RANGES - 1) - 3;
    inc = r < 0 ? inc >> -r : inc << r;
    return inc > 0x60000000u ? 0x60000000u : inc;   /* below 33 kHz */
}

void mo_render(mo_voice *v, int16_t *out, int n, const uint32_t *inc, const mo_params *p)
{
    int32_t acc[128];
    int32_t target, eff;
    uint32_t inc1, base;
    int i;

    if (n > 128)
        n = 128;
    if (!v->sounding) {
        for (i = 0; i < n; i++)
            out[i] = 0;
        return;
    }
    /* pitch: the block's mean increment, the glide offset going linearly to 0 */
    base = (inc[0] >> 1) + (inc[n - 1] >> 1);
    target = mo_log2(base);
    if (!p->glide) {
        v->glide = 0;
    } else if (v->glide > 0) {
        v->glide = v->glide > v->gstep ? v->glide - v->gstep : 0;
    } else if (v->glide < 0) {
        v->glide = -v->glide > v->gstep ? v->glide + v->gstep : 0;
    }
    eff = target + v->glide;
    v->cur = eff;
    inc1 = mo_exp2(eff);

    for (i = 0; i < n; i++)
        acc[i] = 0;
    osc_add(p->wave[0], 0, &v->ph[0], ranged(inc1, p->range[0]), mo_level[p->vol[0] & 15], acc, n);
    osc_add(p->wave[1], 0, &v->ph[1], ranged(mul16(inc1, mo_freq[clampi(p->freq[0], 0, MO_FREQS - 1)]), p->range[1]),
            mo_level[p->vol[1] & 15], acc, n);
    osc_add(p->wave[2], 1, &v->ph[2], ranged(mul16(inc1, mo_freq[clampi(p->freq[1], 0, MO_FREQS - 1)]), p->range[2]),
            mo_level[p->vol[2] & 15], acc, n);
    if (p->vol[3]) {                            /* white noise: rms 0.071 at full */
        uint32_t r = v->lfsr;
        int32_t lv = (mo_level[p->vol[3] & 15] * 5017) >> 13, pk = v->pink;

        if (p->pink) {                          /* pink: a one-pole low-pass at 2 kHz, +5 dB */
            for (i = 0; i < n; i++) {
                r ^= r << 13;
                r ^= r >> 17;
                r ^= r << 5;
                pk += (((int32_t)(r >> 17) - 16384 - pk) * 4348) >> 15;
                acc[i] += (pk * lv * 3) >> 16;  /* x 1.5: the reference level below 2 kHz */
            }
            v->pink = pk;
        } else {
            for (i = 0; i < n; i++) {
                r ^= r << 13;
                r ^= r >> 17;
                r ^= r << 5;
                acc[i] += (((int32_t)(r >> 17) - 16384) * lv) >> 15;
            }
        }
        v->lfsr = r;
    }
    /* keyboard control from the pitch (glide included, as a classic keyboard CV), MIDI key Q16 */
    mo_filter(v, acc, out, n, (69 << 16) + (eff - mo_log2(INC69)) * 12, p);
}

/* tanh, Q14 in and out (1.0 = 16384): table over [-4, 4), clamped, interpolated */
static inline __attribute__((always_inline)) int32_t tnh(int32_t x)
{
    int32_t u, i;

    x = x < -65536 ? -65536 : x;
    x = x > 65535 ? 65535 : x;
    u = x + 65536;
    i = u >> 8;
    return mo_tanh[i] + (((mo_tanh[i + 1] - mo_tanh[i]) * (u & 255)) >> 8);
}

/* cutoff in Hz Q4: 41 Hz * 2^(10 c) * keyboard control (koct: its octaves, Q16) + contour Hz,
 * capped; -> ladder table position, semitones above 20 Hz, Q8 */
static int cutoff_pos(const mo_params *p, int32_t koct, int32_t fe)
{
    int32_t oct = p->cutoff * 20 + 67619 + koct;    /* 10 octaves Q16; log2(41 / 20) */
    int32_t hz, a2, s;

    oct = oct < 0 ? 0 : oct > (11 << 16) ? 11 << 16 : oct;
    hz = (int32_t)mo_exp2(oct + 545947);        /* 20 Hz Q4 = 320 = 2^8.322 */
    a2 = (p->contour * p->contour) >> 15;
    hz += (((a2 * fe) >> 15) * 2475) >> 10;     /* 4950 Hz Q4 at amount 1, contour 1 */
    hz = hz > CUT_HZ16 ? CUT_HZ16 : hz < 320 ? 320 : hz;
    s = ((mo_log2((uint32_t)hz) - 545947) * 12) >> 8;  /* semitones above 20 Hz, Q8 */
    return s < 0 ? 0 : s > 132 * 256 - 1 ? 132 * 256 - 1 : s;
}

void mo_filter(mo_voice *v, const int32_t *acc, int16_t *out, int n, int32_t key, const mo_params *p)
{
    int i, s;
    int res = (p->emph * 4501) >> 15;           /* Q12: self-oscillation from 0.91 */
    uint32_t fa = mo_rate[p->fatt & 31], fd = mo_rate[p->fdec & 31];
    uint32_t la = mo_rate[p->latt & 31], ld = mo_rate[p->ldec & 31];
    int32_t fs = ((p->fsus & 15) * 32767) / 15, ls = ((p->lsus & 15) * 32767) / 15;
    int32_t s0 = v->lad[0], s1 = v->lad[1], s2 = v->lad[2], s3 = v->lad[3], d4 = v->lad[4], d5 = v->lad[5];
    int32_t lg = v->lgain;
    int32_t koct = ((key - (41 << 16)) / 36) * p->kbd;     /* keyboard control: 1/3 octave per step */

    for (s = 0; s < n; s += SUB) {
        int e = s + SUB < n ? s + SUB : n, c, ci, g, rq;
        int32_t lg1, dl;

        env_run(&v->fenv, fa, fd, fs, p->release ? fd : MO_SHORT_RATE);
        env_run(&v->lenv, la, ld, ls, p->release ? ld : MO_SHORT_RATE);
        c = cutoff_pos(p, koct, v->fenv.e);
        ci = c >> 8;
        g = (mo_ladder[ci] + (((mo_ladder[ci + 1] - mo_ladder[ci]) * (c & 255)) >> 8)) >> 1;   /* Q14 */
        rq = (4 * res * mo_acr[ci]) >> 12;      /* Q12: k, 1 = the threshold of self-oscillation */
        lg1 = (v->lenv.e * (OUT_GAIN >> 4)) >> 15;  /* Q11 */
        dl = e - s == SUB ? (lg1 - lg) >> 5 : (lg1 - lg) / (e - s);
        for (i = s; i < e; i++) {               /* four linear stages, the feedback limited */
            int32_t x = (acc[i] * 3) >> 4, o;

            x -= (rq * tnh(d5)) >> 12;          /* stage 4, two samples back, limited */
            x = x > 65535 ? 65535 : x < -65535 ? -65535 : x;
            s0 += (g * (x - s0) + 8192) >> 14;
            s1 += (g * (s0 - s1) + 8192) >> 14;
            s2 += (g * (s1 - s2) + 8192) >> 14;
            s3 += (g * (s2 - s3) + 8192) >> 14;
            d5 = d4;
            d4 = s3;
            lg += dl;
            o = s3 > 65535 ? 65535 : s3 < -65535 ? -65535 : s3;
            o = (o * lg) >> 11;
            if (o > KNEE)                       /* x / 8000 -> tanh (Q14) -> x 8000 */
                o = KNEE + ((tnh(((o - KNEE) * 2097) >> 10) * 1000) >> 11);
            else if (o < -KNEE)
                o = -KNEE - ((tnh(((-KNEE - o) * 2097) >> 10) * 1000) >> 11);
            out[i] = (int16_t)(o > 32767 ? 32767 : o < -32768 ? -32768 : o);
        }
        lg = lg1;
    }
    v->lad[0] = s0;
    v->lad[1] = s1;
    v->lad[2] = s2;
    v->lad[3] = s3;
    v->lad[4] = d4;
    v->lad[5] = d5;
    v->lgain = lg;
}
