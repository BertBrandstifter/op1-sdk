/* ACID voice (see ac_core.h, README.md): Open303's TB-303 model in fixed point + the MO mods.
 * Open303 Copyright (c) 2009 Robin Schmidt, MIT (NOTICE): structure, filter model and constants;
 * this file is our own port.
 *
 * Formats: signals Q20 (1.0 = 2^20), coefficients Q31 (1.31 fractional multiplies: the Blackfin's
 * own, 6 instructions; 64-bit only in the host build), envelopes Q30, the amp Q26 (up to 5.45 with
 * accent), cutoff in octaves above 20 Hz Q16. */
#include "ac_core.h"
#include "ac_tables.h"

#define CTL         8                           /* output samples per control update */
#define LOG69       1595990                     /* log2(INC69), Q16 */
#define ONE20       (1 << 20)
#define ONE30       (1 << 30)
#define INC69       21426141u                   /* 440 Hz at 88.2 kHz */
#define OCT314      260375                      /* log2(314 / 20), Q16: CUTOFF 0 */
#define OCTSPAN     192018                      /* log2(2394 / 314), Q16: CUTOFF 0 .. 255 */
#define SQ_DUTY     2013929472u                 /* the 303 square's high part: 0.4689 of a cycle */
#define KNEE        20000                       /* output: linear below, above it tanh up to KNEE + 8000 */
#define LIM         (1 << 24)                   /* filter state guard (+-16): k y4 stays in 32 bits */
#ifndef AC_BOOST_OCT                            /* ECO + PLAY's EH_BOOST: the filter at 88.2 kHz while */
#define AC_BOOST_OCT 539277                     /* the cutoff is above 6 kHz (log2(6000 / 20), Q16) */
#endif
#define AC_BOOST_HYS 10923                      /* back to 44.1 kHz 2 semitones below */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
/* (a b) >> 31: a 1.31 fractional multiply */
#ifdef __bfin__
#define mulfr(a, b) __builtin_bfin_mult_fr1x32x32((a), (b))
#else
static int32_t mulfr(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 31); }
#endif

int32_t ac_log2(uint32_t x)
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
    return (e << 16) + (int32_t)(ac_tlog2[idx] + (((ac_tlog2[idx + 1] - ac_tlog2[idx]) * fr) >> 8));
}

uint32_t ac_exp2(int32_t l)
{
    int e = l >> 16, idx = (l >> 8) & 255, fr = l & 255;
    uint32_t m = ac_texp2[idx] + (((ac_texp2[idx + 1] - ac_texp2[idx]) * fr) >> 8);

    if (e < 0)
        return 0;
    return e >= 16 ? m << (e - 16) : m >> (16 - e);
}

int ac_decay_ms(int c) { return ac_dec_ms[clampi(c, 0, 255)]; }
int ac_accdec_ms(int s) { return ac_taccdec_ms[clampi(s, 0, 31)]; }
int ac_slide_ms(int s) { return ac_tslide_ms[clampi(s, 0, 31)]; }
int ac_soft_10us(int s) { return ac_tsoft_10us[clampi(s, 0, 31)]; }

static void reset_filter(ac_voice *v)
{
    int i;

    for (i = 0; i < 4; i++)
        v->y[i] = 0;
    v->hx = v->hy = v->px = v->py = v->qx = v->qy = 0;
}

void ac_init(ac_voice *v)
{
    v->ph = v->sph = 0;
    v->cur = 0;
    reset_filter(v);
    v->fy = 0;
    v->meg = v->rc1 = v->rc2 = v->amp = 0;
    v->dc1 = v->dc2 = v->gain = 0;
    v->key = 48 << 16;
    v->on = v->gate = v->accent = v->snap = v->fast = 0;
}

void ac_note(ac_voice *v, int slide, int accent)
{
    int was_gated = v->on && v->gate;

    v->accent = (uint8_t)(accent != 0);
    v->gate = 1;
    if (!v->on)
        slide = 0;
    if (!slide)                                 /* the pitch jumps there */
        v->snap = 1;
    if (!slide || !was_gated) {                 /* triggerNote (a slide after a key-up glides, retriggered) */
        if (v->dc2 < (1 << 16)) {               /* silent: phase and filters start clean */
            v->ph = v->sph = 0;
            reset_filter(v);
        }
        v->meg = ONE30;
    }
    v->amp = ONE30;                             /* attack 0; a legato slide reopens the VCA (no MEG retrigger) */
    v->on = 1;
}

