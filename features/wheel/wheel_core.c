/* WHEEL voice (see wheel_core.h, README.md). Own code; idea from JIANT FM's WHEEL engine.
 *
 * Additive: every drawbar is one sine. All nine are integer multiples of the 16' bar's phase q
 * (16' 5 1/3' 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1' = q x 1 3 2 4 6 8 10 12 16), so one accumulator
 * per rotor group drives them all and they stay locked. Partials above ~6 kHz fold down an octave
 * (as the top tonewheels do), which also keeps everything far below Nyquist.
 * Rotary: partials below ~800 Hz go to the drum, above to the horn (each partial is one sine, so
 * the crossover is exact); each group gets its rotor's amplitude modulation and doppler shift. */
#include "wheel_core.h"

#define XOVER_INC   38956619u                   /* 800 Hz as a phase increment */
#define FOLD_INC    292174646u                  /* 6 kHz */
#define NYQ_INC     973915486u                  /* 20 kHz: never rendered (voice runs at 44.1 kHz) */

#define PERC_FAST   32374                       /* per-block (128 samples) decay, Q15: tau 0.12 s */
#define PERC_SLOW   32673                       /* tau 0.5 s */
#define CLICK_DECAY 20200                       /* tau 3 ms */
#define ANGLE_INC   761                         /* Q16 Hz -> turns (2^32) per sample, >> 10 */
#define PERC_NORMAL 14400                       /* percussion peak, Q15 of full scale */
#define PERC_SOFT   7200
#define DRAWBAR_CUT 22938                       /* normal percussion drops the bars ~3 dB */

#define HORN_EASE   159                         /* Q16 per block: horn tau ~0.6 s */
#define DRUM_EASE   32                          /* drum tau ~3 s */
#define HORN_AM     14745                       /* horn level swing 0.45 */
#define DRUM_AM     8192                        /* drum 0.25 */
#define HORN_DOP    229                         /* horn pitch swing +-0.35 %, Q16 */
#define DRUM_DOP    98                          /* drum +-0.15 % */

static const uint8_t mult[WH_BARS] = { 1, 3, 2, 4, 6, 8, 10, 12, 16 };

/* drawbar step 0..8 -> Q15 level, 3 dB a step */
static const uint16_t bar_level[9] = { 0, 2920, 4125, 5827, 8231, 11627, 16423, 23198, 32767 };

/* one digit per bar: 16' 5 1/3' 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1' */
static const char regs[WH_REGS][WH_BARS + 1] = {
    "008000000", "006600000", "808000000", "888000000",
    "888800000", "868500000", "008080800", "888808008",
    "888400008", "800000888", "008866444", "878600046",
    "006008008", "888888000", "000888888", "888888888",
};
static const char *const reg_names[WH_REGS] = {
    "FLUTE", "SOFT", "BALLAD", "JAZZ", "BLUES", "SMOOTH", "CLARINET", "GOSPEL",
    "ROCK", "SHRILL", "REED", "THEATRE", "SPARKLE", "BRIGHT", "HIGH", "FULL",
};
static const char *const perc_names[WH_PERCS] = {
    "OFF", "2ND", "3RD", "2ND SOFT", "3RD SOFT", "2ND SLOW", "3RD SLOW",
};
static const char *const rot_names[WH_ROTS] = { "OFF", "SLOW", "FAST" };
static const int32_t horn_hz[WH_ROTS] = { 0, 54395, 442368 };   /* Q16: 0.83 / 6.75 Hz */
static const int32_t drum_hz[WH_ROTS] = { 0, 43909, 386662 };   /* 0.67 / 5.9 Hz */

static int zone(int16_t knob, int n)
{
    int k = knob < 0 ? 0 : knob;

    return k * n >> 15;
}

int wh_reg(int16_t knob) { return zone(knob, WH_REGS); }
int wh_perc(int16_t knob) { return zone(knob, WH_PERCS); }
int wh_rot(int16_t knob) { return zone(knob, WH_ROTS); }
int wh_offset(int16_t knob) { return zone(knob, 17) - 8; }
const char *wh_reg_name(int i) { return reg_names[i & (WH_REGS - 1)]; }
const char *wh_perc_name(int i) { return perc_names[(unsigned int)i < WH_PERCS ? i : 0]; }
const char *wh_rot_name(int i) { return rot_names[(unsigned int)i < WH_ROTS ? i : 0]; }

