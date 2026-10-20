/* M1 voice (see m1_core.h, README.md). Own code; the instrument structure: two multisound
 * oscillators, a low-pass VDF with its envelope.
 *
 * Per voice block: osc A and B decode the source samples the block needs (ADPCM: 16 samples per
 * 9-byte block, predictor history restored at the loop start), then interpolate them linearly at
 * 88.2 kHz, at the block's mean TE increment. High notes play an octave-down copy of the sample
 * (pre-rendered by gen_m1.py, chosen at note on), so the step stays <= 1 (single cycles) / 2 there. Levels (A decay,
 * B fade-in, crossfaded mix) ramp per 32 samples; a voice peaks at about half scale, so chords leave
 * headroom in TE's voice mix, and the output has a soft knee (as WHEEL) rather than a hard clip.
 * Filter: trapezoidal state-variable low-pass (Simper), coefficients four
 * times per block; off when the cutoff is fully open with no envelope and no resonance. */
#include "m1_core.h"
#include "m1_tables.h"

/* the multisounds: from a SoundFont when gen_m1.py --rom / --sf2 made one; the benches use the built-in.
 * The SoundFont's sample data is a file (M1_ROM_FILE) that m1.c loads at runtime (m1_set_rom); the
 * host tests (not __bfin__) and the SoundFont bench (-DM1_ROM_INLINE) build it in instead. */
#if !defined(__bfin__) && !defined(M1_ROM_INLINE)
#define M1_ROM_INLINE
#endif
#if !defined(SDK_BENCH) && defined(__has_include)
#if __has_include("build/m1_data.inc")
#include "build/m1_data.inc"
#ifdef M1_ROM_INLINE
#include "build/m1_rom.inc"
#endif
#define M1_DATA_DONE
#endif
#endif
#ifndef M1_DATA_DONE
#include "m1_builtin.inc"
#endif
#if defined(M1_ROM_FILE) && !defined(M1_ROM_INLINE)
#define M1_ROM_RUNTIME
static const uint8_t *volatile m1_rom;          /* 0 until loaded: notes stay silent */
#endif

#define FMT_PCM16   1
#define SUB         32
#define CLAMP       60000
#define INC69       21426141u                   /* 440 Hz at 88.2 kHz, 2^32 per cycle */
#define GAIN_ONE    16000                       /* a voice's waves together, Q15 (peak ~0.5 FS) */
#define SEMI_CUT    301                         /* one semitone on the CUTOFF scale (9 octaves / 32767) */
#define KNEE        24576                       /* soft knee 3/4 up, as WHEEL: never a hard clip */

static const int8_t filt[8][2] = {              /* ADPCM predictors, /64 (as gen_m1.py) */
    { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 }, { 30, 0 }, { 90, -24 }, { 126, -63 },
};

/* key thresholds: INC69 * 2^((j - 0.5) / 12) */
static const uint32_t key_th[13] = {
    20816180, 22053975, 23365372, 24754749, 26226743, 27786267, 29438524, 31189030, 33043626,
    35008502, 37090216, 39295715, 41632360,
};

static int zone(int16_t knob, int n)
{
    int k = knob < 0 ? 0 : knob, z = k * n >> 15;

    return z >= n ? n - 1 : z;
}

int m1_nwaves(void) { return M1_NMULTI; }
int m1_have_rom(void) { return M1_HAVE_ROM; }
uint32_t m1_rom_bytes(void) { return M1_ROM_BYTES; }
const uint8_t *m1_rom_data(void) { return m1_rom; }
#ifdef M1_ROM_RUNTIME
const char *m1_rom_file(void) { return M1_ROM_FILE; }
uint32_t m1_rom_file_bytes(void) { return M1_ROM_FILE_BYTES; }
uint32_t m1_rom_file_sum(void) { return M1_ROM_SUM; }
void m1_set_rom(const uint8_t *data) { m1_rom = data; }
int m1_rom_ready(void) { return m1_rom != 0; }
#else
const char *m1_rom_file(void) { return 0; }
uint32_t m1_rom_file_bytes(void) { return 0; }
uint32_t m1_rom_file_sum(void) { return 0; }
void m1_set_rom(const uint8_t *data) { (void)data; }
int m1_rom_ready(void) { return 1; }
#endif
int m1_wave_of(int16_t knob) { return zone(knob, M1_NMULTI); }
int m1_waveb_of(int16_t knob) { return zone(knob, M1_NMULTI + 1) - 1; }
int m1_semis_of(int16_t knob) { return zone(knob, 49) - 24; }