void ac_release(ac_voice *v)
{
    v->gate = 0;
}

/* tanh, Q14 in and out (1.0 = 16384), clamped to +-4 */
static inline __attribute__((always_inline)) int32_t tnh(int32_t x)
{
    int32_t u, i;

    x = x < -65536 ? -65536 : x > 65535 ? 65535 : x;
    u = x + 65536;
    i = u >> 8;
    return ac_tanh[i] + (((ac_tanh[i + 1] - ac_tanh[i]) * (u & 255)) >> 8);
}

/* polyBLEP residual at t (phase Q16) for step dt (Q16) with rdt = 2^30 / dt; Q16 (+-1 = +-65536) */
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

static int32_t lerp_t(const uint32_t *t, int s)
{
    int i = s >> 8, f = s & 255;

    return (int32_t)t[i] + ((((int32_t)t[i + 1] - (int32_t)t[i]) >> 8) * f);
}

/* filter coefficients for a cutoff (octaves above 20 Hz, Q16) and resonance r (Q15) at the filter
 * rate fr (0: 44.1 kHz, 1: 88.2 kHz, 2: 176.4 kHz): b0 Q31, k Q25 and 2g Q27 (Open303 TB_303:
 * k = kr r, g = ((kr / 17 - 1) r + 1)(1 + r)). At 44.1 kHz kr comes per resonance (nodes 0.5 .. 1
 * in steps of 1/8, interpolated; below 0.5 the first): one kr cannot place the resonance peak
 * at both r = 0.9 and 1 that close to Nyquist (gen_tables.py) */
static void coeffs(int fr, int32_t oct, int32_t r, int32_t *b0, int32_t *k, int32_t *g2)
{
    int s = (oct * 12) >> 8;
    int32_t kr, t, r31 = r << 16;

    s = s < 0 ? 0 : s > AC_TOP * 256 - 1 ? AC_TOP * 256 - 1 : s;
    *b0 = lerp_t(fr == 2 ? ac_b0 : fr == 1 ? ac_b0_1 : ac_b0_h, s);
    if (fr) {
        kr = lerp_t(fr == 2 ? ac_kr : ac_kr_1, s);
    } else if (r <= 16384) {
        kr = lerp_t(ac_kr_h[0], s);
    } else {
        int j = (r - 16384) >> 12;
        int32_t ka = lerp_t(ac_kr_h[j], s);

        kr = ka + ((lerp_t(ac_kr_h[j + 1], s) - ka) >> 12) * ((r - 16384) & 4095);
    }
    *k = mulfr(kr, r31);
    t = mulfr(mulfr(kr, 126322568) - (1 << 25), r31) + (1 << 25);           /* (kr / 17 - 1) r + 1, Q25 */
    *g2 = mulfr(t, (r + 32768) << 15) << 4;                                  /* x (1 + r): Q24 -> 2g Q27 */
}

/* one sample of the oscillator (+ sub), Q20, at the filter rate; q = the phase (the step at 0) */
static inline __attribute__((always_inline)) int32_t osc(uint32_t q, uint32_t sq, int wave, int32_t dt, int32_t rdt,
                                                         int32_t sdt, int32_t srdt, int32_t sl)
{
    int32_t t = (int32_t)(q >> 16), y;

    if (wave == AC_SAW)                         /* the 303 saw: rising, the step at q = 0 */
        y = (int32_t)(q >> 11) - ONE20 - (blep(t, dt, rdt) << 4);
    else                                        /* the 303 square (tanh-shaped saw): high 46.9 %, +-0.5 */
        y = (q < SQ_DUTY ? ONE20 / 2 : -ONE20 / 2) + (blep(t, dt, rdt) << 3)
            - (blep((int32_t)((q - SQ_DUTY) >> 16), dt, rdt) << 3);
    if (sl) {
        int32_t st = (int32_t)(sq >> 16), s;

        s = (sq < 0x80000000u ? ONE20 : -ONE20) + (blep(st, sdt, srdt) << 4)
            - (blep((st - 32768) & 0xffff, sdt, srdt) << 4);
        y += mulfr(s, sl << 16);
    }
    return -y;                                  /* Open303: the oscillator inverted */
}

/* the measured env-mod mapping (Open303): scaler and offset from cutoff c and env mod e, Q16; the
 * cutoff at CUTOFF (base, octaves above 20 Hz) and the accent's gain */
