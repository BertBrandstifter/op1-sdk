/* RESO effect core, see reso_core.h and README.md. */
#include "reso_core.h"

#define MASK        (RS_LEN - 1)
#define MIN_Q16     (8u << 16)                  /* shortest period: 8 samples (5.5 kHz) */
#define MAX_Q16     ((uint32_t)(RS_LEN - 4) << 16)
#define DC_R        32604                       /* input DC blocker pole (0.995, ~35 Hz) */
#define KNEE        16384                       /* string soft knee */

/* voicings: semitones above the lowest sounding note; PLAYED uses the notes themselves */
static const uint8_t voicing[RS_CHORDS][RS_STRINGS] = {
    { 0, 0, 0, 0 },         /* PLAYED */
    { 0, 12, 19, 24 },      /* ROOT: octaves + fifth */
    { 0, 7, 12, 19 },       /* FIFTH */
    { 0, 4, 7, 12 },        /* MAJ */
    { 0, 3, 7, 12 },        /* MIN */
    { 0, 5, 7, 12 },        /* SUS4 */
    { 0, 4, 7, 11 },        /* MAJ7 */
    { 0, 3, 7, 10 },        /* MIN7 */
};

/* 2^(-s/12) in 16.16 (period factor of s semitones up), s = 0..24 */
static const uint32_t down_q16[25] = {
    65536, 61858, 58386, 55109, 52016, 49097, 46341, 43740, 41285, 38968, 36781, 34716, 32768,
    30929, 29193, 27554, 26008, 24548, 23170, 21870, 20643, 19484, 18390, 17358, 16384,
};

/* detune per string in units of ~12.5 cents (25 cents at full DETUNE) */
static const int8_t spread[RS_STRINGS] = { 0, 2, -2, 1 };

