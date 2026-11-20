/* py_synth: PLAY's synth voices (py_synth.h). */
#include "py_synth.h"
#include "py_drum.h"                            /* py_pow2_q16 */

/* TE's halfband (0xffa005d4): y[n] = x[2n-31] / 2 + sum_j h[j] (x[2n-62+2j] + x[2n-2j]), fract16 */
static const int16_t hb[16] = {
    -1, 2, -6, 13, -27, 50, -88, 145, -231, 355, -532, 790, -1184, 1856, -3334, 10382,
};

/* 32-bit only (a 64-bit multiply is a library call on the Blackfin: 1024 of them per block before):
 * every sample s = 65536 hi + lo (lo a signed 16-bit), so h s = 65536 (h hi) + h lo with 16 x 16-bit
 * multiplies; the two sums stay inside 32 bits for |s| < 2^30 (sum |h| + 16384 = 35380, x 32768).
 * Exactly (sum >> 15) of the 64-bit form: 2 (sum hi) + ((sum lo) >> 15). */
void py_syn_decimate(int32_t *hist, const int32_t *in, int32_t *out)
{
    static int32_t z[62 + 128];
    int i, n, j;

    for (i = 0; i < 62; i++)
        z[i] = hist[i];
    for (i = 0; i < 128; i++)
        z[62 + i] = in[i];
    for (n = 0; n < 64; n++) {
        const int32_t *x = z + 2 * n;
        int32_t c = x[31], ah, al;
        int16_t lo = (int16_t)c, hi = (int16_t)((c - lo) >> 16);

        ah = hi * 16384;                        /* 0.5 in Q15 */
        al = lo * 16384;
        for (j = 0; j < 16; j++) {
            int32_t s = x[2 * j] + x[62 - 2 * j];
            int16_t h = hb[j];

            lo = (int16_t)s;
            hi = (int16_t)((s - lo) >> 16);
            ah += h * hi;
            al += h * lo;
        }
        out[n] = ah * 2 + (al >> 15);
    }
    for (i = 0; i < 62; i++)
        hist[i] = z[128 + i];
}

uint32_t py_syn_inc(int key)
{
    key = key < 0 ? 0 : key > 127 ? 127 : key;
    return (uint32_t)(((uint64_t)PY_INC_A4 * py_pow2_q16((key - 69) * 512)) >> 16);
}

void py_syn_reset(py_syn *s)
{
    int i;

    for (i = 0; i < PY_SPOLY; i++)
        s->v[i].on = 0;
    s->phase = 0;
}

void py_syn_note(py_syn *s, int key, int vel7, uint32_t hold)
{
    int i, pick = 0, cap = s->cap >= 1 && s->cap <= PY_SPOLY ? s->cap : PY_SPOLY;
    py_sv *v;

    for (i = 0; i < cap; i++) {
        if (!s->v[i].on) {
            pick = i;
            break;
        }
        if (s->v[i].age < s->v[pick].age)
            pick = i;
    }
    v = &s->v[pick];
    vel7 = vel7 < 1 ? 1 : vel7 > 127 ? 127 : vel7;
    v->vel = (int16_t)(vel7 << 8);
    v->inc = py_syn_inc(key);
    v->gate = hold;
    v->started = 0;
    v->released = 0;
    v->key_up = 0;
    v->age = ++s->ages;
    v->on = 1;
}

void py_syn_restart(py_syn *s)
{
    int i;

    for (i = 0; i < PY_SPOLY; i++)
        if (s->v[i].on && !s->v[i].released)
            s->v[i].started = 0;
        else
            s->v[i].on = 0;
}

int py_syn_active(const py_syn *s)
{
    int i, n = 0;

    for (i = 0; i < PY_SPOLY; i++)
        n += s->v[i].on;
    return n;
}

int py_syn_tail(const py_syn *s)
{
    int i;

    for (i = 0; i < PY_SPOLY; i++)
        if (s->v[i].on && !s->v[i].released)
            return 0;
    return 1;
}

static void start_voice(py_sv *v, const py_syn_api *api)
{
    int i;

    for (i = 0; i < 6; i++)
        v->env[i] = 0;
    ((int16_t *)v->env)[0x14 / 2] = 0x7fff;     /* TE's envelope gain at +0x14 */
    api->env_start(v->env);
    v->started = 1;
}

