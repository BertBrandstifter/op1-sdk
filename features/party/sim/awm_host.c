/* AWM host check (run_awmtest.py): pss_core.c built with the data gen_pss11.py --pss-rom made from a
 * synthetic GEW7 ROM (-DPSS_DATA_INC). Renders the ROM's sounds and prints what it measures, one
 * "name value" per line; run_awmtest.py compares them with what the ROM holds. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../pss_core.h"

#define SR      PSS_SR
#define N       (SR * 2)
static int16_t buf[N];
static uint32_t incs[128];

static int16_t zk(int z, int n) { return (int16_t)(((2 * z + 1) * 32768L) / (2 * n)); }

/* sound w alone at key for N samples, filter open */
static void render(int w, int key)
{
    static pss_voice v;
    int16_t p[PSS_PARAMS];
    int i, k;

    memset(p, 0, sizeof(p));
    memset(&v, 0, sizeof(v));
    p[PSS_WAVE_A] = zk(w, pss_nwaves());
    p[PSS_WAVE_B] = zk(0, pss_nwaves() + 1);
    p[PSS_CUTOFF] = 32767;
    p[PSS_BPITCH] = zk(24, 49);
    for (k = 0; k < 128; k++)
        incs[k] = pss_inc_of_key(key);
    pss_note_on(&v, incs[0], p);
    for (i = 0; i < N; i += 128)
        pss_render(&v, buf + i, 128, incs, p);
}

/* rising zero crossings (interpolated) from sample `from`: mean frequency, and the cents range and
 * rate of its modulation (each period against the mean) */
static void pitch(int from, double *hz, double *lo, double *hi, double *mod_hz)
{
    static double t[20000];
    double c, prev = 0;
    int i, n = 0, turns = 0, dir = 0;

    for (i = from + 1; i < N && n < 20000; i++)
        if (buf[i - 1] < 0 && buf[i] >= 0)
            t[n++] = i - 1 + (double)-buf[i - 1] / (buf[i] - buf[i - 1]);
    *hz = n > 1 ? (n - 1) * SR / (t[n - 1] - t[0]) : 0;
    *lo = 1e9;
    *hi = -1e9;
    for (i = 1; i < n; i++) {
        c = 1200 * log2(SR / (t[i] - t[i - 1]) / *hz);
        *lo = c < *lo ? c : *lo;
        *hi = c > *hi ? c : *hi;
        if (i > 1 && (c - prev) * dir < 0 && fabs(c) > 20)
            turns++;
        if (c != prev)
            dir = c > prev ? 1 : -1;
        prev = c;
    }
    *mod_hz = n > 1 ? turns / 2.0 / ((t[n - 1] - t[0]) / SR) : 0;
}

/* RMS in dB per 2 ms from `from`: range (max - min) */
static double level_range(int from)
{
    double lo = 1e9, hi = -1e9, e;
    int i, j, w = SR / 500;

    for (i = from; i + w <= N; i += w) {
        for (e = 0, j = 0; j < w; j++)
            e += (double)buf[i + j] * buf[i + j];
        e = 10 * log10(e / w + 1e-9);
        lo = e < lo ? e : lo;
        hi = e > hi ? e : hi;
    }
    return hi - lo;
}

int main(void)
{
    double hz, lo, hi, mod;
    int w;
    long e = 0;

    printf("nwaves %d\n", pss_nwaves());
    for (w = 0; w < pss_nwaves(); w++)
        printf("name%d %s\n", w, pss_wave_name(w));
    render(0, 69);                              /* S000: 8-bit, period 50 -> A4 */
    pitch(SR / 10, &hz, &lo, &hi, &mod);
    printf("s0_a4_hz %.3f\n", hz);
    render(0, 48);
    pitch(SR / 10, &hz, &lo, &hi, &mod);
    printf("s0_c3_hz %.3f\n", hz);
    render(0, 113);                             /* step 3.17: read sample by sample (osc_gew) */
    pitch(SR / 10, &hz, &lo, &hi, &mod);
    printf("s0_f8_hz %.3f\n", hz);
    render(1, 69);                              /* S001: 12-bit, period 41.667, vibrato 7 at 7.066 Hz */
    pitch(SR / 10, &hz, &lo, &hi, &mod);
    printf("s1_a4_hz %.3f\ns1_vib_lo %.2f\ns1_vib_hi %.2f\ns1_vib_hz %.3f\n", hz, lo, hi, mod);
    render(2, 69);                              /* S002: 12-bit reversed start, tremolo 7 at 3.196 Hz */
    pitch(SR / 10, &hz, &lo, &hi, &mod);
    printf("s2_a4_hz %.3f\ns2_trem_db %.2f\n", hz, level_range(SR / 10));
    render(3, 60);                              /* S003: noise, unpitched */
    for (w = 0; w < N; w++)
        e += buf[w] < 0 ? -buf[w] : buf[w];
    printf("s3_mean_abs %ld\n", e / N);
    return 0;
}