static uint32_t isqrt(uint32_t x)
{
    uint32_t r = 0, b = 1u << 30;

    while (b > x)
        b >>= 2;
    while (b) {
        if (x >= r + b) {
            x -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

static int32_t knob(const int16_t *p, int i)
{
    return p[i] < 0 ? 0 : p[i];
}

static int16_t sat16(int32_t x)
{
    return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

int rs_voicing(int16_t k)
{
    int32_t v = ((int32_t)(k < 0 ? 0 : k) * RS_CHORDS) >> 15;

    return v >= RS_CHORDS ? RS_CHORDS - 1 : v;
}

void rs_reset(rs_state *s)
{
    int i, j;

    for (i = 0; i < RS_STRINGS; i++) {
        for (j = 0; j < RS_LEN; j++)
            s->line[i][j] = 0;
        s->delay_q16[i] = s->target_q16[i] = 100u << 16;
        s->lp[i] = 0;
        s->err[i] = 0;
    }
    s->wpos = 0;
    s->hp_x = s->hp_y = 0;
    s->out_x = s->out_y = 0;
    s->tuned = 0;
}

int rs_chord(rs_state *s, const uint32_t *inc, const uint8_t *active, int voices, const int16_t *p)
{
    uint32_t notes[RS_VOICES], per, t;
    int n = 0, i, j, v = rs_voicing((int16_t)knob(p, RS_CHORD));
    int oct = (int)((knob(p, RS_OCTAVE) * 5) >> 15) - 2, det = (int)knob(p, RS_DETUNE);

    if (voices > RS_VOICES)
        voices = RS_VOICES;
    for (i = 0; i < voices; i++)
        if (active[i] && inc[i] > 0x1000)       /* below ~0.08 Hz: not a note */
            notes[n++] = inc[i];
    if (!n)
        return 0;                               /* nothing sounds: the strings keep their notes */
    for (i = 1; i < n; i++)                     /* lowest pitch first */
        for (j = i; j > 0 && notes[j - 1] > notes[j]; j--) {
            t = notes[j];
            notes[j] = notes[j - 1];
            notes[j - 1] = t;
        }
    for (i = 0; i < RS_STRINGS; i++) {
        /* period at 44.1 kHz in 16.16 samples: 44100 / f = 2^31 / inc (inc at 88.2 kHz) */
        if (v == 0) {
            per = (uint32_t)(((uint64_t)1 << 47) / notes[i % n]);
            per >>= i / n;                      /* fewer notes than strings: octaves up */
        } else {
            per = (uint32_t)(((((uint64_t)1 << 47) / notes[0]) * down_q16[voicing[v][i]]) >> 16);
        }
        if (det && spread[i])                   /* DETUNE: up to +-25 cents */
            per = (uint32_t)(((uint64_t)per * (uint32_t)(65536 - ((spread[i] * 476 * det) >> 15))) >> 16);
        if (oct > 0)
            per >>= oct;
        else if (oct < 0 && per < (MAX_Q16 >> -oct))
            per <<= -oct;
        while (per > MAX_Q16)                   /* fold into the strings' range */
            per >>= 1;
        while (per < MIN_Q16)
            per <<= 1;
        s->target_q16[i] = per;
        if (!s->tuned)
            s->delay_q16[i] = per;
    }
    s->tuned = 1;
    return n;
}

void rs_process(rs_state *s, int16_t *buf, int n, const int16_t *p)
{
    int32_t mix = knob(p, RS_MIX), e = knob(p, RS_DECAY) * 10, loss, fb, gin, damp, drive, knee;
    int32_t hx = s->hp_x, hy = s->hp_y, ox = s->out_x, oy = s->out_y;
    uint32_t w = s->wpos, glide = (uint32_t)knob(p, RS_GLIDE);
    int i, k;

    if (!mix)
        return;                                 /* dry: untouched, bit-exact */

    /* DECAY: loss per pass 2^-4 .. 2^-14 (Q15), interpolated between the powers of two */
    k = 4 + (e >> 15);
    loss = (32768 >> k) - (((32768 >> (k + 1)) * (e & 0x7fff)) >> 15);
    if (loss < 2)
        loss = 2;
    fb = 32768 - loss;
    /* input gain ~ sqrt(loss): long strings build up slowly, so full normalisation (~loss)
     * would leave them silent on short notes; the soft clip below catches the rest */
    gin = (int32_t)(isqrt((uint32_t)loss << 15) * 3 / 2);
    damp = 6000 + ((knob(p, RS_TONE) * 26767) >> 15);                    /* TONE: lowpass in loop */
    /* DRIVE: louder into the strings and a lower knee (more saturation); the loop gain stays
     * below 1 (knee slope 1/4), so the strings can never run away */
    drive = knob(p, RS_DRIVE);
    knee = KNEE - ((drive * (KNEE - 2048)) >> 15);
    gin += (int32_t)(((int64_t)gin * drive) >> 13);
    if (gin > 32767)
        gin = 32767;

    /* GLIDE: the periods move toward their targets once per block */
    for (k = 0; k < RS_STRINGS; k++) {
        if (!glide) {
            s->delay_q16[k] = s->target_q16[k];
        } else {
            /* move 1/2^g of the way per block, g = 1..10 (~3 ms .. ~1.5 s); a shift, no divide */
            int32_t dlt = (int32_t)(s->target_q16[k] - s->delay_q16[k]), g = 1 + (int32_t)((glide * 10) >> 15);

            s->delay_q16[k] += (uint32_t)(dlt >= 0 ? dlt >> g : -((-dlt) >> g));
            if ((dlt >= 0 ? dlt : -dlt) >> g == 0)
                s->delay_q16[k] = s->target_q16[k];
        }
    }
    while (n > 0) {
        int32_t inb[64], acc[64];
        int m = n < 64 ? n : 64, j;

        for (i = 0; i < m; i++) {
            int32_t x = buf[i];

            hy = x - hx + (hy * DC_R) / 32768;  /* no DC into the strings (toward zero: no
                                                 * stuck residue) */
            hx = x;
            inb[i] = (sat16(hy) * gin) >> 15;
            acc[i] = 0;
        }
        /* one string at a time over the block: its state stays in registers */
        for (j = 0; j < RS_STRINGS; j++) {
            int16_t *ln = s->line[j];
            uint32_t d = s->delay_q16[j], di = (d + 0xffff) >> 16, ww = w;
            int32_t fr = (int32_t)(((0x10000 - (d & 0xffff)) & 0xffff) >> 1);   /* Q15 */
            int32_t lp = s->lp[j], e = s->err[j];

            for (i = 0; i < m; i++, ww++) {
                int32_t a = ln[(ww - di) & MASK], c = ln[(ww - di + 1) & MASK], v;

                lp += ((a + (((c - a) * fr) >> 15) - lp) * damp + 16384) >> 15;
                v = lp * fb + e;                /* error feedback: the rounding remainder goes
                                                 * into the next sample, so small signals
                                                 * neither die nor build up DC */
                e = v & 0x7fff;
                v = inb[i] + (v >> 15);
                if (v > knee)                   /* soft knee: transparent below the knee, 4:1
                                                 * above: the strings limit themselves */
                    v = knee + ((v - knee) >> 2);
                else if (v < -knee)
                    v = -knee - ((-knee - v) >> 2);
                ln[ww & MASK] = sat16(v);
                acc[i] += lp;
            }
            s->lp[j] = lp;
            s->err[j] = e;
        }
        for (i = 0; i < m; i++) {
            int32_t x = buf[i], wet = acc[i] >> 1;

            oy = wet - ox + (oy * DC_R) / 32768;    /* the strings also ring at 0 Hz: no DC */
            ox = wet;
            buf[i] = sat16(x + (((sat16(oy) - x) * mix) >> 15));
        }
        w += (uint32_t)m;
        buf += m;
        n -= m;
    }
    s->hp_x = hx;
    s->hp_y = hy;
    s->out_x = ox;
    s->out_y = oy;
    s->wpos = w;
}
