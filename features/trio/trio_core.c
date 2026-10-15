/* TRIO voice (see trio_core.h, README.md). Own code; the idea (three chip oscillators, ring and
 * sync, AND-combined waves, gritty SVF) from sloop's TRIO engine, ideas only.
 *
 * Naive waves at 88.2 kHz (no band limiting: the chip character; TE's decimation removes what
 * lies above 22 kHz). Filter: trapezoidal state-variable (Simper; stable at any cutoff, one pass per
 * sample), g from a 64-step table, coefficients set four times per block (cutoff knob + filter
 * envelope); its states are clamped, so high resonance turns gritty instead of blowing up. */
#include "trio_core.h"
#include "tr_tables.h"

enum { W_OFF = 0, W_TRI, W_SAW, W_PLS, W_NOI, W_ST, W_PS };
#define RING        1
#define SYNC        2
#define SUB         32
#define CLAMP       60000

static const struct {
    uint8_t w[3], flags;
} sets[TR_SETS] = {
    { { W_SAW, W_SAW, W_SAW }, 0 },     /* SAW3 */
    { { W_PLS, W_PLS, W_PLS }, 0 },     /* PULSE3 */
    { { W_TRI, W_TRI, W_TRI }, 0 },     /* TRI3 */
    { { W_SAW, W_PLS, W_OFF }, 0 },     /* SAW+PLS */
    { { W_TRI, W_SAW, W_OFF }, 0 },     /* TRI+SAW */
    { { W_PLS, W_TRI, W_OFF }, 0 },     /* PLS+TRI */
    { { W_ST, W_OFF, W_OFF }, 0 },      /* SAW&TRI */
    { { W_PS, W_OFF, W_OFF }, 0 },      /* PLS&SAW */
    { { W_PLS, W_NOI, W_OFF }, 0 },     /* PLS+NOISE */
    { { W_NOI, W_OFF, W_OFF }, 0 },     /* NOISE */
    { { W_OFF, W_SAW, W_OFF }, SYNC },  /* SYNC SAW: osc 1 silent master */
    { { W_OFF, W_PLS, W_OFF }, SYNC },  /* SYNC PLS */
    { { W_OFF, W_SAW, W_PLS }, SYNC },  /* SYNC3 */
    { { W_TRI, W_OFF, W_OFF }, RING },  /* RING: osc 3 silent modulator */
    { { W_TRI, W_SAW, W_OFF }, RING },  /* RING+SAW */
    { { W_TRI, W_SAW, W_OFF }, RING | SYNC },   /* RING+SYNC */
};

const char *const tr_set_names[TR_SETS] = {
    "SAW3", "PULSE3", "TRI3", "SAW+PLS", "TRI+SAW", "PLS+TRI", "SAW&TRI", "PLS&SAW",
    "PLS+NOISE", "NOISE", "SYNC SAW", "SYNC PLS", "SYNC3", "RING", "RING+SAW", "RING+SYNC",
};
const char *const tr_mode_names[TR_MODES] = { "LP", "BP", "HP", "NOTCH" };

static int zone(int16_t knob, int n)
{
    int k = knob < 0 ? 0 : knob, z = k * n >> 15;

    return z >= n ? n - 1 : z;
}

int tr_set_of(int16_t knob) { return zone(knob, TR_SETS); }
int tr_mode_of(int16_t knob) { return zone(knob, TR_MODES); }
int tr_semis_of(int16_t knob) { return zone(knob, 49) - 24; }

void tr_note_on(tr_voice *v)
{
    v->ph[0] = v->ph[1] = v->ph[2] = 0;
    v->nz[0] = v->nz[1] = v->nz[2] = 0;
    v->lp = v->bp = 0;
    v->env = 32767;
    if (!v->lfsr)
        v->lfsr = 0x1d872b41u;
}

