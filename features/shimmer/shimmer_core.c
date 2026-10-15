/* SHIMMER effect core, see shimmer_core.h and README.md. */
#include "shimmer_core.h"

#define LMASK       (SH_LINE_LEN - 1)
#define PMASK       (SH_PS_LEN - 1)
#define WIN_Q16     ((uint32_t)SH_PS_WIN << 16)
#define HALF_Q16    (WIN_Q16 >> 1)
#define DC_R        32604                       /* DC blocker pole (0.995, ~35 Hz) */
#define KNEE        20000                       /* tank soft knee */
#define AP_G        20480                       /* diffuser allpass gain 0.625 (Q15) */
#define AP1_D       241
#define AP2_D       593
#define MOD_DEPTH   (14u << 16)                 /* line-length swing at full MOD, 16.16 samples */
#define SLEW_Q16    (2u << 16)                  /* SIZE: most a line length moves per block */
#define PS_COMP     37683                       /* shifter power loss (crossfade of two grains,
                                                 * ~-1.2 dB) made up: 1.15 (Q15) */
#define G_MAX       32700                       /* line gain cap (Q15) */

/* longest line lengths (full SIZE), mutually prime; SIZE scales them down to 1/4 */
static const uint16_t line_max[SH_LINES] = { 2203, 2591, 3011, 3469 };

/* shift ratio per interval (16.16) and the shifter's input lowpass (Q15 one-pole coefficient,
 * 1 - exp(-2 pi fc / 44100)): keeps the shifted top end below Nyquist, so it never folds back */
static const uint32_t ratio_q16[SH_INTERVALS] = { 131072, 98179, 196358, 262144, 32768 };
static const int16_t semis[SH_INTERVALS] = { 12, 7, 19, 24, -12 };
static const int16_t ps_lp[SH_INTERVALS] = { 18832, 20684, 15510, 12865, 22286 };  /* 6k 7k 4.5k 3.5k 8k */

/* sin(i/16 * pi/2), i = 0..16, Q15 */
static const uint16_t sin_tab[17] = {
    0, 3212, 6393, 9512, 12540, 15447, 18205, 20788, 23170,
    25330, 27246, 28899, 30274, 31357, 32138, 32610, 32767,
};

/* 2^(-i/16), i = 0..16, Q15 */
static const uint16_t exp2_tab[17] = {
    32768, 31379, 30048, 28774, 27554, 26386, 25268, 24196, 23170,
    22188, 21247, 20347, 19484, 18658, 17867, 17109, 16384,
};

static int32_t knob(const int16_t *p, int i)
{
    return p[i] < 0 ? 0 : p[i];
}