int m1_steps_of(int16_t knob)                   /* knob = steps x 520 (+ rounding) */
{
    int s = ((knob < 0 ? 0 : knob) * 2017 + 524288) >> 20;

    return s > 63 ? 63 : s;
}

const char *m1_wave_name(int w)
{
    return w >= 0 && w < M1_NMULTI ? m1_multis[w].name : "OFF";
}

int m1_key_of_inc(uint32_t inc)
{
    int key = 69, j;

    if (!inc)
        return 0;
    while (inc < key_th[0]) {
        inc <<= 1;
        key -= 12;
    }
    while (inc >= key_th[12]) {
        inc >>= 1;
        key += 12;
    }
    for (j = 0; j < 11 && inc >= key_th[j + 1]; j++)
        ;
    key += j;
    return key < 0 ? 0 : key > 127 ? 127 : key;
}

uint32_t m1_inc_of_key(int key)
{
    int d = key - 69, o = d >= 0 ? d / 12 : -((11 - d) / 12), s = d - 12 * o;
    uint32_t v = (uint32_t)(((uint64_t)INC69 * m1_semi[s]) >> 16);

    return o >= 0 ? v << o : v >> -o;
}

const m1_sample *m1_pick(int w, int key)
{
    const m1_multi *m = &m1_multis[w < 0 ? 0 : w >= M1_NMULTI ? M1_NMULTI - 1 : w];
    const m1_zone *z = &m1_zones[m->zone];
    int i, best = 0, dist = 1000;

    for (i = 0; i < m->nzones; i++) {
        int d = key < z[i].lo ? z[i].lo - key : key > z[i].hi ? key - z[i].hi : 0;

        if (d < dist) {
            dist = d;
            best = i;
        }
    }
    return &m1_samples[z[best].sample];
}

static uint32_t raw_step(uint32_t inc, uint32_t ratio, const m1_sample *s)
{
    uint64_t st = ((((uint64_t)inc * ratio) >> 16) * s->k) >> 32;

    return st > 0xffffffffu ? 0xffffffffu : (uint32_t)st;
}

/* the copy for this pitch: PCM16 (single cycles, rich in harmonics) at a step <= 1, so nothing folds
 * above 44.1 kHz; ADPCM (sampled sounds, little up there) <= 2 */
const m1_sample *m1_level(const m1_sample *s, uint32_t inc, uint32_t ratio)
{
    while (s->mip != M1_NOMIP && raw_step(inc, ratio, s) > (s->fmt == FMT_PCM16 ? 1u << 16 : 2u << 16))
        s = &m1_samples[s->mip];
    return s;
}

/* ------------------------------------------------------------------ decoder */

/* ADPCM sample i of a sample's data (base): 16 samples per 9 bytes (header: shift | predictor << 4) */
static inline __attribute__((always_inline)) int32_t dec1(const uint8_t *base, uint32_t i, int32_t *h1, int32_t *h2,
                                                          uint8_t *hdr)
{
    const uint8_t *b = base + (i >> 4) * 9;
    const int8_t *f;
    int sh, q;
    int32_t v;

    if ((i & 15) == 0)
        *hdr = b[0];
    sh = *hdr & 15;
    f = filt[*hdr >> 4];
    q = (((b[1 + ((i & 15) >> 1)] >> ((i & 1) << 2)) & 15) ^ 8) - 8;   /* signed nibble */
    v = ((q * (1 << sh)) >> 1) + ((f[0] * *h1 + f[1] * *h2) >> 6);
    v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    *h2 = *h1;
    *h1 = v;
    return v;
}

/* a whole ADPCM block (16 samples) at b into d: header once */
static void dec16(const uint8_t *b, int32_t *ph1, int32_t *ph2, int16_t *d)
{
    int32_t h1 = *ph1, h2 = *ph2, m = 1 << (b[0] & 15), a1 = filt[b[0] >> 4][0], a2 = filt[b[0] >> 4][1], v;
    int j;

    for (j = 1; j <= 8; j++) {
        int x = b[j];

        v = ((((x & 15) ^ 8) - 8) * m >> 1) + ((a1 * h1 + a2 * h2) >> 6);
        v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        h2 = h1;
        h1 = v;
        *d++ = (int16_t)v;
        v = ((((x >> 4) ^ 8) - 8) * m >> 1) + ((a1 * h1 + a2 * h2) >> 6);
        v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        h2 = h1;
        h1 = v;
        *d++ = (int16_t)v;
    }
    *ph1 = h1;
    *ph2 = h2;
}

