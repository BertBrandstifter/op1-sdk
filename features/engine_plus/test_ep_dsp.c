/* Host unit tests for the engine layer DSP (plain cc): bypass is bit-exact, each stage does what
 * its name says, nothing overflows, block size does not change the result. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ep_dsp.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define N 4410                                  /* 100 ms */

static int16_t buf[N], ref[N];

static void sine(double hz, double amp)
{
    int i;
    for (i = 0; i < N; i++)
        buf[i] = (int16_t)lround(amp * 32767 * sin(2 * M_PI * hz * i / 44100));
}

/* run in 128-sample blocks like the device */
static void run(const int16_t p[4])
{
    ep_voice v;
    int i;

    ep_reset(&v);
    for (i = 0; i < N; i += 128)
        ep_process(&v, buf + i, N - i < 128 ? N - i : 128, p);
}

/* RMS of the second half (after the filter settled), relative to full scale */
static double rms(void)
{
    double s = 0;
    int i;
    for (i = N / 2; i < N; i++)
        s += (double)buf[i] * buf[i];
    return sqrt(s / (N / 2)) / 32767;
}

int main(void)
{
    const int16_t off[4] = { 0, 0, 0, 0 };
    int i, ok;
    double in;

    /* bypass: bit-exact, including full-scale and odd values */
    for (i = 0; i < N; i++)
        buf[i] = (int16_t)((i * 7919) ^ (i << 5));
    memcpy(ref, buf, sizeof(buf));
    run(off);
    CHECK(memcmp(ref, buf, sizeof(buf)) == 0);

    /* drive: quiet signal gets louder, loud one stays in range and is compressed */
    sine(220, 0.1);
    in = rms();
    run((const int16_t[]){ 32767, 0, 0, 0 });
    CHECK(rms() > 4 * in);
    sine(220, 1.0);
    run((const int16_t[]){ 32767, 0, 0, 0 });
    CHECK(rms() > 0.75 && rms() <= 1.0);        /* towards a square, bounded */
    sine(220, 0.5);
    in = rms();
    run((const int16_t[]){ 300, 0, 0, 0 });     /* little drive: almost unchanged */
    CHECK(fabs(rms() - in) < 0.1 * in);

    /* filter at the middle (fc ~1.1 kHz): 200 Hz passes, 8 kHz is cut */
    sine(200, 0.5);
    in = rms();
    run((const int16_t[]){ 0, 16384, 0, 0 });
    CHECK(rms() > 0.8 * in);
    sine(8000, 0.5);
    run((const int16_t[]){ 0, 16384, 0, 0 });
    CHECK(rms() < 0.05 * in);
    /* filter fully closed (~70 Hz): 1 kHz is cut hard */
    sine(1000, 0.5);
    run((const int16_t[]){ 0, 32767, 0, 0 });
    CHECK(rms() < 0.02 * in);

    /* resonance boosts a sine at the cutoff, stays bounded */
    sine(1125, 0.2);
    in = rms();
    run((const int16_t[]){ 0, 16384, 0, 0 });
    {
        double flat = rms();
        sine(1125, 0.2);
        run((const int16_t[]){ 0, 16384, 32767, 0 });
        CHECK(rms() > 3 * flat);
    }
    sine(1125, 1.0);                            /* worst case: full scale at the peak */
    run((const int16_t[]){ 32767, 16384, 32767, 0 });
    ok = 1;
    for (i = 0; i < N; i++)
        ok &= buf[i] >= -32768 && buf[i] <= 32767;
    CHECK(ok && rms() > 0.3);

    /* crush at max: 13 low bits dropped, each value held 16 samples */
    sine(440, 0.8);
    run((const int16_t[]){ 0, 0, 0, 32767 });
    ok = 1;
    for (i = 0; i < N; i++)
        ok &= (buf[i] & 0x1fff) == 0 && (i % 16 == 0 || buf[i] == buf[i - 1]);
    CHECK(ok);

    /* block size independence: 128-sample blocks == one long call */
    {
        static const int16_t p[4] = { 20000, 12000, 25000, 9000 };
        static int16_t one[N];
        ep_voice v;

        sine(330, 0.7);
        memcpy(one, buf, sizeof(one));
        ep_reset(&v);
        ep_process(&v, one, N, p);
        run(p);                                 /* buf: same input, block-wise */
        CHECK(memcmp(one, buf, sizeof(one)) == 0);
    }

    printf("%d/%d checks passed\n%s\n", checks - fails, checks, fails ? "FAIL" : "PASS");
    return fails != 0;
}
