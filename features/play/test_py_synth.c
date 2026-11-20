/* host test for py_synth.c (tools/test_all.py builds it with its sibling py_synth.c; py_drum.c is
 * included for py_pow2_q16).
 *
 * Pitch increments, TE's halfband decimator (DC gain, a 1 kHz tone passes, 30 kHz is gone), and the
 * voices against stand-ins for the engine and TE's envelope: FRESH on a note's first block, BLOCK on
 * the instance's first voice, the instance and voice numbers, the note-off after the hold, the
 * voice ends with its envelope or below -60 dB after the release, the voice cap and stealing, the
 * track gain and peak, ECO (one render per two blocks at twice the step, two envelope blocks), a
 * sound without an engine stays silent. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "py_synth.h"
#include "py_drum.c"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* stand-ins */
static int renders, last_inst, last_v, last_flags, first_v, first_flags, no_engine, releases, rel_slot, env_calls, quiet_tail;
static uint32_t last_inc;
static int key_ups, key_up_v, key_up_after_render, last_env0, seq;

static int st_render(int inst, int v, int16_t *out, const int16_t *knobs, const uint32_t *inc,
                     const int16_t *env, int flags)
{
    int i;

    (void)knobs;
    if (no_engine)
        return 0;
    renders++;
    seq = 1;                                    /* a key-up after this: after the render */
    last_env0 = env ? env[0] : -1;
    if (flags & PY_SYN_BLOCK) {
        first_v = v;
        first_flags = flags;
    }
    last_inst = inst;
    last_v = v;
    last_flags = flags;
    last_inc = inc[0];
    for (i = 0; i < 128; i++)
        out[i] = 1000;
    return 1;
}

static void st_key_up(int inst, int v, const int16_t *knobs)
{
    (void)inst;
    (void)knobs;
    key_ups++;
    key_up_v = v;
    key_up_after_render = seq;
}

static void st_start(void *o) { ((uint32_t *)o)[0] = 1; }
static void st_release(void *o, int slot) { ((uint32_t *)o)[0] = 10; releases++; rel_slot = slot; }

/* full level until released, then 2 blocks of release, then done */
static int st_env(void *o, int16_t *env, const int16_t *adsr, const int16_t *vel)
{
    uint32_t *st = o;
    int i;

    (void)adsr;
    (void)vel;
    env_calls++;
    if (quiet_tail && st[0] >= 10) {                                    /* a long tail at -64 dB */
        for (i = 0; i < 128; i++)
            env[i] = 20;
        return 0;
    }
    for (i = 0; i < 128; i++)
        env[i] = st[0] < 12 ? 32767 : 0;
    if (st[0] >= 10)
        st[0]++;
    return st[0] >= 12;
}

static const py_syn_api api = { st_render, st_key_up, st_start, st_release, st_env };

static void test_decimator(void)
{
    static int32_t hist[62], in[128], out[64];
    int b, i;
    double peak = 0;

    memset(hist, 0, sizeof(hist));
    for (b = 0; b < 4; b++) {
        for (i = 0; i < 128; i++)
            in[i] = 10000;
        py_syn_decimate(hist, in, out);
    }
    CHECK(out[63] >= 9997 && out[63] <= 9999);
    memset(hist, 0, sizeof(hist));
    for (b = 0; b < 20; b++) {                                          /* 1 kHz passes */
        for (i = 0; i < 128; i++)
            in[i] = (int32_t)lround(10000 * sin(2 * M_PI * 1000 * (b * 128 + i) / 88200.0));
        py_syn_decimate(hist, in, out);
        if (b > 2)
            for (i = 0; i < 64; i++)
                peak = fmax(peak, fabs((double)out[i]));
    }
    CHECK(peak > 9900 && peak < 10100);
    memset(hist, 0, sizeof(hist));
    peak = 0;
    for (b = 0; b < 20; b++) {                                          /* 30 kHz is gone */
        for (i = 0; i < 128; i++)
            in[i] = (int32_t)lround(10000 * sin(2 * M_PI * 30000 * (b * 128 + i) / 88200.0));
        py_syn_decimate(hist, in, out);
        if (b > 2)
            for (i = 0; i < 64; i++)
                peak = fmax(peak, fabs((double)out[i]));
    }
    CHECK(peak < 100);
    /* the 32-bit form = the 64-bit sum, bit for bit (random input up to +-2^29, history carried) */
    {
        static const int16_t h[16] = { -1, 2, -6, 13, -27, 50, -88, 145, -231, 355, -532, 790, -1184, 1856, -3334, 10382 };
        static int32_t z[62 + 128];
        uint32_t r = 12345;
        int bad = 0, n, j, amp;

        memset(hist, 0, sizeof(hist));
        memset(z, 0, sizeof(z));
        for (b = 0; b < 200; b++) {
            amp = b % 3 == 0 ? 29 : b % 3 == 1 ? 18 : 10;
            for (i = 0; i < 128; i++) {
                r = r * 1664525u + 1013904223u;
                in[i] = (int32_t)(r >> (32 - amp - 1)) - (1 << amp);
            }
            memmove(z, z + 128, 62 * sizeof(int32_t));
            memcpy(z + 62, in, sizeof(in));
            py_syn_decimate(hist, in, out);
            for (n = 0; n < 64; n++) {
                const int32_t *x = z + 2 * n;
                long long acc = (long long)x[31] * 16384;

                for (j = 0; j < 16; j++)
                    acc += (long long)h[j] * ((long long)x[2 * j] + x[62 - 2 * j]);
                bad += out[n] != (int32_t)(acc >> 15);
            }
        }
        CHECK(bad == 0);
    }
}

