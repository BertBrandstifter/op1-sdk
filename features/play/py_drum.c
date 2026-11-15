/* py_drum: PLAY's drum voices (py_drum.h). */
#include "py_drum.h"

/* 2^(i / 12) x 65536, i = 0..12 */
static const uint32_t semi_q16[13] = {
    65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715, 131072,
};

uint32_t py_pow2_q16(int32_t x512)
{
    int32_t semis, frac, oct, rem;
    uint32_t a, b, v;

    if (x512 > 96 * 512)
        x512 = 96 * 512;
    if (x512 < -96 * 512)
        x512 = -96 * 512;
    semis = x512 >> 9;                          /* floor */
    frac = x512 & 511;
    oct = semis >= 0 ? semis / 12 : -((11 - semis) / 12);
    rem = semis - oct * 12;
    a = semi_q16[rem];
    b = semi_q16[rem + 1];
    v = a + (uint32_t)(((b - a) * (uint32_t)frac) >> 9);
    return oct >= 0 ? v << oct : v >> -oct;
}

uint32_t py_drum_frames(uint16_t hi, uint16_t lo)
{
    uint32_t pos32 = ((uint32_t)hi << 16) + 2u * lo;

    return (uint32_t)(((uint64_t)pos32 * PY_DRUM_FRAMES) >> 31);
}

void py_drum_hit(py_drums *d, const int16_t *pcm, const int16_t key[8], int semis, int32_t gain,
                 uint32_t hold, int full, uint32_t delay)
{
    uint32_t start = py_drum_frames((uint16_t)key[1], (uint16_t)key[5]);
    uint32_t end = py_drum_frames((uint16_t)key[2], (uint16_t)key[6]);
    int32_t vol = key[7] < 0 ? 0 : key[7], kit;
    int mode = ((uint16_t)key[3] >> 13) & 3, i, pick = 0;
    py_dv *v;

    if (end > PY_DRUM_FRAMES)
        end = PY_DRUM_FRAMES;
    if (!pcm || start >= end)
        return;
    for (i = 0; i < PY_DVOICES; i++) {          /* a free voice, else the oldest */
        if (!d->v[i].on) {
            pick = i;
            break;
        }
        if (d->v[i].age < d->v[pick].age)
            pick = i;
    }
    v = &d->v[pick];
    v->pcm = pcm;
    v->start = start;
    v->len = end - start;
    v->pos = 0;
    v->frac = 0;
    v->rev = ((uint16_t)key[4] >> 14) != 0;
    v->loop = mode == 2;
    v->oneshot = mode == 1;
    v->released = 0;
    v->fade = 0;
    v->delay = delay;
    v->inc = py_pow2_q16(key[0] + semis * 512);
    kit = (vol * vol) >> 11;                    /* (W7 / 8192)^2, Q15 */
    gain = (int32_t)(((int64_t)gain * kit) >> 15);
    gain = (int32_t)(((int64_t)gain * PY_DRUM_CAL) >> 15);
    v->gain = gain > 65535 ? 65535 : gain < 0 ? 0 : gain;
    v->gate = hold;
    v->age = ++d->ages;
    v->on = 1;
    if (!full)
        v->oneshot = 0;                         /* a gated hit always stops at its gate */
}

static void render_one(py_dv *v, int32_t *out, int n)
{
    int i;

    for (i = 0; i < n && v->on; i++) {
        uint32_t f0, f1;
        int32_t s0, s1, y, g = v->gain;

        if (v->delay) {
            v->delay--;
            continue;
        }
        if (v->gate != 0xffffffffu) {
            if (!v->gate) {                     /* note-off */
                v->gate = 0xffffffffu;
                v->released = 1;
                if (!v->oneshot)
                    v->fade = PY_DRUM_FADE;
            } else {
                v->gate--;
            }
        }
        f0 = v->rev ? v->start + v->len - 1 - v->pos : v->start + v->pos;
        if (v->pos + 1 < v->len)
            f1 = v->rev ? f0 - 1 : f0 + 1;
        else
            f1 = f0;
        s0 = v->pcm[f0];
        s1 = v->pcm[f1];
        {   /* s0 + ((s1 - s0) frac >> 16) without a 64-bit multiply (a library call on the Blackfin):
             * frac = 256 fh + fl, d frac >> 16 = (d fh + (d fl >> 8)) >> 8, exactly */
            int32_t d = s1 - s0, fr = (int32_t)v->frac;

            y = s0 + ((d * (fr >> 8) + ((d * (fr & 255)) >> 8)) >> 8);
        }
        if (v->fade) {
            g = (int32_t)((uint32_t)g * v->fade / PY_DRUM_FADE);    /* g <= 65535, fade 16 bits */
            if (!--v->fade) {
                v->on = 0;
                out[i] += (y * g) >> 15;
                break;
            }
        }
        out[i] += (y * g) >> 15;                /* |y| <= 32768, 0 <= g <= 65535: inside 32 bits */
        v->frac += v->inc;
        v->pos += v->frac >> 16;
        v->frac &= 0xffffu;
        if (v->pos >= v->len) {
            if (v->loop && !v->released)
                v->pos %= v->len;
            else
                v->on = 0;
        }
    }
}

void py_drum_render(py_drums *d, int32_t *out, int n)
{
    int i;

    for (i = 0; i < PY_DVOICES; i++)
        if (d->v[i].on)
            render_one(&d->v[i], out, n);
}

int py_drum_active(const py_drums *d)
{
    int i, n = 0;

    for (i = 0; i < PY_DVOICES; i++)
        n += d->v[i].on;
    return n;
}