typedef struct cut_map {
    int32_t scaler, offset, base, accg;
} cut_map;

static inline __attribute__((always_inline)) void map_of(const ac_params *p, cut_map *m)
{
    int32_t c = clampi(p->cutoff, 0, 255), e = clampi(p->envmod, 0, 255);
    int32_t slo = (e * 247332) / 255 + 48297;   /* 3.773996 e + 0.736966 */
    int32_t shi = (e * 274894) / 255 + 56645;   /* 4.194549 e + 0.864345 */

    m->scaler = (slo * (255 - c) + shi * c) / 255;
    m->offset = (c * 3165) / 255 + 19293;       /* 0.048293 c + 0.294391 */
    m->base = OCT314 + (c * OCTSPAN) / 255;
    m->accg = (clampi(p->accent, 0, 63) * 65536) / 63;                  /* accent gain, Q16 */
}

/* cutoff: c x 2^(scaler (rc1 - offset) + accent rc2), octaves Q16 (rises with rc1 and rc2) */
static inline __attribute__((always_inline)) int32_t cut_oct(const cut_map *m, int sweep, int32_t rc1, int32_t rc2)
{
    int32_t oct = m->base + (((m->scaler >> 4) * (((rc1 >> 14) - m->offset) >> 4)) >> 8);
    int32_t at = (m->accg * (rc2 >> 16)) >> 14;

    return oct + (sweep == AC_SWEEP_HIGH ? at + (at >> 1) + (at >> 3) : at);    /* HIGH: x 1.6 */
}

/* polyBLEP step widths for a phase increment q (per filter sample) */
static void blep_dt(uint32_t q, int32_t *dt, int32_t *rdt, int32_t *sdt, int32_t *srdt)
{
    *dt = (int32_t)(q >> 16);
    *dt = *dt < 1 ? 1 : *dt;
    *rdt = (int32_t)((1u << 30) / (uint32_t)*dt);
    *sdt = (int32_t)(q >> 17);
    *sdt = *sdt < 1 ? 1 : *sdt;
    *srdt = (int32_t)((1u << 30) / (uint32_t)*sdt);
}

/* the voice: output at 88.2 kHz (half = 0) or 44.1 kHz (half = 1: PLAY's ECO, the increments are
 * per 44.1 kHz sample), oscillator + filter at os x the output rate (constants in each caller).
 * Half rate: per-control-update coefficients squared (two 88.2 kHz updates in one), the 44.1 kHz
 * filter table and high-passes: the same envelope times, cutoffs and pitch as at 88.2 kHz.
 * boost (half, os = 1; PLAY's EH_BOOST): per control update, oscillator + filter at 88.2 kHz while
 * the cutoff is above AC_BOOST_OCT (v->fast, 2 semitones of hysteresis), the two samples averaged;
 * the 44.1 kHz filter is fitted only up to 8 kHz and capped at 10.2 kHz (gen_tables.py). */
