/* host tests for dr_dsp.c: cc -std=c99 test_dr_dsp.c dr_dsp.c -lm && ./a.out */
#include <math.h>
#include <stdio.h>
#include "dr_dsp.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

/* RMS of a 12000-amplitude sine at f through the filter, after 0.1 s of settling */
static double rms(int type, int cut, int res, double f)
{
    dr_voice v;
    dr_coef c;
    int16_t buf[128];
    double ph = 0, acc = 0;
    int b, i, m = 0;

    dr_coef_set(&c, type, cut, res);
    dr_voice_start(&v, 0, DR_UNITY, -1);
    for (b = 0; b < 140; b++) {
        for (i = 0; i < 128; i++) {
            buf[i] = (int16_t)(12000 * sin(ph));
            ph += 2 * M_PI * f / DR_VOICE_RATE;
        }
        dr_process(&v, buf, 128, &c);
        if (b >= 70)
            for (i = 0; i < 128; i++, m++)
                acc += (double)buf[i] * buf[i];
    }
    return sqrt(acc / m);
}

int main(void)
{
    const double in = 12000 / sqrt(2.0);
    int16_t buf[128];
    dr_voice v;
    dr_coef c;
    int i, ok;

    /* cut 72 ~ 1 kHz */
    CHECK(rms(DR_F_LP, 72, 0, 100) > in * 0.9);
    CHECK(rms(DR_F_LP, 72, 0, 10000) < in * 0.05);
    CHECK(rms(DR_F_HP, 72, 0, 100) < in * 0.05);
    CHECK(rms(DR_F_HP, 72, 0, 10000) > in * 0.9);
    CHECK(rms(DR_F_BP, 72, 64, 1000) > rms(DR_F_BP, 72, 64, 100) * 4);
    CHECK(rms(DR_F_BP, 72, 64, 1000) > rms(DR_F_BP, 72, 64, 10000) * 4);
    /* resonance lifts the cutoff */
    CHECK(rms(DR_F_LP, 72, 120, 1000) > rms(DR_F_LP, 72, 0, 1000) * 3);
    /* open LP passes 10 kHz, closed passes almost nothing at 1 kHz; never blows up */
    CHECK(rms(DR_F_LP, 127, 0, 10000) > in * 0.75);        /* Q 0.5 at half the cutoff: 0.8 */
    CHECK(rms(DR_F_LP, 0, 0, 1000) < in * 0.002);
    CHECK(rms(DR_F_LP, 20, 0, 1000) < in * 0.006);       /* 60 Hz: -24 dB at 1 kHz */
    CHECK(rms(DR_F_LP, 72, 127, 1000) < 40000);

    /* OFF + unity: untouched */
    dr_coef_set(&c, DR_F_OFF, 10, 10);
    dr_voice_start(&v, 3, DR_UNITY, -1);
    for (i = 0; i < 128; i++)
        buf[i] = (int16_t)(i * 200 - 12000);
    dr_process(&v, buf, 128, &c);
    for (i = 0, ok = 1; i < 128; i++)
        ok &= buf[i] == (int16_t)(i * 200 - 12000);
    CHECK(ok && v.key == 3);
    dr_process(&v, buf, 128, 0);
    CHECK(buf[5] == -11000);

    /* gain half, gate 300 then 256-sample fade, then silence */
    dr_voice_start(&v, 0, DR_UNITY / 2, 300);
    {
        int16_t out[128 * 6];
        int b;

        for (b = 0; b < 6; b++) {
            for (i = 0; i < 128; i++)
                buf[i] = 10000;
            dr_process(&v, buf, 128, 0);
            for (i = 0; i < 128; i++)
                out[b * 128 + i] = buf[i];
        }
        CHECK(out[0] == 5000 && out[299] == 5000);
        CHECK(out[300] == 5000 && out[301] < 5000 && out[300 + 128] > 2400 && out[300 + 128] < 2600);
        CHECK(out[300 + 255] > 0 && out[300 + 255] < 40 && out[300 + 256] == 0 && out[767] == 0);
        for (i = 301, ok = 1; i < 300 + 256; i++)
            ok &= out[i] <= out[i - 1];
        CHECK(ok);
    }
    /* a new note opens the gate again and clears the filter */
    dr_coef_set(&c, DR_F_LP, 72, 0);
    dr_voice_start(&v, 1, DR_UNITY, -1);
    for (i = 0; i < 128; i++)
        buf[i] = 10000;
    dr_process(&v, buf, 128, &c);
    CHECK(v.ic2 != 0);
    dr_voice_start(&v, 1, DR_UNITY, -1);
    CHECK(v.ic1 == 0 && v.ic2 == 0 && v.gate == -1);
    /* idle voice: silence stays silence, states settle to 0 */
    for (i = 0; i < 128; i++)
        buf[i] = 0;
    dr_process(&v, buf, 128, &c);
    for (i = 0, ok = 1; i < 128; i++)
        ok &= buf[i] == 0;
    CHECK(ok && v.ic1 == 0 && v.ic2 == 0);

    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
