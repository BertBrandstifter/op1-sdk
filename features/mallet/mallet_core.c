/* MALLET voice core, see mallet_core.h and README.md. */
#include "mallet_core.h"

#define LEVEL       12000                       /* a mode of gain 1.0 rings at this amplitude */
#define INC_MAX     535653518u                  /* highest mode: 11 kHz (88.2 kHz increments) */
#define INC_MIN     8192u                       /* below ~0.2 Hz: treated as that */
#define SMAX        (22000 << 8)                /* resonator state bound */
#define LOG2_C4     1546839                     /* log2 of C4's increment, Q16 */
#define LOG2_RATE   (-400524)                   /* log2(64 / 44100 * log2(1000)): T60 -> octaves per block */

/* an instrument: mode ratios (Q12), excitation gains (Q15), log2 of T60 at C4 (s, Q16), level */
typedef struct {
    const char *name;
    uint8_t n;
    uint16_t ratio[ML_MODES];
    int16_t gain[ML_MODES];
    int32_t lt[ML_MODES];
    int16_t level;
} ml_ins;

/* ratios: marimba / vibraphone bars tuned 1 : 4 : 10, xylophone 1 : 3 : 6, glockenspiel the free
 * bar's 1 : 2.76 : 5.40 : 8.93, pans tuned to octave and twelfth with a doublet (the wobble),
 * kalimba's tine 1 : 6.27, tubular bell modes 4-9 of a free tube (the strike note an octave below
 * mode 6's partner) */
static const ml_ins ins[ML_INSTRS] = {
    { "MARIMBA",  3, { 4096, 16343, 40960 },               { 32767, 9830, 3277 },
      { 17238, -99259, -200468 },                                                           32767 },
    { "VIBES",    3, { 4096, 16384, 40960 },               { 32767, 7209, 1966 },
      { 183983, 86634, -9962 },                                                             30000 },
    { "XYLOPHONE", 3, { 4096, 12288, 24576 },              { 32767, 16384, 8192 },
      { -75498, -152170, -238804 },                                                         28000 },
    { "GLOCKENSP", 4, { 4096, 11305, 22118, 36577 },       { 32767, 11469, 6554, 3277 },
      { 118447, 55574, -9962, -65536 },                                                     26000 },
    { "STEELPAN", 5, { 4096, 4112, 8192, 12288, 16425 },   { 26214, 11469, 19661, 9830, 3932 },
      { 44438, 44438, 9011, -48298, -99259 },                                               24000 },
    { "HANDPAN",  4, { 4096, 4106, 8192, 12288 },          { 29491, 13107, 14746, 8192 },
      { 131072, 131072, 86634, 38336 },                                                     24000 },
    { "KALIMBA",  2, { 4096, 25682 },                      { 32767, 8192 },
      { 55574, -131072 },                                                                   32767 },
    { "TUBULAR",  6, { 2527, 4956, 8192, 12247, 17080, 22241 }, { 8192, 16384, 32767, 24576, 14746, 8192 },
      { 169408, 183983, 169408, 142208, 118447, 86634 },                                    16000 },
};

static const uint16_t exp2_tab[17] = {
    32768, 34219, 35734, 37316, 38968, 40693, 42495, 44376, 46341, 48393, 50535, 52773, 55109,
    57549, 60097, 62757, 65535,
};

int ml_instr(int16_t knob)
{
    int i = ((knob < 0 ? 0 : knob) * ML_INSTRS) >> 15;

    return i >= ML_INSTRS ? ML_INSTRS - 1 : i;
}

const char *ml_instr_name(int i)
{
    return ins[i < 0 || i >= ML_INSTRS ? 0 : i].name;
}

int ml_modes(int i)
{
    return ins[i < 0 || i >= ML_INSTRS ? 0 : i].n;
}

int ml_ratio(int i, int k)
{
    const ml_ins *in = &ins[i < 0 || i >= ML_INSTRS ? 0 : i];

    return k >= 0 && k < in->n ? in->ratio[k] : 0;
}

/* 2^x, x in 16.16 (-31 .. +14), result 16.16 */
static uint32_t exp2q(int32_t x)
{
    int32_t n = x >> 16, f = (x & 0xffff) >> 12, r = x & 0xfff;
    uint32_t a = exp2_tab[f], b = f < 15 ? exp2_tab[f + 1] : 65536, m = a + (((b - a) * (uint32_t)r) >> 12);

    m <<= 1;                                    /* Q15 -> Q16 mantissa (1 .. 2) */
    if (n >= 0)
        return n > 14 ? 0xffffffffu : m << n;
    return n < -31 ? 0 : m >> -n;
}