static inline __attribute__((always_inline)) void render(ac_voice *v, int16_t *out, int n, const uint32_t *inc,
                                                         const ac_params *p, const int os, const int half,
                                                         const int boost)
{
    const int fr = half ? os - 1 : os;          /* filter rate: 0 44.1, 1 88.2, 2 176.4 kHz */
    const int32_t hp_pre = fr == 2 ? AC_HP_PRE : fr == 1 ? AC_HP_PRE1 : AC_HP_PREH;
    const int32_t hp_fb = fr == 2 ? AC_HP_FB : fr == 1 ? AC_HP_FB1 : AC_HP_FBH;
    const int32_t hp_post = half ? AC_HP_POSTH : AC_HP_POST;
    int32_t amp_dec = AC_AMP_DEC, declick = AC_DECLICK;
    static const int16_t sub_lv[4] = { 0, 11469, 19661, 27853 };   /* 0.35 0.6 0.85, Q15 */
    int32_t r0, trackk, fmk, drv, lev, sl;
    cut_map cm;
    uint32_t target, csl, cdec, c1, c2, crel;
    int32_t y1 = v->y[0], y2 = v->y[1], y3 = v->y[2], y4 = v->y[3];
    int32_t hx = v->hx, hy = v->hy, px = v->px, py = v->py, qx = v->qx, qy = v->qy, fy = v->fy;
    uint32_t ph = v->ph, sph = v->sph;
    int32_t dt, rdt, sdt, srdt, ktr, dt2 = 0, rdt2 = 0, sdt2 = 0, srdt2 = 0;
    int i, s, wave = p->wave, muff = p->muff, sweep = p->sweep;

    if (n > 128)
        n = 128;
    if (!v->on) {
        for (i = 0; i < n; i++)
            out[i] = 0;
        return;
    }
    map_of(p, &cm);
    r0 = ac_res[clampi(p->res, 0, 255)];
    trackk = (clampi(p->track, 0, 31) * 65536) / 31;
    fmk = (clampi(p->fm, 0, 31) * 24 * 256) / 31;                       /* oct Q16 per Q14, Q8 */
    drv = p->drive ? ac_drive[clampi(p->drive, 0, 31)] : 0;
    lev = (clampi(p->level, 0, 15) * 2900) / 10;                        /* Q12: 10 = -3 dB (accents have room) */
    sl = sub_lv[p->sub & 3];
    csl = ac_slide[clampi(p->slide, 0, 31)];
    cdec = v->accent ? ac_accdec[clampi(p->accdec, 0, 31)] : ac_dec[clampi(p->decay, 0, 255)];
    c1 = ac_soft[clampi(p->soft, 0, 31)];
    c2 = sweep == AC_SWEEP_OFF ? AC_RC_FAST : ac_speed[p->speed > 2 ? 2 : p->speed];
    crel = v->accent ? AC_AMP_REL_ACC : AC_AMP_REL;
    if (half) {                                 /* one control update = two at 88.2 kHz: c^2 */
        csl = (uint32_t)mulfr((int32_t)csl, (int32_t)csl);
        cdec = (uint32_t)mulfr((int32_t)cdec, (int32_t)cdec);
        c1 = (uint32_t)mulfr((int32_t)c1, (int32_t)c1);
        c2 = (uint32_t)mulfr((int32_t)c2, (int32_t)c2);
        crel = (uint32_t)mulfr((int32_t)crel, (int32_t)crel);
        amp_dec = mulfr(amp_dec, amp_dec);
        declick = mulfr(declick, declick);
    }

    target = (inc[0] >> 1) + (inc[n - 1] >> 1);
    if (p->tune)
        target = (uint32_t)(((uint64_t)target * ac_exp2((clampi(p->tune, -12, 12) << 16) / 12 + (16 << 16))) >> 16);
    if (v->snap) {
        v->cur = target;
        v->snap = 0;
    }

    /* the key (tracking, screen) and the polyBLEP steps from the pitch at the block's start */
    v->key = (69 << 16) + (ac_log2((v->cur ? v->cur : target) >> half) - LOG69) * 12;   /* 88.2 kHz units */
    {
        uint32_t q = (v->cur ? v->cur : target) / (uint32_t)os;

        blep_dt(q, &dt, &rdt, &sdt, &srdt);
        if (boost)                              /* the steps at 88.2 kHz too */
            blep_dt(q >> 1, &dt2, &rdt2, &sdt2, &srdt2);
    }
    ktr = ((((v->key - (48 << 16)) / 12) >> 4) * (trackk >> 4)) >> 8;     /* tracking, octaves Q16 */

    for (s = 0; s < n; s += CTL) {
        int e2 = s + CTL < n ? s + CTL : n, m = e2 - s;
        int32_t oct, r, b0, k, g2, amp_in, g0, g1, dg;
        int32_t sdt_, srdt_, dt_ = dt, rdt_ = rdt, hpp = hp_pre, hpf = hp_fb;
        uint32_t q_inc, s_inc;
        int sos = os, sfr = fr;                 /* this update's rate: os, fr (boost: 1 / 2, 0 / 1) */

        sdt_ = sdt;
        srdt_ = srdt;
        /* pitch slew (slide) on the frequency, as Open303 */
        v->cur = target + (uint32_t)mulfr((int32_t)(v->cur - target), (int32_t)csl);

        /* MEG, rc1, rc2 */
        v->meg = mulfr(v->meg, (int32_t)cdec);
        v->rc1 = v->meg + mulfr(v->rc1 - v->meg, (int32_t)c1);
        {
            int32_t in2 = v->accent ? v->meg : 0;

            v->rc2 = in2 + mulfr(v->rc2 - in2, (int32_t)c2);
        }

        oct = cut_oct(&cm, sweep, v->rc1, v->rc2) + ktr;     /* (+ tracking) */
        r = r0;
        if (sweep == AC_SWEEP_HIGH && v->accent)    /* HIGH: the sweep adds resonance (up to 1) */
            r += (((32767 - r0) * (v->rc2 >> 15)) >> 15) * 2 / 5;
        r = r > 32767 ? 32767 : r;
        if (boost) {                            /* ECO: the cutoff high: this update at 88.2 kHz */
            v->fast = (uint8_t)(oct > AC_BOOST_OCT - (v->fast ? AC_BOOST_HYS : 0));
            if (v->fast) {
                sos = 2;
                sfr = 1;
                hpp = AC_HP_PRE1;
                hpf = AC_HP_FB1;
                dt_ = dt2;
                rdt_ = rdt2;
                sdt_ = sdt2;
                srdt_ = srdt2;
            }
        }
        coeffs(sfr, oct, r, &b0, &k, &g2);
        q_inc = v->cur >> (sos - 1);            /* at the filter rate */
        q_inc = q_inc > 0x30000000u ? 0x30000000u : q_inc;
        s_inc = q_inc >> 1;

        /* amp: envelope (decay to 0 / release) + MEG x (0.45 + 4 accent) while the gate is on,
         * through the de-click (2 x one-pole 200 Hz), Q26 */
        v->amp = mulfr(v->amp, v->gate ? amp_dec : (int32_t)crel);
        amp_in = v->amp >> 4;
        if (v->gate)                            /* MEG x (0.45 + 4 accent): Q30 x Q28 -> Q27 */
            amp_in += mulfr(v->meg, (29491 + (v->accent ? cm.accg * 4 : 0)) << 12) >> 1;
        v->dc1 = amp_in + mulfr(v->dc1 - amp_in, declick);
        v->dc2 = v->dc1 + mulfr(v->dc2 - v->dc1, declick);
        g1 = v->dc2;
        g0 = v->gain;
        dg = m == CTL ? (g1 - g0) >> 3 : (g1 - g0) / m;

        for (i = s; i < e2; i++) {
            int32_t acc = 0, y, j;

            if (fmk) {                          /* filter FM: the output moves the cutoff */
                int32_t fo = fy > 16384 ? 16384 : fy < -16384 ? -16384 : fy;

                coeffs(sfr, oct + ((fo * fmk) >> 8), r, &b0, &k, &g2);
            }
            for (j = 0; j < sos; j++) {         /* (oversampled:) oscillator, high-pass, filter */
                int32_t x = osc(ph + 0x80000000u, sph, wave, dt_, rdt_, sdt_, srdt_, sl), f, y0;

                ph += q_inc;
                sph += s_inc;
                py = mulfr(py + x - px, hpp);               /* pre-filter high-pass 44.5 Hz */
                px = x;
                x = py;
                if (drv)                        /* overdrive: into a tanh (the MO's level into the filter) */
                    x = tnh(((x >> 6) * drv) >> 8) << 6;
                f = mulfr(k, y4) << 6;                      /* Q25 x Q20 -> Q14 -> Q20 */
                hy = mulfr(hy + f - hx, hpf);               /* feedback high-pass 150 Hz */
                hx = f;
                y0 = x - hy;
                y1 += mulfr(b0, y0 - y1 + y2) << 1;
                y2 += mulfr(b0, y1 - 2 * y2 + y3);
                y3 += mulfr(b0, y2 - 2 * y3 + y4);
                y4 += mulfr(b0, y3 - 2 * y4);
                y1 = y1 > LIM ? LIM : y1 < -LIM ? -LIM : y1;
                y4 = y4 > LIM ? LIM : y4 < -LIM ? -LIM : y4;
                acc += mulfr(g2, y4) << (4 - (sos >> 1));   /* 2 g y4 (Q27 x Q20 -> Q16), averaged */
            }
            qy = mulfr(qy + acc - qx, hp_post);          /* post-filter high-pass 24 Hz */
            qx = acc;
            g0 += dg;
            y = mulfr(qy, g0) >> 1;                         /* Q20 x Q26 -> Q14 */
            fy = y;
            if (muff == AC_MUFF_SOFT)
                y = (tnh(y) * 5) >> 2;
            else if (muff == AC_MUFF_HARD)
                y = tnh(y * 3);
            y = (y * lev) >> 12;                            /* Q14 1.0 -> 11600 at LEVEL 10 */
            if (y > KNEE)
                y = KNEE + ((tnh(((y - KNEE) * 2097) >> 10) * 1000) >> 11);
            else if (y < -KNEE)
                y = -KNEE - ((tnh(((-KNEE - y) * 2097) >> 10) * 1000) >> 11);
            out[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
        }
        v->gain = g1;
    }
    v->y[0] = y1; v->y[1] = y2; v->y[2] = y3; v->y[3] = y4;
    v->hx = hx; v->hy = hy; v->px = px; v->py = py; v->qx = qx; v->qy = qy; v->fy = fy;
    v->ph = ph; v->sph = sph;
    if (!v->gate && v->amp < (1 << 14) && v->dc1 < (1 << 10) && v->dc2 < (1 << 10) && v->gain < (1 << 10))
        v->on = 0;                              /* released and the amp gone (below -96 dB): silent from
                                                 * now (TE keeps calling a voice until its envelope ends) */
}

static __attribute__((noinline)) void render1(ac_voice *v, int16_t *out, int n, const uint32_t *inc, const ac_params *p)
{
    render(v, out, n, inc, p, 1, 0, 0);
}

static __attribute__((noinline)) void render2(ac_voice *v, int16_t *out, int n, const uint32_t *inc, const ac_params *p)
{
    render(v, out, n, inc, p, 2, 0, 0);
}

static __attribute__((noinline)) void render1h(ac_voice *v, int16_t *out, int n, const uint32_t *inc, const ac_params *p)
{
    render(v, out, n, inc, p, 1, 1, 0);
}

static __attribute__((noinline)) void render2h(ac_voice *v, int16_t *out, int n, const uint32_t *inc, const ac_params *p)
{
    render(v, out, n, inc, p, 2, 1, 0);
}

static __attribute__((noinline)) void render1hb(ac_voice *v, int16_t *out, int n, const uint32_t *inc, const ac_params *p)
{
    render(v, out, n, inc, p, 1, 1, 1);
}

/* ECO + boost: can the cutoff reach AC_BOOST_OCT in this block? An upper bound: rc1 moves towards
 * the MEG (which only decays), rc2 towards the MEG (accent) or 0; the cutoff rises with both. The
 * tracking from the higher of the pitch now and the block's. No: render1h (the work as without). */
static int boost_due(const ac_voice *v, const ac_params *p, const uint32_t *inc, int n)
{
    cut_map m;
    int32_t rc1 = v->rc1 > v->meg ? v->rc1 : v->meg, rc2 = v->rc2, oct;

    if (!v->on)
        return 0;
    if (v->fast)
        return 1;
    if (v->accent && v->meg > rc2)
        rc2 = v->meg;
    map_of(p, &m);
    oct = cut_oct(&m, p->sweep, rc1, rc2);
    if (p->track) {
        uint32_t q = (inc[0] >> 1) + (inc[n - 1] >> 1);
        int32_t key;

        q = (q > v->cur ? q : v->cur) >> 1;     /* (the tune: up to an octave up, allowed for below) */
        key = (69 << 16) + (ac_log2(q ? q : 1) - LOG69) * 12 + (p->tune > 0 ? (12 << 16) : 0);
        oct += ((((key - (48 << 16)) / 12) >> 4) * (((clampi(p->track, 0, 31) * 65536) / 31) >> 4)) >> 8;
    }
    return oct > AC_BOOST_OCT - AC_BOOST_HYS;
}

/* Oscillator + filter at the output rate (the filter is linear; its coefficients are fitted to the
 * 176.4 kHz response, gen_tables.py); with FILTER FM on at twice the output rate (the FM loop's
 * one-sample delay and its sidebands need the rate). p->half: output at 44.1 kHz (PLAY's ECO);
 * p->boost: the filter at 88.2 kHz while the cutoff is high (render1hb, only in blocks whose
 * cutoff can get there: boost_due). */
void ac_render(ac_voice *v, int16_t *out, int n, const uint32_t *inc, const ac_params *p)
{
    if (p->half && !p->fm && p->boost && boost_due(v, p, inc, n > 128 ? 128 : n)) {
        render1hb(v, out, n, inc, p);
    } else if (p->half) {
        v->fast = 0;
        (p->fm ? render2h : render1h)(v, out, n, inc, p);
    } else {
        (p->fm ? render2 : render1)(v, out, n, inc, p);
    }
}
