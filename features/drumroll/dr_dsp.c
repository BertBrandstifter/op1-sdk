/* DRUM ROLL voice post-process (dr_dsp.h). The SVF runs on 32-bit states (samples << 12) with Q31
 * coefficients, so low cutoffs stay clean (a Q15 version leaked ~25 % at 20 Hz). */
#include "dr_dsp.h"

#define SH          12                  /* state scale: sample << SH */

#ifdef __bfin__
/* (a * b) >> 31: the 3-MAC sequence TE uses (e.g. 0x01a85ce6); no fix-up for -1 * -1, which the
 * coefficients (0 .. 1) never hit. Half the instructions of __builtin_bfin_mult_fr1x32x32NS. */
static inline __attribute__((always_inline)) int32_t mulf(int32_t a, int32_t b)
{
    int32_t r;

    __asm__("A1 = %1.L * %2.L (FU);\n\t"
            "A1 = A1 >> 16;\n\t"
            "A0 = %1.H * %2.H, A1 += %1.H * %2.L (M);\n\t"
            "A1 += %2.H * %1.L (M);\n\t"
            "A1 = A1 >>> 15;\n\t"
            "%0 = (A0 += A1);"
            : "=d"(r) : "d"(a), "d"(b) : "A0", "A1");
    return r;
}
#define MULF(a, b)  mulf((a), (b))
#define ADDS(a, b)  __builtin_bfin_add_fr1x32((a), (b))            /* saturating */
#define SUBS(a, b)  __builtin_bfin_sub_fr1x32((a), (b))
#else
#define MULF(a, b)  ((int32_t)(((int64_t)(a) * (b)) >> 31))
static int32_t sat32(int64_t v) { return v > 0x7fffffff ? 0x7fffffff : v < -0x7fffffff - 1 ? -0x7fffffff - 1 : (int32_t)v; }
#define ADDS(a, b)  sat32((int64_t)(a) + (b))
#define SUBS(a, b)  sat32((int64_t)(a) - (b))
#endif

const char *const dr_filter_name[DR_FILTERS] = { "OFF", "LP", "BP", "HP" };

/* g = tan(pi f / 88200) Q31, f = 20 Hz * 1000^(c / 127) */
static const uint32_t g_tab[128] = {
    1529823u, 1615338u, 1705632u, 1800974u, 1901645u, 2007944u, 2120184u, 2238699u,
    2363838u, 2495972u, 2635493u, 2782812u, 2938367u, 3102616u, 3276047u, 3459173u,
    3652534u, 3856705u, 4072288u, 4299922u, 4540281u, 4794075u, 5062057u, 5345018u,
    5643796u, 5959275u, 6292390u, 6644125u, 7015522u, 7407680u, 7821760u, 8258986u,
    8720654u, 9208128u, 9722853u, 10266351u, 10840231u, 11446192u, 12086026u, 12761629u,
    13474999u, 14228248u, 15023607u, 15863429u, 16750201u, 17686549u, 18675243u, 19719213u,
    20821549u, 21985515u, 23214558u, 24512320u, 25882644u, 27329589u, 28857442u, 30470732u,
    32174239u, 33973013u, 35872387u, 37877994u, 39995782u, 42232035u, 44593390u, 47086858u,
    49719844u, 52500171u, 55436105u, 58536377u, 61810214u, 65267364u, 68918130u, 72773400u,
    76844681u, 81144139u, 85684635u, 90479768u, 95543919u, 100892299u, 106541001u, 112507053u,
    118808480u, 125464361u, 132494905u, 139921520u, 147766891u, 156055072u, 164811575u, 174063473u,
    183839516u, 194170247u, 205088140u, 216627751u, 228825874u, 241721733u, 255357177u, 269776911u,
    285028746u, 301163889u, 318237267u, 336307887u, 355439263u, 375699889u, 397163789u, 419911152u,
    444029066u, 469612371u, 496764662u, 525599456u, 556241580u, 588828812u, 623513832u, 660466567u,
    699877009u, 741958634u, 786952571u, 835132705u, 886812007u, 942350390u, 1002164586u, 1066740655u,
    1136649966u, 1212569849u, 1295310584u, 1385851102u, 1485386872u, 1595395133u, 1717725249u, 1854726285u,
};

