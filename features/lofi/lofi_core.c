/* LOFI synth voice, see lofi_core.h and README.md. */
#include "lofi_core.h"

#define ARP_FRAMES  12                          /* frames per arpeggio step (~17 ms, NES-like) */
#define BYTE_SHIFT  11                          /* bytebeat t rate = inc >> 11 per sample = 32 x the
                                                 * pitch (any sample rate): C4 -> ~8.4 kHz, so
                                                 * "t & 255" plays the key 3 octaves down */
#define BYTE_SMOOTH 11380                       /* one-pole on the 8-bit steps, ~6 kHz at 88.2 kHz */
#define TABLE_SCALE ((2 * LF_LEVEL + 7) / 15)   /* 4-bit table value * 256 -> +-LF_LEVEL, >> 8 */
#define SWEEP_MIN   1024                        /* sweep stops at 1/64 of the pitch */

/* 32-sample 4-bit wavetables (Game Boy style) */
static const uint8_t tables[LF_TABLES][32] = {
    { 8, 9, 10, 12, 13, 14, 14, 15, 15, 15, 14, 14, 13, 12, 10, 9,
      8, 6, 5, 3, 2, 1, 1, 0, 0, 0, 1, 1, 2, 3, 5, 6 },
    { 8, 10, 13, 14, 15, 15, 14, 12, 11, 10, 10, 10, 10, 10, 9, 8,
      8, 7, 6, 5, 5, 5, 5, 5, 4, 3, 1, 0, 0, 1, 2, 5 },
    { 3, 5, 6, 8, 11, 14, 15, 14, 12, 10, 9, 10, 11, 12, 12, 9,
      3, 1, 0, 1, 3, 5, 6, 5, 3, 1, 0, 1, 3, 5, 6, 5 },
    { 2, 1, 0, 0, 1, 3, 5, 7, 8, 8, 7, 5, 4, 3, 4, 5,
      7, 10, 11, 12, 11, 10, 8, 7, 7, 8, 10, 12, 14, 15, 15, 14 },
};

/* arpeggio patterns in semitones (LOOP knob, chip waves); count 0 = off */
static const struct { uint8_t count, semi[4]; } arps[8] = {
    { 0, { 0 } }, { 2, { 0, 12 } }, { 2, { 0, 7 } }, { 3, { 0, 4, 7 } },
    { 3, { 0, 3, 7 } }, { 3, { 0, 7, 12 } }, { 3, { 0, 12, 24 } }, { 4, { 0, 5, 7, 12 } },
};

/* 2^(s/12) in 16.16, s = 0..24 */
static const uint32_t semi_q16[25] = {
    65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715,
    131072, 138866, 147123, 155872, 165140, 174960, 185364, 196386, 208064, 220436, 233544,
    247431, 262144,
};

/* classic bytebeat formulas (community one-liners), 8-bit output */
static uint32_t byte_formula(int f, uint32_t t)
{
    switch (f) {
    case 0:  return t * (t >> 5 | t >> 8);
    case 1:  return t * ((t >> 12 | t >> 8) & 63 & t >> 4);
    case 2:  return (t * 5 & t >> 7) | (t * 3 & t >> 10);
    case 3:  return t * (42 & t >> 10);
    case 4:  return (t >> 6 | t | t >> (t >> 16 & 15)) * 10 + ((t >> 11) & 7);
    case 5:  return t * (t >> 11 & t >> 8 & 123 & t >> 3);
    case 6:  return (t * 9 & t >> 4) | (t * 5 & t >> 7) | (t * 3 & t >> 10);
    case 7:  return ((t * (t >> 8 | t >> 9)) & 46 & t >> 8) ^ ((t & t >> 13) | t >> 6);
    case 8:  return (t >> 7 | t | t >> 6) * 10 + 4 * ((t & t >> 13) | t >> 6);
    case 9:  return (t * (0xCA98u >> (t >> 9 & 14) & 15)) | t >> 8;
    case 10: return t & t >> 8;
    case 11: return t * ((t >> 9 | t >> 13) & 25 & t >> 6);
    case 12: return ((t | t >> 9 | t >> 7) * t) & (t >> 11 | t >> 9);
    case 13: return (t * 5 & t >> 7) | (t * 3 & (t * 4 >> 10));
    case 14: return (t >> 5) | (t << 4) | ((t & 1023) ^ 1981) | ((t - 67) >> 4);
    default: return t * ((t ^ (t + ((t >> 15) | 1))) ^ ((t - 1280) ^ t) >> 10);
    }
}