static inline __attribute__((always_inline)) uint32_t rnd(uint32_t *s)
{
    uint32_t x = *s;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

/* one oscillator value, -32768 .. 32767 */
static inline __attribute__((always_inline)) int osc(int w, uint32_t ph, uint32_t prev, unsigned int pw,
                                                     int16_t *nz, uint32_t *s)
{
    unsigned int u = ph >> 16, tri = u < 32768 ? u * 2 : (65535 - u) * 2;

    switch (w) {
    case W_TRI: return (int)tri - 32768;
    case W_SAW: return (int)u - 32768;
    case W_PLS: return u < pw ? 32767 : -32768;
    case W_NOI:                                 /* a new value 16 times a cycle */
        if ((ph >> 28) != (prev >> 28))
            *nz = (int16_t)(rnd(s) >> 16);
        return *nz;
    case W_ST: return (int)(u & tri) - 32768;
    case W_PS: return (int)(u < pw ? u : 0) - 32768;
    default: return 0;
    }
}

static int knob(const int16_t *p, int i) { return p[i] < 0 ? 0 : p[i]; }

void tr_render(tr_voice *v, int16_t *out, int n, const uint32_t *inc, const int16_t *p)
{
    int set = tr_set_of(p[TR_WAVE]), mode = tr_mode_of(p[TR_MODE]), i, s, k, sounding = 0;
    int det = (knob(p, TR_DETUNE) * 1895) >> 15, drive = knob(p, TR_DRIVE), noise = knob(p, TR_NOISE);
    int g;
    int d = knob(p, TR_FDECAY), di = d >> 10, df = d & 1023;
    int fenv = knob(p, TR_FENV), cut = knob(p, TR_CUTOFF), env0 = v->env, env1;
    unsigned int pw = 32768 - ((unsigned int)(knob(p, TR_PW) * 30720) >> 15);
    uint32_t inc1, inc2, ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2], lfsr = v->lfsr;
    int32_t ic1 = v->bp, ic2 = v->lp;                /* SVF integrator states */
    int k15 = 65536 - ((knob(p, TR_RES) * 63800) >> 15);   /* damping 2 .. 0.05, Q15 */
    const uint8_t *w = sets[set].w;           /* pass 1's loops repeat this table */

    /* osc 2 / 3: interval x detune, from this block's base pitch */
    inc1 = (uint32_t)(((uint64_t)inc[0] * tr_ratio[tr_semis_of(p[TR_INT2]) + 24] >> 16) * (65536 + det) >> 16);
    inc2 = (uint32_t)(((uint64_t)inc[0] * tr_ratio[tr_semis_of(p[TR_INT3]) + 24] >> 16) * (65536 - det) >> 16);
    for (k = 0; k < 3; k++)
        sounding += w[k] != W_OFF;
    g = sounding == 1 ? 20000 : sounding == 2 ? 12000 : 8500;     /* headroom for resonance */

    v->env = (env0 * (tr_decay[di] + (((tr_decay[di + 1] - tr_decay[di]) * df) >> 10))) >> 15;
    env1 = v->env;

    /* pass 1: the oscillators into out (one loop per set: the wave types are constants) */
    switch (set) {
#define LOOP(N, W0, W1, W2, FL) case N: \
        for (i = 0; i < n; i++) { \
            uint32_t o0 = ph0, o1 = ph1, o2 = ph2; \
            int32_t a, b, c3; \
            ph0 += inc[i]; \
            if (((FL) & SYNC) && ph0 < o0) \
                ph1 = o1 = 0;                   /* osc 1 starts a cycle: osc 2 restarts */ \
            ph1 += inc1; \
            ph2 += inc2; \
            a = osc(W0, ph0, o0, pw, &v->nz[0], &lfsr); \
            if (((FL) & RING) && (ph2 & 0x80000000u)) \
                a = -a; \
            b = osc(W1, ph1, o1, pw, &v->nz[1], &lfsr); \
            c3 = osc(W2, ph2, o2, pw, &v->nz[2], &lfsr); \
            out[i] = (int16_t)(((a + b + c3) * g) >> 15); \
        } \
        break;
    LOOP(0, W_SAW, W_SAW, W_SAW, 0)     LOOP(1, W_PLS, W_PLS, W_PLS, 0)
    LOOP(2, W_TRI, W_TRI, W_TRI, 0)     LOOP(3, W_SAW, W_PLS, W_OFF, 0)
    LOOP(4, W_TRI, W_SAW, W_OFF, 0)     LOOP(5, W_PLS, W_TRI, W_OFF, 0)
    LOOP(6, W_ST, W_OFF, W_OFF, 0)      LOOP(7, W_PS, W_OFF, W_OFF, 0)
    LOOP(8, W_PLS, W_NOI, W_OFF, 0)     LOOP(9, W_NOI, W_OFF, W_OFF, 0)
    LOOP(10, W_OFF, W_SAW, W_OFF, SYNC) LOOP(11, W_OFF, W_PLS, W_OFF, SYNC)
    LOOP(12, W_OFF, W_SAW, W_PLS, SYNC) LOOP(13, W_TRI, W_OFF, W_OFF, RING)
    LOOP(14, W_TRI, W_SAW, W_OFF, RING) LOOP(15, W_TRI, W_SAW, W_OFF, RING | SYNC)
#undef LOOP
    default:
        break;
    }

    /* pass 2: noise, then drive, each only when on */
    if (noise) {
        uint32_t r = lfsr;

        for (i = 0; i < n; i++) {
            int32_t x;

            r ^= r << 13;
            r ^= r >> 17;
            r ^= r << 5;
            x = out[i] + ((((int32_t)(r >> 17) - 16384) * noise) >> 15);
            out[i] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
        }
        lfsr = r;
    }
    if (drive) {                                /* gain 1..8 into 1.5 u - 0.5 u^3 */
        int gain = drive >> 3;

        for (i = 0; i < n; i++) {
            int32_t u = out[i] + ((out[i] * gain) >> 9);

            u = u > 32767 ? 32767 : u < -32767 ? -32767 : u;
            out[i] = (int16_t)(((3 * u) >> 1) - ((((u * u) >> 15) * u) >> 16));
        }
    }

    /* pass 3: the filter, coefficients four times per block (cutoff + envelope), one loop per mode */
    for (s = 0; s < n; s += SUB) {
        int e = s + SUB < n ? s + SUB : n, env = env0 + (((env1 - env0) * (s + SUB / 2)) >> 7);
        int c = cut + ((((fenv * env) >> 15) * 21700) >> 15), ci, gg, a1, a2, a3;  /* up to 6 octaves */

        c = c > 32767 ? 32767 : c;
        ci = c >> 9;
        gg = tr_cut[ci] + (((tr_cut[ci + 1] - tr_cut[ci]) * (c & 511)) >> 9);
        a1 = (int)((32768u << 15) / (uint32_t)(32768 + ((gg * (gg + k15)) >> 15)));    /* 1/(1+g(g+k)) */
        a2 = (gg * a1) >> 15;
        a3 = (gg * a2) >> 15;
        switch (mode) {
#define SVF(M, Y) case M: \
            for (i = s; i < e; i++) {           /* trapezoidal SVF (Simper) */ \
                int32_t x = out[i], v3 = x - ic2, v1 = ((a1 * ic1) >> 15) + ((a2 * v3) >> 15), y; \
                int32_t v2 = ic2 + ((a2 * ic1) >> 15) + ((a3 * v3) >> 15); \
                ic1 = 2 * v1 - ic1; \
                ic2 = 2 * v2 - ic2; \
                ic1 = ic1 > CLAMP ? CLAMP : ic1 < -CLAMP ? -CLAMP : ic1;   /* loud resonance: grit */ \
                ic2 = ic2 > CLAMP ? CLAMP : ic2 < -CLAMP ? -CLAMP : ic2; \
                y = (Y); \
                out[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y); \
            } \
            break;
        SVF(0, v2)
        SVF(1, v1)
        SVF(2, x - ((k15 * v1) >> 15) - v2)
        SVF(3, x - ((k15 * v1) >> 15))
#undef SVF
        default:
            break;
        }
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->ph[2] = ph2;
    v->lfsr = lfsr;
    v->lp = ic2;
    v->bp = ic1;
}