void dr_coef_set(dr_coef *c, int type, int cut, int res)
{
    int64_t g, kh, den, a1;

    cut = cut < 0 ? 0 : cut > 127 ? 127 : cut;
    res = res < 0 ? 0 : res > 127 ? 127 : res;
    g = g_tab[cut];
    kh = 0x7fffffff - (int64_t)res * 2093796557 / 127;      /* damping / 2: 1 .. 0.025 (Q31) */
    den = ((int64_t)1 << 31) + ((g * ((g + 2 * kh) >> 1)) >> 30);    /* 1 + g (g + k) */
    a1 = ((int64_t)1 << 62) / den;
    c->a1 = (int32_t)(a1 > 0x7fffffff ? 0x7fffffff : a1);
    c->a2 = (int32_t)((g * c->a1) >> 31);
    c->a3 = (int32_t)((g * c->a2) >> 31);
    c->kh = (int32_t)kh;
    c->type = (uint8_t)(type > DR_F_OFF && type < DR_FILTERS ? type : DR_F_OFF);
}

void dr_voice_start(dr_voice *v, int key, int32_t gain, int32_t gate)
{
    v->ic1 = v->ic2 = 0;
    v->gain = gain;
    v->gate = gate;
    v->fade = DR_FADE;
    v->key = (uint8_t)key;
}

static int silent(const int16_t *out, int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (out[i])
            return 0;
    return 1;
}

void dr_process(dr_voice *v, int16_t *out, int n, const dr_coef *c)
{
    int i;

    if (c && c->type != DR_F_OFF) {
        int32_t ic1 = v->ic1, ic2 = v->ic2, a1 = c->a1, a2 = c->a2, a3 = c->a3, kh = c->kh;

        if (ic1 > -(8 << SH) && ic1 < (8 << SH) && ic2 > -(8 << SH) && ic2 < (8 << SH) && silent(out, n)) {
            ic1 = ic2 = 0;                      /* idle voice: nothing to filter */
        } else {
            /* trapezoidal SVF (Simper); saturating sums instead of per-sample clamps (states have
             * 16x headroom over full scale) */
            switch (c->type) {
#define SVF(M, Y) case M: \
                for (i = 0; i < n; i++) { \
                    int32_t x = (int32_t)out[i] * (1 << SH), v3 = SUBS(x, ic2), y; \
                    int32_t v1 = ADDS(MULF(a1, ic1), MULF(a2, v3)); \
                    int32_t v2 = ADDS(ic2, ADDS(MULF(a2, ic1), MULF(a3, v3))); \
                    ic1 = SUBS(ADDS(v1, v1), ic1); \
                    ic2 = SUBS(ADDS(v2, v2), ic2); \
                    y = (Y) >> SH; \
                    out[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y); \
                } \
                break;
            SVF(DR_F_LP, v2)
            SVF(DR_F_BP, v1)
            SVF(DR_F_HP, SUBS(SUBS(x, v2), ADDS(MULF(kh, v1), MULF(kh, v1))))
#undef SVF
            default:
                break;
            }
        }
        v->ic1 = ic1;
        v->ic2 = ic2;
    }
    if (v->gain == DR_UNITY && v->gate < 0)
        return;
    {
        int32_t gate = v->gate, fade = v->fade, seg = gate < 0 || gate > n ? n : gate;
        int16_t g = (int16_t)(v->gain >= DR_UNITY ? 32767 : v->gain);
        int unity = v->gain >= DR_UNITY;

        /* open: gain only; then the fade (DR_FADE = 256); then silence */
        if (!unity)
            for (i = 0; i < seg; i++)
                out[i] = (int16_t)(((int32_t)out[i] * g) >> 15);
        i = seg;
        if (gate >= 0)
            gate -= seg;
        if (gate == 0) {
            for (; i < n && fade > 0; i++, fade--)
                out[i] = (int16_t)(((unity ? out[i] : ((int32_t)out[i] * g) >> 15) * fade) >> 8);
            for (; i < n; i++)
                out[i] = 0;
        }
        v->gate = gate;
        v->fade = fade;
    }
}