static int16_t sat16(int32_t x)
{
    return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

static int32_t knob(const int16_t *p, int i)
{
    int32_t v = p[i];

    return v < 0 ? 0 : v;
}

int lf_wave(int16_t k)
{
    int32_t w = ((int32_t)(k < 0 ? 0 : k) * LF_WAVES) >> 15;

    return w >= LF_WAVES ? LF_WAVES - 1 : w;
}

void lf_note_on(lf_voice *v)
{
    v->phase = 0;
    v->t = 0;
    v->tfrac = 0;
    v->nacc = 0;
    if (!v->lfsr)
        v->lfsr = 1;
    v->count = 0;
    v->arp = 0;
    v->frame = 0;
    v->fpos = 0;
    v->smooth = 0;
    v->sweep_q16 = 65536;
}

/* phase distortion set-up: the first half of the cycle is squeezed into [0, knee) */
typedef struct { uint32_t knee, s1, s2; } bend;

static uint32_t bend_phase(const bend *b, uint32_t p)
{
    if (p < b->knee)
        return ((p >> 17) * b->s1) << 5;
    return 0x80000000u + ((((p - b->knee) >> 17) * b->s2) << 5);
}

/* inc * f (16.16), at most half the sample rate */
static uint32_t scaled(uint32_t inc, uint32_t f)
{
    uint64_t e = ((uint64_t)inc * f) >> 16;

    return e > 0x7fffffffu ? 0x7fffffffu : (uint32_t)e;
}

/* one frame (n <= LF_FRAME samples); f = arpeggio x sweep pitch factor, 16.16. Branch-free
 * passes: phases (bent or not), the wave, then only the processing that is switched on. */
static void frame(lf_voice *v, int16_t *out, int n, const uint32_t *incs, uint32_t f, const int16_t *p)
{
    uint32_t q[LF_FRAME], e[LF_FRAME];
    const uint32_t *inc = incs;
    int w = lf_wave((int16_t)knob(p, LF_WAVE)), i;
    int32_t shape = knob(p, LF_SHAPE), x;
    uint32_t ph = v->phase;

    if (f != 65536) {                           /* arpeggio / sweep */
        for (i = 0; i < n; i++)
            e[i] = scaled(incs[i], f);
        inc = e;
    }

    if (w >= LF_W_BYTE) {
        uint32_t loop = (uint32_t)knob(p, LF_LOOP), bnd = (uint32_t)knob(p, LF_BEND);
        uint32_t mask = loop ? (1u << (17 - ((loop * 12) >> 15))) - 1 : 0xffffffffu;
        uint32_t sh = bnd ? 1 + ((bnd * 11) >> 15) : 32, var = (uint32_t)shape >> 5, t;
        uint32_t tt = v->t, frac = v->tfrac;
        int32_t sm = v->smooth, fm = w - LF_W_BYTE;

        for (i = 0; i < n; i++) {
            frac += inc[i] >> BYTE_SHIFT;
            tt += frac >> 16;
            frac &= 0xffff;
            t = tt & mask;                      /* LOOP: repeat a window of 2^17 .. 2^5 steps */
            if (sh < 32)
                t ^= t >> sh;                   /* BEND */
            t ^= var;                           /* VAR */
            x = ((int32_t)(byte_formula(fm, t) & 255) - 128) * (LF_LEVEL / 128);
            sm += ((x - sm) * BYTE_SMOOTH) >> 15;
            out[i] = (int16_t)sm;
        }
        v->t = tt;
        v->tfrac = frac;
        v->smooth = sm;
    } else if (w == LF_W_NOISE || w == LF_W_METAL) {
        /* NOISE: 8 x pitch (x2..x32 with SHAPE), long taps; METAL: 93 x pitch with the 93-step
         * short taps, so its tone is the played note. Clock rate set per frame (noise_inc). */
        uint32_t tap = w == LF_W_METAL ? 6 : 1, acc = v->nacc, c;
        uint16_t r = v->lfsr;

        for (i = 0; i < n; i++) {
            out[i] = (int16_t)(r & 1 ? LF_LEVEL : -LF_LEVEL);
            acc += v->noise_inc;
            for (c = acc >> 16; c; c--)
                r = (uint16_t)((r >> 1) | (((r ^ (r >> tap)) & 1) << 14));
            acc &= 0xffff;
        }
        v->nacc = acc;
        v->lfsr = r;
    } else {
        /* phase pass (BEND: phase distortion, the first half squeezed into [0, knee)) */
        if (knob(p, LF_BEND)) {
            uint32_t k = 0x80000000u - (uint32_t)(((uint64_t)knob(p, LF_BEND) * 0x7ae147aeu) >> 15);
            bend b;

            b.knee = k;
            b.s1 = (uint32_t)(((uint64_t)1 << 43) / k);                    /* 0.5 / knee, Q12 */
            b.s2 = (uint32_t)(((uint64_t)1 << 43) / (0xffffffffu - k + 1)); /* 0.5 / (1 - knee) */
            for (i = 0; i < n; i++) {
                q[i] = bend_phase(&b, ph);
                ph += inc[i];
            }
        } else {
            for (i = 0; i < n; i++) {
                q[i] = ph;
                ph += inc[i];
            }
        }
        v->phase = ph;

        if (w == LF_W_PULSE) {
            uint32_t duty = 0x80000000u - (uint32_t)(((uint64_t)shape * 0x78000000u) >> 15);

            for (i = 0; i < n; i++)             /* SHAPE: 50 % .. ~3 % */
                out[i] = (int16_t)(q[i] < duty ? LF_LEVEL : -LF_LEVEL);
        } else if (w == LF_W_SAW && !shape) {
            for (i = 0; i < n; i++)
                out[i] = (int16_t)((int32_t)(q[i] >> 16) * (2 * LF_LEVEL / 256) / 256 - LF_LEVEL);
        } else if (w <= LF_W_SAW) {
            /* steps: TRI 16 levels (NES) .. 2, SAW 64 .. 4 */
            uint32_t sb = w == LF_W_TRI ? 4 - (uint32_t)((shape * 4) >> 15)
                                        : 6 - (uint32_t)((shape * 5) >> 15);
            int32_t scale;

            if (sb < 1)
                sb = 1;
            scale = (int32_t)(2u * LF_LEVEL / ((1u << sb) - 1));
            if (w == LF_W_TRI)
                for (i = 0; i < n; i++)
                    out[i] = (int16_t)((int32_t)((q[i] < 0x80000000u ? q[i] : ~q[i]) >> (31 - sb))
                                       * scale - LF_LEVEL);
            else
                for (i = 0; i < n; i++)
                    out[i] = (int16_t)((int32_t)(q[i] >> (32 - sb)) * scale - LF_LEVEL);
        } else {
            /* wavetable, SHAPE crossfades into the next table */
            const uint8_t *ta = tables[w - LF_W_TABLE], *tc = tables[(w - LF_W_TABLE + 1) % LF_TABLES];
            int32_t m = shape >> 7, a, c;

            for (i = 0; i < n; i++) {
                a = ta[q[i] >> 27];
                c = tc[q[i] >> 27];
                out[i] = (int16_t)((((a * (256 - m) + c * m) * TABLE_SCALE) >> 8) - LF_LEVEL);
            }
        }
    }

    if (knob(p, LF_CRUSH)) {                    /* sample-and-hold, 1..128 */
        uint32_t crush = 1 + (uint32_t)((knob(p, LF_CRUSH) * knob(p, LF_CRUSH)) >> 23);
        uint32_t cnt = v->count;
        int16_t h = v->hold;

        for (i = 0; i < n; i++) {
            if (!cnt) {
                h = out[i];
                cnt = crush;
            }
            cnt--;
            out[i] = h;
        }
        v->count = (uint8_t)cnt;
        v->hold = h;
    }
    if (knob(p, LF_BITS)) {                     /* 16 .. 1 bits, rounded (no DC offset) */
        uint32_t s = (uint32_t)((knob(p, LF_BITS) * 15) >> 15);
        int32_t half = 1 << (s - 1 + (s == 0)), m = ~((1 << s) - 1);

        if (s)
            for (i = 0; i < n; i++)
                out[i] = sat16((out[i] + half) & m);
    }
    if (knob(p, LF_TONE)) {                     /* one-pole lowpass */
        int32_t c = 32767 - (((knob(p, LF_TONE) * knob(p, LF_TONE)) >> 15) * 30 >> 5), y = v->tone;

        for (i = 0; i < n; i++) {
            y += ((out[i] - y) * c) >> 15;
            out[i] = (int16_t)y;
        }
        v->tone = y;
    } else {
        v->tone = n ? out[n - 1] : v->tone;
    }
}

void lf_render(lf_voice *v, int16_t *out, int n, const uint32_t *inc, const int16_t *p)
{
    int w = lf_wave((int16_t)knob(p, LF_WAVE)), a = (int)((knob(p, LF_LOOP) * 8) >> 15), chunk;
    uint32_t sweep = (uint32_t)knob(p, LF_SWEEP);

    while (n > 0) {
        uint32_t f = 65536;

        chunk = LF_FRAME - v->fpos;
        if (chunk > n)
            chunk = n;
        if (w < LF_W_BYTE && arps[a].count)     /* chip arpeggio (LOOP is the byte window) */
            f = semi_q16[arps[a].semi[v->arp % arps[a].count]];
        if (sweep)
            f = (uint32_t)(((uint64_t)f * v->sweep_q16) >> 16);
        if (w == LF_W_NOISE || w == LF_W_METAL) {
            /* noise clock per frame from the frame's first increment */
            uint64_t c = ((uint64_t)scaled(inc[0], f)
                          * (w == LF_W_METAL ? 93u : 8u << ((knob(p, LF_SHAPE) * 3) >> 15))) >> 16;

            v->noise_inc = (uint32_t)(c > (16u << 16) ? 16u << 16 : c);    /* <= 16 clocks/sample */
        }
        frame(v, out, chunk, inc, f, p);
        out += chunk;
        inc += chunk;
        n -= chunk;
        v->fpos = (uint16_t)(v->fpos + chunk);
        if (v->fpos >= LF_FRAME) {
            v->fpos = 0;
            if (++v->frame % ARP_FRAMES == 0)
                v->arp++;
            if (sweep && v->sweep_q16 > SWEEP_MIN)     /* per frame (1.45 ms): up to -1.6 % */
                v->sweep_q16 -= (uint32_t)(((uint64_t)v->sweep_q16 * ((sweep * sweep) >> 20)) >> 16);
        }
    }
}