static uint32_t tblk;                                                   /* block number */

static uint32_t blk(py_syn *s, int inst, int slot, int32_t gain, int32_t *bus, int32_t *out)
{
    static const int16_t knobs[8], adsr[8];

    memset(bus, 0, 128 * sizeof(int32_t));
    memset(out, 0, 64 * sizeof(int32_t));
    return py_syn_block(s, &api, inst, knobs, adsr, slot, gain, bus, out, tblk++);
}

int main(void)
{
    static py_syn s;
    static int32_t bus[128], out[64], hist[62];
    uint32_t pk;
    int b, i, ok;

    CHECK(py_syn_inc(69) == PY_INC_A4 && py_syn_inc(81) == 2 * PY_INC_A4 && py_syn_inc(57) == PY_INC_A4 / 2);
    CHECK(py_syn_inc(200) == py_syn_inc(127));
    test_decimator();

    memset(&s, 0, sizeof(s));
    CHECK(py_syn_tail(&s));                                             /* nothing sounds */
    py_syn_note(&s, 81, 100, 100);                                      /* held 100 samples */
    CHECK(!py_syn_tail(&s));                                            /* a note waits to start */
    blk(&s, 2, 5, 32767, bus, out);
    CHECK(renders == 1 && last_inst == 2 && last_v == 0 && last_flags == (PY_SYN_FRESH | PY_SYN_BLOCK)
          && last_inc == 2 * PY_INC_A4 && !releases && s.v[0].vel == (100 << 8));
    CHECK(!py_syn_tail(&s));                                            /* held */
    seq = 0;
    blk(&s, 2, 5, 32767, bus, out);                                     /* 36 left: release now */
    CHECK(last_flags == PY_SYN_BLOCK && releases == 1 && rel_slot == 5 && py_syn_active(&s) == 1);
    /* the engine's note-off (TE's voices) once, before this block's render; the render got the
     * block's envelope (rendered first: TE's dsynth reads it) */
    CHECK(key_ups == 1 && key_up_v == 0 && !key_up_after_render && last_env0 == 32767);
    CHECK(py_syn_tail(&s));                                             /* only its release sounds */
    py_syn_note(&s, 60, 100, 1000);
    CHECK(!py_syn_tail(&s));                                            /* a tail and a new note */
    s.v[1].on = 0;
    blk(&s, 2, 5, 32767, bus, out);
    blk(&s, 2, 5, 32767, bus, out);                                     /* the envelope is done */
    CHECK(!py_syn_active(&s) && key_ups == 1);
    /* a note shorter than a block: note_on (the FRESH render) first, then its note-off */
    memset(&s, 0, sizeof(s));
    key_ups = 0;
    seq = 0;
    py_syn_note(&s, 60, 100, 10);
    blk(&s, 2, 5, 32767, bus, out);
    CHECK(key_ups == 1 && key_up_after_render);
    /* restart (a new engine on the instance): held notes start again (FRESH), released ones end */
    memset(&s, 0, sizeof(s));
    s.cap = 4;
    py_syn_note(&s, 60, 100, 100000);
    py_syn_note(&s, 62, 100, 10);
    blk(&s, 2, 5, 32767, bus, out);
    py_syn_restart(&s);
    CHECK(s.v[0].on && !s.v[0].started && !s.v[1].on);
    blk(&s, 2, 5, 32767, bus, out);
    CHECK(last_flags & PY_SYN_FRESH);

    /* the output: (1000 x 32767) >> 15 = 999 at 88.2 kHz, gain 1/2 -> 499, into the 88.2 kHz bus;
     * peak after the gain; nothing at 44.1 kHz */
    memset(&s, 0, sizeof(s));
    py_syn_note(&s, 60, 127, 100000);
    pk = blk(&s, 1, 0, 16384, bus, out);
    ok = 1;
    for (i = 0; i < 128; i++)
        ok &= bus[i] == 499;
    for (i = 0; i < 64; i++)
        ok &= out[i] == 0;
    CHECK(ok && pk == 499);
    /* decimated by the caller: DC gain 0.99988 */
    memset(hist, 0, sizeof(hist));
    for (b = 0; b < 4; b++) {
        blk(&s, 1, 0, 32767, bus, out);
        py_syn_decimate(hist, bus, out);
    }
    CHECK(out[63] >= 996 && out[63] <= 998);
    /* two voices: the new note renders first (FRESH + BLOCK), the old one after it (no flags); they sum */
    s.cap = 4;
    py_syn_note(&s, 64, 100, 100000);
    renders = 0;
    blk(&s, 1, 0, 32767, bus, out);
    CHECK(renders == 2 && first_v == 1 && first_flags == (PY_SYN_FRESH | PY_SYN_BLOCK) && last_v == 0
          && last_flags == 0 && bus[0] == 1997);              /* (2 x 999 x 32767) >> 15 */

    /* stealing: 4 voices busy, a fifth note takes the oldest (voice 0) */
    py_syn_note(&s, 65, 100, 100000);
    py_syn_note(&s, 66, 100, 100000);
    py_syn_note(&s, 67, 100, 100000);
    CHECK(py_syn_active(&s) == 4 && s.v[0].inc == py_syn_inc(67));

    /* cap 2 (and 0 = PY_SPOLY in py_synth: PLAY sets the default): only voices 0 and 1 */
    memset(&s, 0, sizeof(s));
    s.cap = 2;
    py_syn_note(&s, 60, 100, 100000);
    py_syn_note(&s, 62, 100, 100000);
    py_syn_note(&s, 64, 100, 100000);
    CHECK(py_syn_active(&s) == 2 && s.v[0].inc == py_syn_inc(64) && s.v[1].inc == py_syn_inc(62));
    s.cap = 1;                                                          /* mono: always voice 0 */
    py_syn_note(&s, 67, 100, 100000);
    CHECK(s.v[0].inc == py_syn_inc(67) && !s.v[2].on);

    /* a released voice below -60 dB ends, without waiting for the envelope's own end */
    memset(&s, 0, sizeof(s));
    quiet_tail = 1;
    py_syn_note(&s, 60, 100, 10);
    blk(&s, 1, 0, 32767, bus, out);                                     /* released, tail at 20 */
    CHECK(!py_syn_active(&s));
    quiet_tail = 0;

    /* ECO: render every other block at twice the step, 2 envelope blocks, output at 44.1 kHz, the
     * engine told (HALF); only on blocks of the track's parity */
    memset(&s, 0, sizeof(s));
    s.eco = 1;
    s.par = 1;
    tblk = 0;
    py_syn_note(&s, 69, 100, 100000);
    renders = 0;
    pk = blk(&s, 1, 0, 32767, bus, out);                                /* block 0: not its parity */
    CHECK(renders == 0 && pk == 0 && s.phase == 0 && out[0] == 0 && py_syn_active(&s) == 1);
    renders = env_calls = 0;
    pk = blk(&s, 1, 0, 32767, bus, out);
    ok = 1;
    for (i = 0; i < 128; i++)
        ok &= bus[i] == 0;
    for (i = 0; i < 64; i++)
        ok &= out[i] == 998;
    CHECK(ok && pk == 998 && renders == 1 && env_calls == 2 && last_inc == 2 * PY_INC_A4 && s.phase == 1
          && last_flags == (PY_SYN_FRESH | PY_SYN_BLOCK | PY_SYN_HALF));
    pk = blk(&s, 1, 0, 32767, bus, out);                                /* the second half: no render */
    CHECK(renders == 1 && env_calls == 2 && out[0] == 998 && out[63] == 998 && pk == 998 && s.phase == 0);
    blk(&s, 1, 0, 32767, bus, out);
    CHECK(renders == 2 && !(last_flags & PY_SYN_BOOST));
    /* ECO boost (the caller's budget): the engine told (BOOST) with HALF; not without ECO */
    s.boost = 1;
    blk(&s, 1, 0, 32767, bus, out);                                     /* the second half */
    blk(&s, 1, 0, 32767, bus, out);
    CHECK(renders == 3 && last_flags == (PY_SYN_BLOCK | PY_SYN_HALF | PY_SYN_BOOST));
    s.eco = 0;
    s.phase = 0;
    blk(&s, 1, 0, 32767, bus, out);
    CHECK(renders == 4 && last_flags == PY_SYN_BLOCK);
    /* ECO hold: 100 samples = release in the first envelope block of the second pair */
    memset(&s, 0, sizeof(s));
    s.eco = 1;
    tblk = 0;
    releases = 0;
    py_syn_note(&s, 69, 100, 100);
    blk(&s, 1, 0, 32767, bus, out);
    CHECK(releases == 1);

    /* no engine for the sound: voices stop, nothing added */
    memset(&s, 0, sizeof(s));
    no_engine = 1;
    py_syn_note(&s, 60, 100, 1000);
    pk = blk(&s, 1, 0, 32767, bus, out);
    ok = 1;
    for (i = 0; i < 128; i++)
        ok &= bus[i] == 0;
    CHECK(ok && !pk && !py_syn_active(&s));

    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