int wh_bar(const int16_t *p, int b)
{
    int d = regs[wh_reg(p[WH_REG])][b] - '0';

    d += wh_offset(p[b < 2 ? WH_BASS : b < 4 ? WH_BODY : WH_TOP]);
    return d < 0 ? 0 : d > 8 ? 8 : d;
}

/* Blackfin fract16 ops (single cycle), portable equivalents for the host tests */
#ifdef __bfin__
#define MUL16(a, b) __builtin_bfin_mult_fr1x16((a), (b))
#define ABS16(a)    __builtin_bfin_abs_fr1x16((a))
#else
static inline int16_t MUL16(int16_t a, int16_t b)
{
    int32_t p = ((int32_t)a * b) >> 15;

    return (int16_t)(p > 32767 ? 32767 : p);
}
static inline int16_t ABS16(int16_t a) { return (int16_t)(a < 0 ? (a == -32768 ? 32767 : -a) : a); }
#endif

static inline int16_t sat16(int32_t x) { return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); }

/* sine of a 16-bit phase (65536 = one cycle), Q15: parabola + one correction step, max error
 * ~0.1 %. 16-bit maths only. */
static inline int16_t fsin16(int16_t x)
{
    int16_t y = (int16_t)(MUL16(x, (int16_t)(32767 - ABS16(x))) * 4), y2 = MUL16(y, ABS16(y));  /* <= 32764 */

    return (int16_t)(y + MUL16((int16_t)(y2 - y), 7373));
}

/* output stage: exactly linear up to 3/4 of full scale, then a smooth knee that approaches
 * +-32767 (y = T + R d / (d + R)) instead of a hard clip; the divide only runs above the knee */
#define KNEE        24576
static inline int16_t soft(int32_t x)
{
    int32_t ax = x < 0 ? -x : x, d, y;

    if (ax <= KNEE)
        return (int16_t)x;
    d = ax - KNEE;
    y = KNEE + (int32_t)(((uint32_t)(32767 - KNEE) * (uint32_t)d) / (uint32_t)(d + 32767 - KNEE));
    return (int16_t)(x < 0 ? -y : y);
}

static inline int32_t fsin(uint32_t phase) { return fsin16((int16_t)(phase >> 16)); }

void wh_note_on(wh_voice *v)
{
    v->qh = v->qd = 0;
    v->perc = 1 << 30;
    v->click = 1 << 30;                         /* Q30 */
    v->hp = 0;
    v->last = 0;
    if (!v->lfsr)
        v->lfsr = 0x2545f491u;
}

void wh_rotor_block(wh_rotor *r, const int16_t *p, int n)
{
    int rot = wh_rot(p[WH_ROTARY]);

    r->horn_hz += ((horn_hz[rot] - r->horn_hz) * HORN_EASE) >> 16;
    r->drum_hz += ((drum_hz[rot] - r->drum_hz) * DRUM_EASE) >> 16;
    r->horn0 = r->horn1;
    r->drum0 = r->drum1;
    /* Q16 Hz * 2^32 / SR per sample = Q16 Hz * 0.743 (32-bit only: per block, no divides) */
    r->horn1 += (uint32_t)((r->horn_hz * ANGLE_INC) >> 10) * (uint32_t)n;
    r->drum1 += (uint32_t)((r->drum_hz * ANGLE_INC) >> 10) * (uint32_t)n;
}

/* x / n for the per-sample ramps (TE always renders 128 = 64 at half rate) */
static inline int32_t per_sample(int32_t x, int n) { return n == 64 ? x >> 6 : x / n; }

/* rotor gain at an angle: 1 facing the listener, 1 - depth facing away (Q15) */
static int32_t rotor_gain(uint32_t angle, int32_t depth)
{
    return 32767 - ((depth * (32768 - fsin(angle + 0x40000000u))) >> 16);
}