/* the note-off once `samples` output samples of the gate have passed */
static void gate(py_sv *v, const py_syn_api *api, int slot, uint32_t samples)
{
    if (v->released)
        return;
    if (v->gate <= samples) {
        api->env_release(v->env, slot);
        v->released = 1;
        v->key_up = 1;
    } else {
        v->gate -= samples;
    }
}

static int peak_of(const int16_t *x, int n)
{
    int i, m = 0;

    for (i = 0; i < n; i++) {
        int a = x[i] < 0 ? -x[i] : x[i];

        if (a > m)
            m = a;
    }
    return m;
}

uint32_t py_syn_block(py_syn *s, const py_syn_api *api, int inst, const int16_t *knobs,
                      const int16_t *adsr, int slot, int32_t gain, int32_t *bus88, int32_t *out44,
                      uint32_t blk)
{
    static int32_t sum[128];
    static int16_t eng[128], env[128], env2[128];
    static uint32_t inc[128];
    int i, k, j, first = 1;
    unsigned fresh = 0;
    uint32_t pk = 0;

    if (s->eco && s->phase) {                   /* ECO: the second output block of the pair */
        for (i = 0; i < 64; i++) {
            int32_t x = s->eco_buf[64 + i], a = x < 0 ? -x : x;

            out44[i] += x;
            if ((uint32_t)a > pk)
                pk = (uint32_t)a;
        }
        s->phase = 0;
        return pk;
    }
    if (s->eco && ((blk ^ s->par) & 1))
        return 0;                               /* ECO: not this track's block (its notes wait one) */
    for (i = 0; i < 128; i++)
        sum[i] = 0;
    for (k = 0; k < PY_SPOLY; k++)
        fresh = (unsigned)(fresh | (s->v[k].on && !s->v[k].started) << k);
    for (j = 0; j < 2 * PY_SPOLY; j++) {        /* new notes first, then the others */
        py_sv *v = &s->v[j % PY_SPOLY];
        int flags = 0, done;

        k = j % PY_SPOLY;
        if (!v->on || (j < PY_SPOLY) != ((fresh >> k) & 1))
            continue;
        if (!v->started) {                      /* a new note on this voice (also a stolen one) */
            start_voice(v, api);
            flags |= PY_SYN_FRESH;
        }
        if (first)
            flags |= PY_SYN_BLOCK;
        if (s->eco)
            flags |= PY_SYN_HALF | (s->boost ? PY_SYN_BOOST : 0);
        for (i = 0; i < 128; i++)
            inc[i] = s->eco ? v->inc << 1 : v->inc;
        if (s->eco) {                           /* two output blocks: two envelope blocks */
            gate(v, api, slot, 64);
            done = api->env_render(v->env, env, adsr, &v->vel);
            gate(v, api, slot, 64);
            done |= api->env_render(v->env, env2, adsr, &v->vel);
            for (i = 0; i < 64; i++) {
                env[i] = env[2 * i];
                env[64 + i] = env2[2 * i];
            }
        } else {
            gate(v, api, slot, 64);
            done = api->env_render(v->env, env, adsr, &v->vel);
        }
        if (v->key_up && !(flags & PY_SYN_FRESH)) {    /* (a note shorter than a block: on first) */
            v->key_up = 0;
            if (api->key_up)
                api->key_up(inst, k, knobs);
        }
        if (!api->render(inst, k, eng, knobs, inc, env, flags)) {
            v->on = 0;                          /* no engine for this sound: silence */
            continue;
        }
        if (v->key_up) {
            v->key_up = 0;
            if (api->key_up)
                api->key_up(inst, k, knobs);
        }
        first = 0;
        for (i = 0; i < 128; i++)
            sum[i] += (eng[i] * env[i]) >> 15;
        if (done || (v->released && peak_of(env, 128) < PY_ENV_FLOOR))
            v->on = 0;                          /* the envelope is done or below -60 dB */
    }
    if (first)
        return 0;                               /* nothing rendered */
    if (s->eco) {
        for (i = 0; i < 128; i++)
            s->eco_buf[i] = (sum[i] * gain) >> 15;
        for (i = 0; i < 64; i++) {
            int32_t x = s->eco_buf[i], a = x < 0 ? -x : x;

            out44[i] += x;
            if ((uint32_t)a > pk)
                pk = (uint32_t)a;
        }
        s->phase = 1;
        return pk;
    }
    for (i = 0; i < 128; i++) {
        int32_t x = (sum[i] * gain) >> 15, a = x < 0 ? -x : x;

        bus88[i] += x;
        if ((uint32_t)a > pk)
            pk = (uint32_t)a;
    }
    return pk;
}