/* log2 of x (> 0), 16.16; log2(1 + f) ~ f (1.3466 - 0.3466 f), within 0.01 */
static int32_t log2q(uint32_t x)
{
    int32_t n = 31 - __builtin_clz(x), f = (int32_t)(((x << (31 - n)) & 0x7fffffffu) >> 15);

    return (n << 16) + (int32_t)(((int64_t)f * (88250 - ((22714 * f) >> 16))) >> 16);
}

/* resonator coefficient 2 sin(w / 2) at the half rate, Q16, for an 88.2 kHz increment */
static int32_t coef(uint32_t inc)
{
    int64_t x = (int64_t)(((uint64_t)inc * 6746518852ull) >> 32), x2 = (x * x) >> 30;    /* rad, Q30 */
    int64_t x3 = (x * x2) >> 30, x5 = (x3 * x2) >> 30;
    int64_t s = x - ((x3 * 178956971) >> 30) + ((x5 * 8947849) >> 30);  /* x - x^3/6 + x^5/120 */

    return (int32_t)((2 * s) >> 14);
}

/* sine of a 32-bit phase, Q15 (parabola, within 0.1 %: the motor's tremolo) */
static int32_t sine(uint32_t ph)
{
    int32_t x = (int32_t)ph >> 16, a = x < 0 ? -x : x, y = (x * (32768 - a)) >> 13;   /* 4 x (1 - |x|) */
    int32_t ay = y < 0 ? -y : y;

    return y + ((((y * ay) >> 15) - y) * 7203 >> 15);   /* + 0.22 (y |y| - y) */
}

/* soft knee: linear to half scale, cubic up to full scale */
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

void ml_note_on(ml_voice *v)
{
    v->strike = 1;
}

int32_t ml_motor(uint32_t *phase, const int16_t *p, int n)
{
    int32_t u = p[ML_MOTOR] < 0 ? 0 : p[ML_MOTOR], hz;

    if (u < 300)
        return 32768;
    hz = 78643 + (int32_t)(((int64_t)u * 445645) >> 15);              /* 1.2 .. 8 Hz, Q16 */
    *phase += (uint32_t)(((uint64_t)hz * (uint32_t)n * 48695u) >> 16);  /* 2^32 / ML_SR per Hz */
    return 32768 - ((26214 * (32768 - sine(*phase))) >> 16);          /* depth 0.8 */
}

/* per-mode set-up for this pitch and these knobs: coefficient, strike gain, decay per 16 samples */
static void setup(ml_voice *v, const ml_ins *in, uint32_t f, const int16_t *p)
{
    int32_t dt = 32768 + (((p[ML_DETUNE] - 16384) * 1311) >> 14), sm = 4096 + ((p[ML_STRIKE] * 24576) >> 15);
    int32_t tube = p[ML_TUBE] < 0 ? 0 : p[ML_TUBE], lt_off = (p[ML_DECAY] - 16384) * 8;
    int32_t lvl = (LEVEL * in->level) >> 15, k;

    for (k = 0; k < in->n; k++) {
        uint32_t ratio = in->ratio[k];
        uint64_t ik;
        int32_t gk = in->gain[k], lt;
        int64_t x;

        if (k)
            ratio = (ratio * (uint32_t)dt) >> 15;
        ik = ((uint64_t)f * ratio) >> 12;
        if (ik > INC_MAX) {                     /* above 11 kHz: silent */
            v->e[k] = v->g[k] = v->r8[k] = 0;
            v->s1[k] = v->s2[k] = 0;
            continue;
        }
        v->e[k] = coef((uint32_t)ik);
        if (k)
            gk = (gk * sm) >> 14;
        else
            gk = (gk * (32768 + ((tube * 19661) >> 15))) >> 15;         /* TUBE: up to 1.6x */
        v->g[k] = (lvl * gk) >> 15;
        lt = in->lt[k] + lt_off + v->pitch_lt + (k ? 0 : tube * 4);      /* TUBE: up to 4x longer */
        x = ((int64_t)exp2q(LOG2_RATE - lt) * 11629080) >> 18;          /* octaves per 16 samples x ln 2, Q24 */
        x = (1 << 24) - x + ((x * x) >> 25);
        v->r8[k] = x < 0 ? 0 : (int32_t)x;
    }
    v->nm = in->n;
    v->key_f = f;
    v->key_lt = v->pitch_lt;
    for (k = 0; k < ML_PARAMS; k++)
        v->key_p[k] = p[k];
}