void wh_render(wh_voice *v, const wh_rotor *r, int16_t *out, int n, const uint32_t *inc,
               const int16_t *p)
{
    uint8_t hm[WH_BARS + 1], dm[WH_BARS + 1];   /* multipliers per group (+ percussion) */
    int16_t ha[WH_BARS + 1], da[WH_BARS + 1];   /* amplitudes, Q15 */
    int nh = 0, nd = 0, i, j, b, perc = wh_perc(p[WH_PERC]), rot = wh_rot(p[WH_ROTARY]);
    uint32_t h4 = inc[0] >> 5, qh = v->qh, qd = v->qd, lfsr = v->lfsr;   /* h4: 16' inc >> 4 */
    int32_t sum = 0, lv[WH_BARS], pa = 0, pstep = 0, pend, ca, cstep, cend, hp = v->hp, click;
    int32_t gh = 32767, gd = 32767, dgh = 0, dgd = 0, dh = 0, dd = 0, drive, pm = 0;
    int32_t norm, dqh, dqd, last = v->last, comp;
    int perc_horn = 0, n2 = (n + 1) >> 1;
    const char *reg = regs[wh_reg(p[WH_REG])];
    int off[3] = { wh_offset(p[WH_BASS]), wh_offset(p[WH_BODY]), wh_offset(p[WH_TOP]) };

    /* bars: level, foldback, group. Per-block set-up stays in 32 bits (64-bit maths and divides
     * are library calls on the Blackfin); frequencies are compared as increments >> 4 */
    for (b = 0; b < WH_BARS; b++) {
        int d = reg[b] - '0' + off[b < 2 ? 0 : b < 4 ? 1 : 2];

        lv[b] = perc && b == WH_BARS - 1 ? 0        /* percussion takes over the 1' bar's contact */
                : bar_level[d < 0 ? 0 : d > 8 ? 8 : d];
        sum += lv[b];
    }
    /* gain = WH_LEVEL / (0.75 S + 0.25) for S full bars: one bar peaks at WH_LEVEL, nine at ~1.3x
     * (Q14) */
    norm = (int32_t)(((uint32_t)WH_LEVEL << 16) / (uint32_t)(3 * sum + 32767));
    if (perc == 1 || perc == 2 || perc >= 5)
        norm = norm * DRAWBAR_CUT >> 15;
    for (b = 0; b < WH_BARS; b++) {
        uint32_t m = mult[b], f = m * h4;

        if (!lv[b])
            continue;
        while (f > (FOLD_INC >> 4) && !(m & 1)) {
            m >>= 1;
            f >>= 1;
        }
        if (f >= (NYQ_INC >> 4))
            continue;
        if (!rot || f > (XOVER_INC >> 4)) {      /* no rotary: one group, one loop */
            hm[nh] = (uint8_t)m;
            ha[nh++] = (int16_t)((lv[b] * norm) >> 14);
        } else {
            dm[nd] = (uint8_t)m;
            da[nd++] = (int16_t)((lv[b] * norm) >> 14);
        }
    }

    /* percussion: 4' (2nd) or 2 2/3' (3rd), decaying from note on, ramped across the block */
    if (perc) {
        int32_t level = perc == 3 || perc == 4 ? PERC_SOFT : PERC_NORMAL;

        pm = (perc & 1) ? 4 : 6;                /* 1 3 5: 2nd, 2 4 6: 3rd */
        pa = ((v->perc >> 15) * level) >> 15;
        v->perc = (v->perc >> 15) * (perc >= 5 ? PERC_SLOW : PERC_FAST);
        pend = ((v->perc >> 15) * level) >> 15;
        pstep = per_sample(pend - pa, n2);
        perc_horn = !rot || (uint32_t)pm * h4 > (XOVER_INC >> 4);
    }

    /* key click: high-passed noise, a few ms */
    click = p[WH_CLICK] < 0 ? 0 : p[WH_CLICK];
    ca = ((v->click >> 15) * click) >> 16;      /* up to a quarter of full scale */
    v->click = (v->click >> 15) * CLICK_DECAY;
    cend = ((v->click >> 15) * click) >> 16;
    cstep = per_sample(cend - ca, n2);

    /* rotary: gains ramp across the block, doppler per block from the middle angle */
    if (rot) {
        gh = rotor_gain(r->horn0, HORN_AM);
        gd = rotor_gain(r->drum0, DRUM_AM);
        dgh = per_sample(rotor_gain(r->horn1, HORN_AM) - gh, n2);
        dgd = per_sample(rotor_gain(r->drum1, DRUM_AM) - gd, n2);
        dh = (HORN_DOP * fsin(r->horn0 + ((r->horn1 - r->horn0) >> 1))) >> 15;
        dd = (DRUM_DOP * fsin(r->drum0 + ((r->drum1 - r->drum0) >> 1))) >> 15;
    }

    /* drive gain 1..8 in Q12 (0 = off); its output x 1 / (1 + 4 d), d = 0..1: about the clean
     * loudness at every setting (more drive = more grit, not more volume) */
    drive = p[WH_DRIVE] <= 0 ? 0 : 4096 + (p[WH_DRIVE] * 7 >> 3);
    comp = drive ? (int32_t)((32767u << 15) / (32768u + 4u * (uint32_t)p[WH_DRIVE])) : 32767;

    /* the voice runs at half TE's rate (44.1 kHz: partials stop at ~6 kHz) and is interpolated
     * up; TE's 2x decimation removes the images. Doppler: one offset per block */
    dqh = (int32_t)(inc[0] >> 16) * dh;
    dqd = (int32_t)(inc[0] >> 16) * dd;
    for (i = 0; i < n2; i++) {
        uint32_t h = (inc[2 * i] >> 1) + (inc[2 * i + 1 < n ? 2 * i + 1 : 2 * i] >> 1);
        int16_t q16h, q16d, s16;
        int32_t sh = 0, sd = 0, s;

        qh += h + (uint32_t)dqh;
        qd += h + (uint32_t)dqd;
        q16h = (int16_t)(qh >> 16);
        q16d = (int16_t)(qd >> 16);
        /* partial phase = m x q in 16 bits (the low half's carry is below 0.1 % of a cycle) */
        for (j = 0; j < nh; j++)
            sh += (int32_t)ha[j] * fsin16((int16_t)(hm[j] * q16h));
        for (j = 0; j < nd; j++)
            sd += (int32_t)da[j] * fsin16((int16_t)(dm[j] * q16d));
        if (pm) {
            if (perc_horn)
                sh += (int32_t)(int16_t)pa * fsin16((int16_t)(pm * q16h));
            else
                sd += (int32_t)(int16_t)pa * fsin16((int16_t)(pm * q16d));
            pa += pstep;
        }
        s = (int32_t)MUL16(sat16(sh >> 15), (int16_t)gh) + MUL16(sat16(sd >> 15), (int16_t)gd);
        gh += dgh;
        gd += dgd;
        if (ca > 0) {
            int16_t x;

            lfsr ^= lfsr << 13;
            lfsr ^= lfsr >> 17;
            lfsr ^= lfsr << 5;
            x = (int16_t)((int32_t)lfsr >> 18);        /* +-8192 */
            s += MUL16((int16_t)(x - hp), (int16_t)ca); /* first difference: bright tick */
            hp = x;
            ca += cstep;
        }
        if (drive) {
            /* gain 1..8, then y = 1.5 u - 0.5 u^3 on u clamped to +-1, level compensated (more
             * drive = more grit, not more volume) */
            int16_t u = sat16((s * (drive > 32767 ? 32767 : drive)) >> 12);

            s = (int32_t)MUL16(sat16(u + (u >> 1) - (MUL16(MUL16(u, u), u) >> 1)), (int16_t)comp);
        }
        s16 = soft(s);
        out[2 * i] = (int16_t)((last + s16) >> 1);
        if (2 * i + 1 < n)
            out[2 * i + 1] = s16;
        last = s16;
    }
    v->last = (int16_t)last;
    v->qh = qh;
    v->qd = qd;
    v->lfsr = lfsr;
    v->hp = hp;
}
