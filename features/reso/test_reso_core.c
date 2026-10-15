/* Host tests for the RESO effect core (plain cc, built with reso_core.c by tools/test_all.py). */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "reso_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SR      44100
#define BLOCK   64                              /* TE's FX block */
static rs_state st;
static int16_t buf[SR * 4];

static uint32_t inc_hz(double hz) { return (uint32_t)(hz / 88200.0 * 4294967296.0); }   /* voice rate */
static double per_hz(uint32_t q16) { return SR * 65536.0 / q16; }
static int near(double a, double b, double rel) { return fabs(a - b) <= b * rel; }

static void run(int16_t *b, int n, const int16_t *p)
{
    int i;

    for (i = 0; i < n; i += BLOCK)
        rs_process(&st, b + i, n - i < BLOCK ? n - i : BLOCK, p);
}

/* strongest autocorrelation lag in [lo, hi] */
static int best_lag(const int16_t *b, int n, int lo, int hi)
{
    int lag, i, best = lo;
    double bv = -1e30;

    for (lag = lo; lag <= hi; lag++) {
        double acc = 0;

        for (i = 0; i + lag < n; i++)
            acc += (double)b[i] * b[i + lag];
        if (acc > bv) {
            bv = acc;
            best = lag;
        }
    }
    return best;
}

/* buf = a sine note of hz for n samples, then silence (4 s total) */
static void note(double hz, int n)
{
    int i;

    for (i = 0; i < SR * 4; i++)
        buf[i] = (int16_t)(i < n ? 8000 * sin(2 * 3.14159265358979 * hz * i / SR) : 0);
}