int16_t m1_osc_next(m1_osc *o, const uint8_t *rom)
{
    const m1_sample *s = o->s;

    if (!o->on)
        return 0;
    if (o->i >= s->len) {
        if (s->loop == M1_NOLOOP) {
            o->on = 0;
            return 0;
        }
        if (s->fmt == FMT_PCM16) {
            o->i = s->loop;
        } else {                                /* back to the loop's block, replay up to the start */
            o->i = s->loop & ~15u;
            o->h1 = s->h1;
            o->h2 = s->h2;
            for (; o->i < s->loop; o->i++)
                dec1(rom + s->off, o->i, &o->h1, &o->h2, &o->hdr);
        }
    }
    if (s->fmt == FMT_PCM16)
        return ((const int16_t *)(const void *)(rom + s->off))[o->i++];
    return (int16_t)dec1(rom + s->off, o->i++, &o->h1, &o->h2, &o->hdr);
}

void m1_osc_start(m1_osc *o, const m1_sample *s)
{
    o->s = s;
    o->i = 0;
    o->frac = 0;
    o->h1 = o->h2 = 0;
    o->hdr = 0;
    o->on = s != 0 && m1_rom_ready();
    if (o->on) {
        o->x0 = m1_osc_next(o, m1_rom);
        o->x1 = m1_osc_next(o, m1_rom);
    } else {
        o->x0 = o->x1 = 0;
    }
}

/* output stage: linear up to 3/4 of full scale, then a smooth knee approaching +-32767
 * (y = T + R d / (d + R)) instead of a hard clip; the divide only runs above the knee */
static inline int16_t soft(int32_t x)
{
    int32_t ax = x < 0 ? -x : x, d, y;

    if (ax <= KNEE)
        return (int16_t)x;
    d = ax - KNEE;
    y = KNEE + (int32_t)(((uint32_t)(32767 - KNEE) * (uint32_t)d) / (uint32_t)(d + 32767 - KNEE));
    return (int16_t)(x < 0 ? -y : y);
}

/* ------------------------------------------------------------------ voice */

static uint32_t semi_ratio(int s);

void m1_note_on(m1_voice *v, uint32_t inc, const int16_t *p)
{
    int key = m1_key_of_inc(inc), wa = m1_wave_of(p[M1_WAVE_A]), wb = m1_waveb_of(p[M1_WAVE_B]);
    int fa = m1_multis[wa].fixed, kb, sb = m1_semis_of(p[M1_BPITCH]);

    v->key = key;
    v->a.fixed = fa != 0xff;
    v->fixa = v->a.fixed ? m1_inc_of_key(fa) : 0;
    m1_osc_start(&v->a, m1_level(m1_pick(wa, v->a.fixed ? fa : key), v->a.fixed ? v->fixa : inc, 65536));
    if (wb >= 0) {
        int fb = m1_multis[wb].fixed;

        kb = key + sb;
        kb = kb < 0 ? 0 : kb > 127 ? 127 : kb;
        v->b.fixed = fb != 0xff;
        v->fixb = v->b.fixed ? m1_inc_of_key(fb) : 0;
        m1_osc_start(&v->b, m1_level(m1_pick(wb, v->b.fixed ? fb : kb), v->b.fixed ? v->fixb : inc,
                                     semi_ratio(sb)));
    } else {
        m1_osc_start(&v->b, 0);
    }
    v->lp = v->bp = 0;
    v->env = 32767;
    v->la = 32767;
    v->lb = m1_steps_of(p[M1_BFADE]) ? 0 : 32767;
}

/* 16.16 source samples per output sample, bounded so a block's work is too: ADPCM (decoded) at
 * most 4, PCM16 (copied) 16. With the octave-down copies (up to C8) only bends / LFO far above
 * that reach the cap; the pitch then stays there. */
#define STEP_ADPCM  (4u << 16)
#define STEP_PCM    (16u << 16)

static uint32_t step_of(uint32_t inc, uint32_t ratio, const m1_sample *s)
{
    uint32_t st = raw_step(inc, ratio, s), max = s->fmt == FMT_PCM16 ? STEP_PCM : STEP_ADPCM;

    return st > max ? max : st;
}

/* the source samples one block of one oscillator needs (step <= 16), and the mix; the audio thread
 * renders one voice at a time */
