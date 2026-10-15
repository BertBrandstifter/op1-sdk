/* WARP voice (see warp_core.h, README.md). Own code; the idea of phase warps on a single-cycle
 * wave from strudel's worklets (AGPL, ideas only).
 *
 * The oscillator phase is a 32-bit accumulator; its top 16 bits go through the warp (phase warps
 * bend where in the cycle the wave is read, output warps reshape the value read), then a 256-point
 * table with linear interpolation. Warp constants are set every 16 samples from the effective
 * amount (AMOUNT + SWEEP x envelope + MOTION x LFO), ramped across the block. */
#include "warp_core.h"
#include "wp_tables.h"

#define SUB         32                          /* samples per warp set-up */
#define LFO_MIN     2                           /* motion LFO 0.2 .. 8 Hz (x 10) */
#define LFO_SPAN    78
#define DETUNE_MAX  786                         /* unison: ~1.2 % = ~20 cents, Q16 */

const char *const wp_wave_names[WP_WAVES] = {
    "SINE", "TRI", "SAW", "SQUARE", "PULSE", "HALF", "ORGAN", "VOX",
};
const char *const wp_mode_names[WP_MODES] = {
    "BEND", "SYNC", "MIRROR", "FOLD", "QUANT", "SQUEEZE", "PINCH", "WRAP",
    "FM", "GAP", "BITREV", "CHAOS", "STAIR", "HARM", "RING", "XOR",
};

static int zone(int16_t knob, int n)
{
    int k = knob < 0 ? 0 : knob, z = k * n >> 15;

    return z >= n ? n - 1 : z;
}

int wp_wave_of(int16_t knob) { return zone(knob, WP_WAVES); }
int wp_mode_of(int16_t knob) { return zone(knob, WP_MODES); }

/* warp set-up for one sub-block */
typedef struct wk {
    int mode, a;                                /* a: amount, Q15 */
    uint32_t k, s1, s2;                         /* BEND knee / slopes, GAP end / slope */
    uint32_t r;                                 /* SYNC / RING ratio Q12, HARM n, QUANT / STAIR mask */
    const int16_t *t;                           /* the wave */
} wk;

static uint8_t rev8(unsigned int x)
{
    x = (x & 0xf0) >> 4 | (x & 0x0f) << 4;
    x = (x & 0xcc) >> 2 | (x & 0x33) << 2;
    return (uint8_t)((x & 0xaa) >> 1 | (x & 0x55) << 1);
}

static void setup(wk *w, int wave, int mode, int a)
{
    w->mode = mode;
    w->a = a;
    w->t = wp_wave[wave];
    switch (mode) {
    case WP_BEND:                               /* knee from the middle to 1 % of the cycle */
        w->k = 32768 - (uint32_t)((a * 32112) >> 15);
        if (w->k == 32768) {                    /* no bend: unit slopes, no divides */
            w->s1 = w->s2 = 32768;
            break;
        }
        w->s1 = (32768u << 15) / w->k;          /* Q15 slopes: first half, second half */
        w->s2 = (32768u << 15) / (65536 - w->k);
        break;
    case WP_GAP:                                /* wave squeezed into the first 100 .. 10 % */
        w->k = 65536 - (uint32_t)((a * 58982) >> 15);
        w->s1 = (65536u << 14) / w->k;
        break;
    case WP_SYNC:                               /* ratio 1 .. 7.5, Q12 (not an integer at the top:
                                                 * an exact multiple is just a harmonic) */
        w->r = 4096 + (uint32_t)((a * 26624) >> 15);
        break;
    case WP_RING: case WP_FOLD:                 /* ratio / gain 1 .. 8, Q12 */
        w->r = 4096 + (uint32_t)((a * 7 * 4096) >> 15);
        break;
    case WP_HARM:
        w->r = 1 + (uint32_t)((a * 8) >> 15);
        if (w->r > 8)
            w->r = 8;
        break;
    case WP_QUANT:                              /* phase steps: 16 .. 2 bits */
        w->r = 0xffffu << (uint32_t)((a * 14) >> 15) & 0xffff;
        break;
    case WP_STAIR:                              /* output levels: 16 .. 3 bits */
        w->r = 0xffffu << (uint32_t)((a * 13) >> 15) & 0xffff;
        break;
    default:
        break;
    }
}