void ml_render(ml_voice *v, int16_t *out, int n, const uint32_t *inc, const int16_t *p, int32_t motor)
{
    const ml_ins *in = &ins[ml_instr(p[ML_INSTR])];
    const int32_t *e = v->e, *g = v->g;
    int32_t nm, k, i, h = n >> 1, tc = 2000 + ((p[ML_TONE] * 30767) >> 15), same;
    uint32_t f = inc[0] < INC_MIN ? INC_MIN : inc[0];

    if (v->strike) {                            /* the mallet: pulse length a share of the period */
        int32_t hard = p[ML_MALLET] < 0 ? 0 : p[ML_MALLET], lt;
        uint32_t frac = (39322u * exp2q(-((hard * 125) >> 4))) >> 16;  /* 0.6 .. 0.04, Q16 */
        uint32_t len = (uint32_t)((((uint64_t)frac << 31) / f) >> 8);    /* samples, Q8 */

        len = len < 512 ? 512 : len > 153600 ? 153600 : len;              /* 2 .. 600 samples */
        for (k = 0; k < ML_MODES; k++) {        /* a restruck bar: what rings is damped */
            v->s1[k] >>= 2;
            v->s2[k] >>= 2;
        }
        v->ex_ph = 0;
        v->ex_step = (uint32_t)(((uint64_t)1 << 39) / len);
        v->ex_amp = (int32_t)((98304u * 256u) / len);                     /* 1.5 / length: area 1 */
        v->click = hard > 18000 ? (hard - 18000) * 2 : 0;
        lt = -((log2q(f) - LOG2_C4) >> 1);                                /* higher notes ring shorter */
        v->pitch_lt = lt < -131072 ? -131072 : lt > 131072 ? 131072 : lt;
        v->rnd = 0x2545f491u;
        v->strike = 0;
    }
    same = v->nm && v->key_f == f && v->key_lt == v->pitch_lt;          /* set up again on any change */
    for (k = 0; same && k < ML_PARAMS; k++)
        same = k == ML_MOTOR || k == ML_TONE || k == ML_MALLET || v->key_p[k] == p[k];
    if (!same)
        setup(v, in, f, p);
    nm = v->nm;
    {
        int32_t xb[64], yb[64], ex = v->ex_step && v->ex_ph < 0x80000000u, c;

        for (i = 0; i < h; i++) {               /* the pulse: 4 u (1 - u), area 1 (Q16) */
            int32_t x = 0;

            yb[i] = 0;
            if (ex && v->ex_ph < 0x80000000u) {
                int32_t u = (int32_t)(v->ex_ph >> 16), q = (u * (32768 - u)) >> 13;

                x = (q * v->ex_amp) >> 15;
                if (v->click) {                 /* hard mallets: noise on the pulse */
                    v->rnd = v->rnd * 1664525u + 1013904223u;
                    x += (x * ((((int32_t)(v->rnd >> 16) - 32768) * v->click) >> 15)) >> 14;
                }
                v->ex_ph += v->ex_step;
            }
            xb[i] = x;
        }
        for (k = 0; k < nm; k++) {              /* one resonator at a time, its states in registers */
            int32_t a = v->s1[k], b = v->s2[k], ek = e[k], gk = g[k], m = k ? 32768 : motor;

            if (!ek)
                continue;
            for (c = 0; c < h; c += 16) {
                int32_t end = c + 16 < h ? c + 16 : h;

                if (ex)
                    for (i = c; i < end; i++) {
                        a += (gk * xb[i]) >> 8;
                        a += ((b >> 8) * ek) >> 8;
                        b -= ((a >> 8) * ek) >> 8;
                        yb[i] += ((a >> 8) * m) >> 15;
                    }
                else
                    for (i = c; i < end; i++) {
                        a += ((b >> 8) * ek) >> 8;
                        b -= ((a >> 8) * ek) >> 8;
                        yb[i] += m == 32768 ? a >> 8 : ((a >> 8) * m) >> 15;
                    }
                a = (int32_t)(((int64_t)a * v->r8[k]) >> 24);      /* decay, every 16 samples */
                b = (int32_t)(((int64_t)b * v->r8[k]) >> 24);
                a = a > SMAX ? SMAX : a < -SMAX ? -SMAX : a;         /* states kept where (b >> 8) e fits */
                b = b > SMAX ? SMAX : b < -SMAX ? -SMAX : b;
            }
            v->s1[k] = a;
            v->s2[k] = b;
        }
        for (i = 0; i < h; i++) {
            int32_t y = yb[i] > 40000 ? 40000 : yb[i] < -40000 ? -40000 : yb[i];

            v->lp += ((y - v->lp) * (tc >> 1)) >> 14;
            y = knee(v->lp);
            out[2 * i] = (int16_t)((v->last + y) >> 1);
            out[2 * i + 1] = (int16_t)y;
            v->last = (int16_t)y;
        }
    }
}