static int16_t sat16(int32_t x)
{
    return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

/* 2^(-x), x in 16.16 (>= 0), result Q15 */
static int32_t exp2_neg(uint32_t x)
{
    uint32_t n = x >> 16, f = (x & 0xffff) >> 12, r = x & 0xfff;
    int32_t a, b;

    if (n > 15)
        return 0;
    a = exp2_tab[f];
    b = exp2_tab[f + 1];
    return (a - (((a - b) * (int32_t)r) >> 12)) >> n;
}

/* sin(u * pi/2), u Q15 (0..32767), result Q15 */
static int32_t qsin(int32_t u)
{
    int32_t i = u >> 11, f = u & 0x7ff, a = sin_tab[i], b = sin_tab[i + 1 > 16 ? 16 : i + 1];

    return a + (((b - a) * f) >> 11);
}

static int32_t knee(int32_t v)
{
    if (v > KNEE)
        return KNEE + ((v - KNEE) >> 2);
    if (v < -KNEE)
        return -KNEE - ((-KNEE - v) >> 2);
    return v;
}

int sh_interval(int16_t k)
{
    int32_t v = ((int32_t)(k < 0 ? 0 : k) * SH_INTERVALS) >> 15;

    return v >= SH_INTERVALS ? SH_INTERVALS - 1 : v;
}

int sh_interval_semis(int i)
{
    return semis[i < 0 ? 0 : i >= SH_INTERVALS ? SH_INTERVALS - 1 : i];
}

void sh_reset(sh_state *s)
{
    int i, j;

    for (i = 0; i < SH_LINES; i++) {
        for (j = 0; j < SH_LINE_LEN; j++)
            s->line[i][j] = 0;
        s->lp[i] = 0;
    }
    for (j = 0; j < SH_PS_LEN; j++)
        s->ps[j] = 0;
    for (j = 0; j < SH_AP1_LEN; j++)
        s->ap1[j] = 0;
    for (j = 0; j < SH_AP2_LEN; j++)
        s->ap2[j] = 0;
    s->wpos = 0;
    s->ps_ph = 0;
    s->lfo = 0;
    for (i = 0; i < SH_LINES; i++)
        s->dcur[i] = 0;
    s->pl1 = s->pl2 = s->phx = s->phy = 0;
    s->hp_x = s->hp_y = 0;
    s->level = 0;
    s->sized = 0;
}

/* interpolated read d (16.16) samples behind position w */
static int32_t tap(const int16_t *ln, uint32_t mask, uint32_t w, uint32_t d)
{
    uint32_t di = d >> 16;
    int32_t fr = (int32_t)((d & 0xffff) >> 1), a = ln[(w - di) & mask], b = ln[(w - di - 1) & mask];

    return a + (((b - a) * fr) >> 15);
}

/* the tank + shifter on m <= 64 samples: in[] mono input, wl/wr wet out per side */
static void core(sh_state *s, const int16_t *in, int32_t *wl, int32_t *wr, int m, const int16_t *p)
{
    int32_t g[SH_LINES], damp, xc, xs, lpk, hpa, scale_q15, lvl = s->level;
    uint32_t d0[SH_LINES], dd[SH_LINES], w = s->wpos, rate, step, x;
    int iv = sh_interval((int16_t)knob(p, SH_INTERVAL)), i, k;
    int32_t hx = s->hp_x, hy = s->hp_y, pl1 = s->pl1, pl2 = s->pl2, phx = s->phx, phy = s->phy;
    int32_t lp0 = s->lp[0], lp1 = s->lp[1], lp2 = s->lp[2], lp3 = s->lp[3];
    uint32_t ph = s->ps_ph;

    /* SIZE: target lengths 1/4 .. 1 of the longest; current lengths slew toward them */
    scale_q15 = 8192 + ((knob(p, SH_SIZE) * 24575) >> 15);
    for (k = 0; k < SH_LINES; k++) {
        uint32_t t = ((uint32_t)line_max[k] * (uint32_t)scale_q15) << 1;     /* 16.16 */

        if (!s->sized)
            s->delay_q16[k] = t;
        else if (s->delay_q16[k] < t)
            s->delay_q16[k] = t - s->delay_q16[k] > SLEW_Q16 ? s->delay_q16[k] + SLEW_Q16 : t;
        else
            s->delay_q16[k] = s->delay_q16[k] - t > SLEW_Q16 ? s->delay_q16[k] - SLEW_Q16 : t;
    }
    s->sized = 1;

    /* MOD: triangle LFO ~0.4 Hz, the four lines a quarter period apart. Each line's length moves
     * linearly over the block from where the last block ended (no steps at block edges) to its
     * slewed SIZE length + modulation */
    {
        uint32_t depth = (uint32_t)(((uint64_t)MOD_DEPTH * (uint32_t)knob(p, SH_MOD)) >> 15);

        s->lfo += 1250000u;                     /* 2^32 * 0.4 Hz / 689 blocks/s */
        for (k = 0; k < SH_LINES; k++) {
            uint32_t a = s->lfo + (uint32_t)k * 0x40000000u, t = (a < 0x80000000u ? a : ~a) >> 15;
            uint32_t end = s->delay_q16[k] + (uint32_t)(((uint64_t)depth * t) >> 16);

            d0[k] = s->dcur[k] ? s->dcur[k] : end;
            dd[k] = (uint32_t)((int32_t)(end - d0[k]) / m);
            s->dcur[k] = d0[k] + dd[k] * (uint32_t)m;
        }
    }

    /* DECAY: RT60 = 0.3 s * 2^(6.06 u) (0.3 .. 20 s); line gain g = 2^(-len * 9.966 / (RT * fs)) */
    {
        uint32_t u = (uint32_t)knob(p, SH_DECAY);
        int32_t f = exp2_neg((u * 12412u) >> 10);   /* 2^(-6.06 u), Q15 (6.06 u in 16.16 = 12.12 knob) */

        for (k = 0; k < SH_LINES; k++) {
            /* 9.966 / (0.3 * 44100) = 7.533e-4 per sample = 12638 in Q24 */
            x = (uint32_t)(((uint64_t)(s->delay_q16[k] >> 16) * 12638u * (uint32_t)f) >> 23);
            g[k] = exp2_neg(x);
            if (g[k] > G_MAX)
                g[k] = G_MAX;
        }
    }
    damp = 3000 + ((knob(p, SH_TONE) * 29767) >> 15);      /* lowpass in the tank: ~650 Hz .. open */
    /* SHIMMER: power-preserving crossfade on line 0, cos / sin of SHIMMER * 70 degrees (more
     * would only shift the tail away faster than it rings) */
    xs = (knob(p, SH_SHIMMER) * 25486) >> 15;   /* 70 / 90 */
    xc = qsin(32767 - xs);
    xs = qsin(xs);
    xs = (xs * PS_COMP) >> 15;
    lpk = ps_lp[iv];
    {
        /* LOW CUT: one-pole high-pass, pole 1 - 2 pi fc / fs, fc = 40 + 960 u^2 Hz */
        int32_t u = knob(p, SH_LOWCUT), fc = 40 + ((((u * u) >> 15) * 960) >> 15);

        hpa = 32768 - ((fc * 4669) >> 10);       /* 2 pi / 44100 * 32768 = 4.669 */
    }
    rate = ratio_q16[iv];
    step = rate >= 65536 ? WIN_Q16 - (rate - 65536) : 65536 - rate;   /* (1 - ratio) mod WIN */

    for (i = 0; i < m; i++, w++) {
        int32_t xin = in[i], v, y0, y1, y2, y3, sum, f0, a, b, wa, sh;
        uint32_t da, db;

        hy = xin - hx + (hy * DC_R) / 32768;
        hx = xin;
        v = sat16(hy) >> 1;                     /* 6 dB headroom in the tank */

        /* two diffusers */
        a = s->ap1[(w - AP1_D) & (SH_AP1_LEN - 1)];
        v -= (a * AP_G) >> 15;
        s->ap1[w & (SH_AP1_LEN - 1)] = sat16(v);
        v = a + ((v * AP_G) >> 15);
        a = s->ap2[(w - AP2_D) & (SH_AP2_LEN - 1)];
        v -= (a * AP_G) >> 15;
        s->ap2[w & (SH_AP2_LEN - 1)] = sat16(v);
        v = a + ((v * AP_G) >> 15);

        /* tank: read, damp */
        y0 = tap(s->line[0], LMASK, w, d0[0] + dd[0] * (uint32_t)i);
        y1 = tap(s->line[1], LMASK, w, d0[1] + dd[1] * (uint32_t)i);
        y2 = tap(s->line[2], LMASK, w, d0[2] + dd[2] * (uint32_t)i);
        y3 = tap(s->line[3], LMASK, w, d0[3] + dd[3] * (uint32_t)i);
        lp0 += ((y0 - lp0) * damp) >> 15;
        lp1 += ((y1 - lp1) * damp) >> 15;
        lp2 += ((y2 - lp2) * damp) >> 15;
        lp3 += ((y3 - lp3) * damp) >> 15;
        sum = (lp0 + lp1 + lp2 + lp3) >> 1;     /* twice the mean: Householder = y - sum */

        /* the shimmer: line 0's feedback (after the mix) goes through the shifter (lowpass x2
         * against fold-back, low cut first) */
        f0 = lp0 - sum;
        pl1 += ((f0 - pl1) * lpk) >> 15;
        pl2 += ((pl1 - pl2) * lpk) >> 15;
        phy = pl2 - phx + ((phy * hpa) >> 15);
        phx = pl2;
        s->ps[w & PMASK] = sat16(phy);
        da = ph + (2u << 16);                   /* two taps half a window apart, triangle */
        db = (ph >= HALF_Q16 ? ph - HALF_Q16 : ph + HALF_Q16) + (2u << 16);   /* windows (sum 1) */
        wa = (int32_t)((ph < HALF_Q16 ? ph : WIN_Q16 - ph) >> 11);   /* distance to a jump, Q15 */
        a = tap(s->ps, PMASK, w, da);
        b = tap(s->ps, PMASK, w, db);
        sh = b + (((a - b) * wa) >> 15);
        ph += step;
        if (ph >= WIN_Q16)
            ph -= WIN_Q16;

        /* line 0 gets a power-preserving crossfade of its feedback and the shifted copy
         * (SHIMMER): the two are at different pitches (uncorrelated), so the power that goes
         * round stays about the same and the tank's line gains (< 1) still set the decay; the
         * shifted energy climbs out through the lowpasses (down: through the low cut) */
        f0 = (f0 * xc + sh * xs) >> 15;
        s->line[0][w & LMASK] = sat16(knee(v + ((f0 * g[0]) >> 15)));
        s->line[1][w & LMASK] = sat16(knee(-v + (((lp1 - sum) * g[1]) >> 15)));
        s->line[2][w & LMASK] = sat16(knee(v + (((lp2 - sum) * g[2]) >> 15)));
        s->line[3][w & LMASK] = sat16(knee(-v + (((lp3 - sum) * g[3]) >> 15)));
        wl[i] = lp0 - lp3;
        wr[i] = lp1 - lp2;
        a = (wl[i] + wr[i]) >> 1;
        a = a < 0 ? -a : a;
        if (a > lvl)
            lvl = a;
    }
    lvl -= lvl >> 5;                            /* per block: ~0.3 s to fall 20 dB */
    s->level = lvl;
    s->wpos = w;
    s->hp_x = hx;
    s->hp_y = hy;
    s->pl1 = pl1;
    s->pl2 = pl2;
    s->phx = phx;
    s->phy = phy;
    s->lp[0] = lp0;
    s->lp[1] = lp1;
    s->lp[2] = lp2;
    s->lp[3] = lp3;
    s->ps_ph = ph;
}

void sh_process(sh_state *s, int16_t *buf, int n, const int16_t *p)
{
    int32_t mix = knob(p, SH_MIX), wl[64], wr[64];
    int i, m;

    if (!mix)
        return;                                 /* dry: untouched, bit-exact */
    while (n > 0) {
        m = n < 64 ? n : 64;
        core(s, buf, wl, wr, m, p);
        for (i = 0; i < m; i++) {
            int32_t x = buf[i], wet = sat16(wl[i] + wr[i]);

            buf[i] = sat16(x + (((wet - x) * mix) >> 15));
        }
        buf += m;
        n -= m;
    }
}

void sh_process_stereo(sh_state *s, int16_t *l, int16_t *r, int n, const int16_t *p)
{
    int32_t mix = knob(p, SH_MIX), wl[64], wr[64];
    int16_t mid[64];
    int i, m;

    if (!mix)
        return;
    while (n > 0) {
        m = n < 64 ? n : 64;
        for (i = 0; i < m; i++)
            mid[i] = (int16_t)((l[i] + r[i]) >> 1);
        core(s, mid, wl, wr, m, p);
        for (i = 0; i < m; i++) {
            int32_t a = l[i], b = r[i], el = sat16(wl[i] * 2), er = sat16(wr[i] * 2);

            l[i] = sat16(a + (((el - a) * mix) >> 15));
            r[i] = sat16(b + (((er - b) * mix) >> 15));
        }
        l += m;
        r += m;
        n -= m;
    }
}