static int16_t src[16 * 128 + 4];
static int32_t acc[128];

/* decode the next m samples of o into d (loops wrap; after a one-shot's end: zeros) */
static void fill(m1_osc *o, int16_t *d, uint32_t m)
{
    const m1_sample *s = o->s;
    const uint8_t *base = m1_rom + s->off;
    uint32_t i = o->i, len = s->len, loop = s->loop, q = 0, c, e;
    int32_t h1 = o->h1, h2 = o->h2;
    uint8_t hdr = o->hdr;

    while (q < m) {
        if (!o->on || i >= len) {
            if (!o->on || loop == M1_NOLOOP) {
                o->on = 0;
                for (; q < m; q++)
                    d[q] = 0;
                break;
            }
            if (s->fmt == FMT_PCM16) {
                i = loop;
            } else {
                i = loop & ~15u;                /* the loop's block, replayed up to the start */
                h1 = s->h1;
                h2 = s->h2;
                for (; i < loop; i++)
                    dec1(base, i, &h1, &h2, &hdr);
            }
        }
        c = len - i < m - q ? len - i : m - q;
        e = i + c;
        if (s->fmt == FMT_PCM16) {
            const int16_t *pcm = (const int16_t *)(const void *)base;

            for (; i < e; i++)
                d[q++] = pcm[i];
        } else {
            while (i < e) {
                if ((i & 15) == 0 && e - i >= 16) {     /* whole blocks: header once */
                    dec16(base + (i >> 4) * 9, &h1, &h2, d + q);
                    q += 16;
                    i += 16;
                } else {
                    d[q++] = (int16_t)dec1(base, i++, &h1, &h2, &hdr);
                }
            }
        }
    }
    o->i = i;
    o->h1 = h1;
    o->h2 = h2;
    o->hdr = hdr;
}

/* one oscillator, interpolated, into x[0..n): st = 16.16 source samples per output sample (constant
 * for the block, <= STEP_PCM) */
static void osc_run(m1_osc *o, int16_t *x, int n, uint32_t st)
{
    uint32_t pos = o->frac, m = (pos + st * (uint32_t)n) >> 16;
    int k;

    if (!o->on && !o->x0 && !o->x1) {
        for (k = 0; k < n; k++)
            x[k] = 0;
        return;
    }
    src[0] = o->x0;
    src[1] = o->x1;
    fill(o, src + 2, m);
    for (k = 0; k < n; k++, pos += st) {
        const int16_t *q = src + (pos >> 16);

        x[k] = (int16_t)(q[0] + (((q[1] - q[0]) * (int32_t)((pos & 0xffff) >> 2)) >> 14));
    }
    o->frac = pos & 0xffff;
    o->x0 = src[m];
    o->x1 = src[m + 1];
}

static uint32_t semi_ratio(int s)                /* 2^(s / 12), Q16, s = -24 .. +24 */
{
    int o = 0;

    for (; s < 0; s += 12)
        o--;
    for (; s >= 12; s -= 12)
        o++;
    return o >= 0 ? m1_semi[s] << o : m1_semi[s] >> -o;
}