static inline __attribute__((always_inline)) int lerp(int x, int y, int a) { return x + (((y - x) * a) >> 15); }

static inline __attribute__((always_inline)) int lookup(const int16_t *t, unsigned int p)
{
    int i = (int)(p >> 8), f = (int)(p & 0xff);

    return t[i] + (((t[i + 1] - t[i]) * f) >> 8);
}

/* one sample of the warped wave at 16-bit phase p; mode is a constant in the render loops (one
 * specialised loop per mode, the switch folds away) */
static inline __attribute__((always_inline)) int warp_m(const wk *w, unsigned int p, int mode)
{
    int a = w->a, x, y;
    unsigned int q;

    switch (mode) {
    case WP_BEND:
        q = p < w->k ? (p * w->s1) >> 15 : 32768 + (((p - w->k) * w->s2) >> 15);
        return lookup(w->t, q & 0xffff);
    case WP_SYNC:
        return lookup(w->t, (p * w->r >> 12) & 0xffff);
    case WP_MIRROR:
        q = p < 32768 ? 65535 - 2 * p : 2 * p - 65536;
        return lookup(w->t, (unsigned int)lerp((int)p, (int)q, a) & 0xffff);
    case WP_QUANT:
        return lookup(w->t, p & w->r);
    case WP_SQUEEZE: {
        unsigned int s = (p * p) >> 16, s2 = (s * s) >> 16;

        q = (unsigned int)(a < 16384 ? lerp((int)p, (int)s, a * 2) : lerp((int)s, (int)s2, (a - 16384) * 2));
        return lookup(w->t, q & 0xffff);
    }
    case WP_PINCH:
        x = (int)p - 32768;
        y = (((x * x) >> 15) * x) >> 15;
        return lookup(w->t, (unsigned int)(lerp(x, y, a) + 32768) & 0xffff);
    case WP_WRAP:
        return lookup(w->t, (p + (unsigned int)((lookup(wp_wave[0], p) * a) >> 15)) & 0xffff);
    case WP_FM:
        return lookup(w->t, (p + (unsigned int)((lookup(w->t, p) * a) >> 15)) & 0xffff);
    case WP_GAP:
        return p < w->k ? lookup(w->t, ((p * w->s1) >> 14) & 0xffff) : 0;
    case WP_BITREV:
        q = (unsigned int)rev8(p >> 8) << 8 | (p & 0xff);
        return lookup(w->t, (unsigned int)lerp((int)p, (int)q, a) & 0xffff);
    case WP_CHAOS:
        q = (p * (65535 - p)) >> 14;
        q = q > 65535 ? 65535 : q;
        return lookup(w->t, (unsigned int)lerp((int)p, (int)q, a) & 0xffff);
    case WP_HARM:
        return lookup(w->t, (p * w->r) & 0xffff);
    case WP_XOR:
        return lookup(w->t, p ^ ((unsigned int)(a * 19) & 0x3ff0));   /* never all bits (= mirror) */
    case WP_FOLD:                               /* output: gain 1..8, triangle fold at +-32767 */
        y = (lookup(w->t, p) * (int)w->r) >> 12;
        y += 32768;
        y &= 131071;                            /* period 4 x 32768 */
        return y < 65536 ? y - 32768 : 98304 - y;
    case WP_STAIR:
        y = lookup(w->t, p);
        return y >= 0 ? (int)((unsigned int)y & w->r) : -(int)((unsigned int)-y & w->r);
    case WP_RING:
        y = lookup(w->t, p);
        x = (y * ((lookup(wp_wave[0], (p * w->r >> 12) & 0xffff) * 21845) >> 14)) >> 15;
        return lerp(y, x, a);
    default:
        return lookup(w->t, p);
    }
}

void wp_note_on(wp_voice *v)
{
    v->phase = v->phase2 = 0;
    v->env = 32767;
    v->lp = v->lp2 = 0;
    v->a_prev = -1;                             /* first block starts at its own amount */
}