int main(void)
{
    int16_t p[RS_PARAMS];
    uint32_t inc[6] = { 0 };
    uint8_t act[6] = { 0 };
    int i, n, ok;

    memset(p, 0, sizeof(p));
    p[RS_OCTAVE] = 16384;                       /* 0 octaves */

    /* voicing knob covers all eight */
    CHECK(rs_voicing(0) == 0 && rs_voicing(32767) == RS_CHORDS - 1 && rs_voicing(4096) == 1);

    /* PLAYED: strings take the sounding notes (lowest first); 220 Hz -> period 200.45 samples */
    rs_reset(&st);
    inc[0] = inc_hz(330); act[0] = 1;
    inc[3] = inc_hz(220); act[3] = 1;
    n = rs_chord(&st, inc, act, 6, p);
    CHECK(n == 2);
    CHECK(near(per_hz(st.target_q16[0]), 220, 0.001) && near(per_hz(st.target_q16[1]), 330, 0.001));
    CHECK(near(per_hz(st.target_q16[2]), 440, 0.001) && near(per_hz(st.target_q16[3]), 660, 0.001));
    CHECK(st.delay_q16[0] == st.target_q16[0]); /* first tuning: no glide from nowhere */

    /* nothing sounding: strings keep their notes */
    act[0] = act[3] = 0;
    CHECK(rs_chord(&st, inc, act, 6, p) == 0 && near(per_hz(st.target_q16[0]), 220, 0.001));

    /* MAJ voicing on the lowest note: root, +4, +7, +12 */
    act[0] = act[3] = 1;
    p[RS_CHORD] = (int16_t)(3 * 4096 + 100);
    rs_chord(&st, inc, act, 6, p);
    CHECK(near(per_hz(st.target_q16[0]), 220, 0.001) && near(per_hz(st.target_q16[1]), 277.18, 0.002)
          && near(per_hz(st.target_q16[2]), 329.63, 0.002) && near(per_hz(st.target_q16[3]), 440, 0.001));

    /* OCTAVE +2 and -2; extreme notes fold into the strings' range */
    p[RS_OCTAVE] = 32767;
    rs_chord(&st, inc, act, 6, p);
    CHECK(near(per_hz(st.target_q16[0]), 880, 0.001));
    p[RS_OCTAVE] = 0;
    rs_chord(&st, inc, act, 6, p);
    CHECK(near(per_hz(st.target_q16[0]), 55, 0.001));
    inc[3] = inc_hz(8); inc[0] = inc_hz(12000);
    rs_chord(&st, inc, act, 6, p);
    for (i = 0, ok = 1; i < RS_STRINGS; i++)
        ok &= st.target_q16[i] >= (8u << 16) && st.target_q16[i] <= ((uint32_t)(RS_LEN - 4) << 16);
    CHECK(ok);
    p[RS_OCTAVE] = 16384;
    p[RS_CHORD] = 0;

    /* DETUNE spreads strings 2-4 by up to 25 cents, string 1 stays */
    inc[3] = inc_hz(220); inc[0] = inc_hz(330);
    act[0] = 0;
    p[RS_DETUNE] = 32767;
    rs_chord(&st, inc, act, 6, p);
    CHECK(near(per_hz(st.target_q16[0]), 220, 0.001) && near(per_hz(st.target_q16[1]), 440 * 1.0145, 0.003)
          && near(per_hz(st.target_q16[2]), 880 / 1.0145, 0.003));
    p[RS_DETUNE] = 0;

    /* MIX 0: dry, bit-exact */
    for (i = 0; i < 4096; i++)
        buf[i] = (int16_t)(i * 37 % 20000 - 10000);
    p[RS_MIX] = 0;
    rs_reset(&st);
    run(buf, 4096, p);
    for (i = 0, ok = 1; i < 4096; i++)
        ok &= buf[i] == (int16_t)(i * 37 % 20000 - 10000);
    CHECK(ok);

    /* a note at the strings' pitch, then silence: the strings ring on at their note (strings 2-4
     * are octaves of it, so the strongest lag is the 220 Hz period, 200.45 samples) */
    {
        int lag, peak = 0;

        rs_reset(&st);
        act[3] = 1;
        rs_chord(&st, inc, act, 6, p);
        p[RS_MIX] = 32767;
        p[RS_DECAY] = 24000;
        p[RS_TONE] = 32767;
        note(220, SR / 2);
        run(buf, SR * 2, p);
        lag = best_lag(buf + SR, 8000, 150, 260);
        CHECK(lag >= 199 && lag <= 202);
        for (i = SR + SR / 2; i < SR + SR / 2 + 4000; i++)
            peak = buf[i] > peak ? buf[i] : peak;
        CHECK(peak > 3000);                     /* still ringing 1 s after the note */
    }

    /* short DECAY dies away; long rings on */
    {
        int32_t tail_short = 0, tail_long = 0;

        p[RS_DECAY] = 0;
        rs_reset(&st);
        rs_chord(&st, inc, act, 6, p);
        note(220, SR / 2);
        run(buf, SR * 2, p);
        for (i = SR; i < SR + 2000; i++)
            tail_short += buf[i] < 0 ? -buf[i] : buf[i];
        p[RS_DECAY] = 32767;
        rs_reset(&st);
        rs_chord(&st, inc, act, 6, p);
        note(220, SR / 2);
        run(buf, SR * 2, p);
        for (i = SR; i < SR + 2000; i++)
            tail_long += buf[i] < 0 ? -buf[i] : buf[i];
        CHECK(tail_short * 100 < tail_long && tail_long > 2000 * 3000);
    }

    /* stability: full-scale noise into the longest, brightest, driven strings for 4 s stays
     * bounded, and after the input stops a short decay brings it back to silence */
    {
        uint32_t r = 1;
        int lo = 0, hi = 0;

        for (i = 0; i < RS_PARAMS; i++)
            p[i] = 32767;
        p[RS_GLIDE] = 0;
        rs_reset(&st);
        rs_chord(&st, inc, act, 6, p);
        for (i = 0; i < SR * 4; i++) {
            r = r * 1664525u + 1013904223u;
            buf[i] = (int16_t)(r >> 16);
        }
        run(buf, SR * 4, p);
        p[RS_DECAY] = 0;
        p[RS_MIX] = 32767;
        memset(buf, 0, sizeof(buf));
        run(buf, SR, p);
        for (i = SR / 2; i < SR; i++) {
            lo = buf[i] < lo ? buf[i] : lo;
            hi = buf[i] > hi ? buf[i] : hi;
        }
        CHECK(lo > -50 && hi < 50);
    }

    /* DC in: no build-up (blocked before the strings) */
    {
        int peak = 0;

        memset(p, 0, sizeof(p));
        p[RS_OCTAVE] = 16384;
        p[RS_MIX] = 32767;
        p[RS_DECAY] = 32767;
        p[RS_TONE] = 32767;
        rs_reset(&st);
        rs_chord(&st, inc, act, 6, p);
        for (i = 0; i < SR * 2; i++)
            buf[i] = 8000;
        run(buf, SR * 2, p);
        for (i = SR; i < SR * 2; i++)
            peak = buf[i] > peak ? buf[i] : -buf[i] > peak ? -buf[i] : peak;
        CHECK(peak < 1000);
    }

    /* GLIDE: a new chord is reached gradually, not in one block */
    {
        uint32_t before, after1;

        p[RS_GLIDE] = 20000;
        rs_reset(&st);
        inc[3] = inc_hz(220);
        rs_chord(&st, inc, act, 6, p);
        before = st.delay_q16[0];
        inc[3] = inc_hz(440);
        rs_chord(&st, inc, act, 6, p);
        memset(buf, 0, 2 * BLOCK);
        rs_process(&st, buf, BLOCK, p);
        after1 = st.delay_q16[0];
        CHECK(after1 < before && after1 > st.target_q16[0] + (5u << 16));
        for (i = 0; i < 4000; i++)
            rs_process(&st, buf, BLOCK, p);
        CHECK(st.delay_q16[0] - st.target_q16[0] < (1u << 16));
        p[RS_GLIDE] = 0;
    }

    printf("%d/%d checks passed\n%s\n", checks - fails, checks, fails ? "FAIL" : "PASS");
    return fails != 0;
}