void m1_render(m1_voice *v, int16_t *out, int n, const uint32_t *inc, const int16_t *p)
{
    int16_t xa[128], xb[128];
    int i, s, mix = p[M1_MIX] < 0 ? 0 : p[M1_MIX] > 32767 ? 32767 : p[M1_MIX], on_b = v->b.s != 0;
    int det = (m1_steps_of(p[M1_DETUNE]) * 969) >> 6;          /* 63 steps: 25 cents = 954 / 65536 */
    uint32_t ra = (uint32_t)(65536 - det), rb = (uint32_t)(((uint64_t)semi_ratio(m1_semis_of(p[M1_BPITCH]))
                                                            * (uint32_t)(65536 + det)) >> 16);
    int32_t la0 = v->la, lb0 = v->lb, la1, lb1, ga0, ga1, gb0, gb1;
    int ad = m1_steps_of(p[M1_ADECAY]), bf = m1_steps_of(p[M1_BFADE]);
    int cut = p[M1_CUTOFF] < 0 ? 0 : p[M1_CUTOFF], fenv = p[M1_FENV] < 0 ? 0 : p[M1_FENV];
    int res = p[M1_RES] < 0 ? 0 : p[M1_RES], d = p[M1_FDECAY] < 0 ? 0 : p[M1_FDECAY];
    int di = d >> 10, df = d & 1023, env0 = v->env, env1;
    int32_t ic1 = v->bp, ic2 = v->lp;
    int k15 = 65536 - ((res * 55706) >> 15);                    /* damping 2 .. 0.3, Q15 */

    if (n > 128)
        n = 128;
    la1 = (la0 * m1_adecay[ad]) >> 15;
    lb1 = bf ? lb0 + (((32767 - lb0) * m1_bfade[bf]) >> 15) : 32767;
    v->la = la1;
    v->lb = lb1;
    if (on_b) {                                 /* a true A/B crossfade: they add to one wave's level */
        int wa = 32767 - mix, wb = mix;

        ga0 = (((GAIN_ONE * wa) >> 15) * la0) >> 15;
        ga1 = (((GAIN_ONE * wa) >> 15) * la1) >> 15;
        gb0 = (((GAIN_ONE * wb) >> 15) * lb0) >> 15;
        gb1 = (((GAIN_ONE * wb) >> 15) * lb1) >> 15;
    } else {
        ga0 = (GAIN_ONE * la0) >> 15;
        ga1 = (GAIN_ONE * la1) >> 15;
        gb0 = gb1 = 0;
    }
    {                                           /* pitch: the block's mean increment */
        uint32_t ia = v->a.fixed ? v->fixa : (inc[0] >> 1) + (inc[n - 1] >> 1);

        if (v->a.s)
            osc_run(&v->a, xa, n, step_of(ia, ra, v->a.s));
        else
            for (i = 0; i < n; i++)
                xa[i] = 0;
    }
    if (on_b) {
        uint32_t ib = v->b.fixed ? v->fixb : (inc[0] >> 1) + (inc[n - 1] >> 1);

        osc_run(&v->b, xb, n, step_of(ib, rb, v->b.s));
    }

    v->env = (env0 * (m1_fdecay[di] + (((m1_fdecay[di + 1] - m1_fdecay[di]) * df) >> 10))) >> 15;
    env1 = v->env;
    for (s = 0; s < n; s += SUB) {              /* levels and filter coefficients per 32 samples */
        int e = s + SUB < n ? s + SUB : n, mid = s + SUB / 2;
        int32_t ga = ga0 + (((ga1 - ga0) * mid) >> 7), gb = gb0 + (((gb1 - gb0) * mid) >> 7);
        int env = env0 + (((env1 - env0) * mid) >> 7);
        int c = cut + ((((fenv * env) >> 15) * 21700) >> 15)          /* up to 6 octaves */
                + (((v->key - 60) * SEMI_CUT * (p[M1_KEYTRK] < 0 ? 0 : p[M1_KEYTRK])) >> 15);
        int ci, gg, a1, a2, a3;

        if (on_b)
            for (i = s; i < e; i++)
                acc[i] = (xa[i] * ga + xb[i] * gb) >> 15;
        else
            for (i = s; i < e; i++)
                acc[i] = (xa[i] * ga) >> 15;
        if (cut >= 32700 && !fenv && !res) {    /* filter fully open: the samples as they are */
            for (i = s; i < e; i++)
                out[i] = soft(acc[i]);
            ic1 = ic2 = 0;
            continue;
        }
        c = c > 32767 ? 32767 : c < 0 ? 0 : c;
        ci = c >> 9;
        gg = m1_cut[ci] + (((m1_cut[ci + 1] - m1_cut[ci]) * (c & 511)) >> 9);
        a1 = (int)((32768u << 15) / (uint32_t)(32768 + ((gg * (gg + k15)) >> 15)));   /* 1/(1+g(g+k)) */
        a2 = (gg * a1) >> 15;
        a3 = (gg * a2) >> 15;
        for (i = s; i < e; i++) {               /* trapezoidal SVF (Simper), low-pass */
            int32_t v3 = acc[i] - ic2, v1 = ((a1 * ic1) >> 15) + ((a2 * v3) >> 15);
            int32_t v2 = ic2 + ((a2 * ic1) >> 15) + ((a3 * v3) >> 15);

            ic1 = 2 * v1 - ic1;
            ic2 = 2 * v2 - ic2;
            ic1 = ic1 > CLAMP ? CLAMP : ic1 < -CLAMP ? -CLAMP : ic1;
            ic2 = ic2 > CLAMP ? CLAMP : ic2 < -CLAMP ? -CLAMP : ic2;
            out[i] = soft(v2);
        }
    }
    v->lp = ic2;
    v->bp = ic1;
}