/* effective amount for this block: AMOUNT + SWEEP x env + MOTION x LFO, clamped */
static int amount(const wp_voice *v, const int16_t *p)
{
    int a = p[WP_AMOUNT] < 0 ? 0 : p[WP_AMOUNT], sweep = p[WP_SWEEP] < 0 ? 0 : p[WP_SWEEP];
    int m = p[WP_MOTION] < 0 ? 0 : p[WP_MOTION];

    a += (sweep * v->env) >> 15;
    if (m) {
        int tri = (int)(v->lfo >> 16), depth = (m * 13107) >> 15;   /* up to 0.4 of the range */

        tri = (tri < 32768 ? tri : 65535 - tri) * 2 - 32768;        /* -32768 .. 32766 */
        a += (tri * depth) >> 15;
    }
    return a < 0 ? 0 : a > 32767 ? 32767 : a;
}

void wp_render(wp_voice *v, int16_t *out, int n, const uint32_t *inc, const int16_t *p)
{
    int wave = wp_wave_of(p[WP_WAVE]), mode = wp_mode_of(p[WP_MODE]), s, i;
    int a0 = v->a_prev, a1, color = p[WP_COLOR] < 0 ? 0 : p[WP_COLOR];
    int uni = p[WP_UNISON] < 0 ? 0 : p[WP_UNISON], m = p[WP_MOTION] < 0 ? 0 : p[WP_MOTION];
    int g = 32767 - ((color * 31780) >> 15), det = (uni * DETUNE_MAX) >> 15;
    int d = p[WP_DECAY] < 0 ? 0 : p[WP_DECAY], di = d >> 10, df = d & 1023;
    uint32_t ph = v->phase, ph2 = v->phase2;
    int32_t lp = v->lp;
    wk w;

    /* per block: envelope, LFO, the amount ramps from the last block's value to this one's */
    v->env = (v->env * (wp_decay[di] + (((wp_decay[di + 1] - wp_decay[di]) * df) >> 10))) >> 15;
    if (m)
        v->lfo += (uint32_t)(LFO_MIN + ((m * LFO_SPAN) >> 15)) * (uint32_t)(4294967296.0 / 10 * 128 / WP_SR);
    a1 = amount(v, p);
    if (a0 < 0)
        a0 = a1;
    for (s = 0; s < n; s += SUB) {
        int e = s + SUB < n ? s + SUB : n, r = (a1 - a0) * (s + SUB / 2);

        setup(&w, wave, mode, a0 + (n == 128 ? r >> 7 : r / n));     /* TE always renders 128 */
        switch (mode) {
#define LOOP(M) case M: \
            for (i = s; i < e; i++) { \
                int y; \
                ph += inc[i]; \
                y = warp_m(&w, ph >> 16, M); \
                if (det) { \
                    ph2 += inc[i] + (uint32_t)((int32_t)(inc[i] >> 16) * det); \
                    y = (y + warp_m(&w, ph2 >> 16, M)) >> 1; \
                } \
                if (color) { \
                    lp += ((y - lp) * g) >> 15; \
                    y = lp; \
                } \
                out[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y); \
            } \
            break;
        LOOP(WP_BEND) LOOP(WP_SYNC) LOOP(WP_MIRROR) LOOP(WP_FOLD)
        LOOP(WP_QUANT) LOOP(WP_SQUEEZE) LOOP(WP_PINCH) LOOP(WP_WRAP)
        LOOP(WP_FM) LOOP(WP_GAP) LOOP(WP_BITREV) LOOP(WP_CHAOS)
        LOOP(WP_STAIR) LOOP(WP_HARM) LOOP(WP_RING) LOOP(WP_XOR)
#undef LOOP
        default:
            break;
        }
    }
    v->phase = ph;
    v->phase2 = ph2;
    v->lp = lp;
    v->a_prev = a1;
}

void wp_cycle(int wave, int mode, int a, int16_t *out, int n)
{
    wk w;
    int i;

    setup(&w, wave < 0 ? 0 : wave % WP_WAVES, mode < 0 ? 0 : mode % WP_MODES, a);
    for (i = 0; i < n; i++)
        out[i] = (int16_t)warp_m(&w, (unsigned int)((i * 65536) / n) & 0xffff, w.mode);
}
