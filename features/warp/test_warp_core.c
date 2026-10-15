/* Host tests for the WARP voice core (plain cc, built with warp_core.c by tools/test_all.py). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "warp_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SR      WP_SR
#define N       (SR / 2)
static int16_t buf[N];
static uint32_t incs[128];

static uint32_t inc_hz(double hz) { return (uint32_t)(hz / SR * 4294967296.0); }
static int16_t zk(int z, int n) { return (int16_t)(((2 * z + 1) * 32768L) / (2 * n)); }

static void params(int16_t *p, int wave, int mode, int amount)
{
    memset(p, 0, WP_PARAMS * sizeof(*p));
    p[WP_WAVE] = zk(wave, 8);
    p[WP_MODE] = zk(mode, WP_MODES);
    p[WP_AMOUNT] = (int16_t)amount;
}

static void render(wp_voice *v, double hz, const int16_t *p, int n)
{
    int i, k;

    memset(v, 0, sizeof(*v));
    wp_note_on(v);
    for (k = 0; k < 128; k++)
        incs[k] = inc_hz(hz);
    for (i = 0; i < n; i += 128)
        wp_render(v, buf + i, n - i < 128 ? n - i : 128, incs, p);
}

static double amp(double hz, int from, int len)
{
    double w = 2 * M_PI * hz / SR, c = 2 * cos(w), s0, s1 = 0, s2 = 0, re, im;
    int i;

    for (i = 0; i < len; i++) {
        double win = 0.5 - 0.5 * cos(2 * M_PI * i / (len - 1));

        s0 = buf[from + i] * win + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    re = s1 - s2 * cos(w);
    im = s2 * sin(w);
    return sqrt(re * re + im * im) / (len * 0.25);
}

/* sum of harmonics 2..10 relative to the fundamental: brightness */
static double bright(double hz)
{
    double f = amp(hz, 4410, 16384), h = 0;
    int k;

    for (k = 2; k <= 10; k++)
        h += amp(hz * k, 4410, 16384);
    return h / (f + 1);
}

static int peak(void)
{
    int i, m = 0;

    for (i = 0; i < N; i++)
        m = abs(buf[i]) > m ? abs(buf[i]) : m;
    return m;
}

int main(void)
{
    static int16_t ref[N];
    wp_voice v;
    int16_t p[WP_PARAMS], cyc[64];
    int mode, wave, i, same;

    CHECK(wp_wave_of(0) == 0 && wp_wave_of(32767) == 7 && wp_mode_of(32767) == WP_MODES - 1);
    CHECK(!strcmp(wp_mode_names[WP_FOLD], "FOLD") && !strcmp(wp_wave_names[2], "SAW"));

    /* SINE, no warp: a clean sine at pitch */
    params(p, 0, WP_BEND, 0);
    render(&v, 440, p, N);
    printf("sine 440: %.0f, h2 %.1f dB, h3 %.1f dB, peak %d\n", amp(440, 4410, 16384),
           20 * log10(amp(880, 4410, 16384) / amp(440, 4410, 16384)),
           20 * log10(amp(1320, 4410, 16384) / amp(440, 4410, 16384)), peak());
    CHECK(amp(440, 4410, 16384) > 23000 && amp(880, 4410, 16384) < 100 && amp(1320, 4410, 16384) < 100);
    memcpy(ref, buf, sizeof(buf));

    /* amount 0 = the plain wave in every mode (within interpolation rounding) */
    for (mode = 0; mode < WP_MODES; mode++) {
        int maxd = 0;

        params(p, 0, mode, 0);
        render(&v, 440, p, N);
        for (i = 0; i < N; i++)
            maxd = abs(buf[i] - ref[i]) > maxd ? abs(buf[i] - ref[i]) : maxd;
        if (maxd > 600)
            printf("mode %s at 0: max diff %d\n", wp_mode_names[mode], maxd);
        CHECK(maxd <= 600);
    }

    /* every mode at full amount: bounded, still sounding, brighter than the sine; phase warps
     * keep the pitch (fundamental present) */
    for (mode = 0; mode < WP_MODES; mode++) {
        params(p, 0, mode, 32767);
        render(&v, 220, p, N);
        printf("%-8s full: fundamental %6.0f  brightness %5.2f  peak %d\n", wp_mode_names[mode],
               amp(220, 4410, 16384), bright(220), peak());
        CHECK(peak() <= 32768 && peak() > 2000);
        CHECK(bright(220) > 0.05);
        if (mode != WP_HARM && mode != WP_RING && mode != WP_GAP && mode != WP_BITREV)  /* by design */
            CHECK(amp(220, 4410, 16384) > 1000);
    }

    /* every wave renders, different spectra */
    for (wave = 0, same = 0; wave < 8; wave++) {
        params(p, wave, WP_BEND, 0);
        render(&v, 220, p, N);
        CHECK(peak() > 15000 && peak() <= 32767);
        if (wave && !memcmp(buf, ref, sizeof(buf)))
            same++;
    }
    CHECK(!same);

    /* SWEEP: brighter at the start than after the decay */
    params(p, 2, WP_BEND, 0);
    p[WP_SWEEP] = 32767;
    p[WP_DECAY] = 8000;
    params(p, 0, WP_SYNC, 0);
    p[WP_SWEEP] = 32767;
    p[WP_DECAY] = 6000;
    render(&v, 220, p, N);
    {
        double early = amp(440, 0, 2048) + amp(660, 0, 2048) + amp(880, 0, 2048);
        double late = amp(440, N - 8192, 8192) + amp(660, N - 8192, 8192) + amp(880, N - 8192, 8192);

        printf("sweep: harmonics early %.0f late %.0f\n", early, late);
        CHECK(early > late * 3);
    }

    /* MOTION: the spectrum moves (brightness differs between two windows) */
    params(p, 0, WP_FOLD, 12000);
    p[WP_MOTION] = 32767;
    render(&v, 220, p, N);
    {
        double b1 = amp(660, 4410, 4096), b2 = amp(660, 4410 + 8820, 4096), b3 = amp(660, 4410 + 17640, 4096);

        printf("motion: h3 %.0f %.0f %.0f\n", b1, b2, b3);
        CHECK(fabs(b1 - b2) > 300 || fabs(b2 - b3) > 300);
    }

    /* UNISON: beating (level swings), COLOR: darker */
    params(p, 2, WP_BEND, 0);
    p[WP_UNISON] = 32767;
    render(&v, 440, p, N);
    CHECK(peak() > 10000);
    params(p, 2, WP_BEND, 0);
    render(&v, 440, p, N);
    {
        double open = amp(4400, 4410, 16384);

        p[WP_COLOR] = 32767;
        render(&v, 440, p, N);
        printf("color: 10th harmonic open %.0f, closed %.0f\n", open, amp(4400, 4410, 16384));
        CHECK(amp(4400, 4410, 16384) < open * 0.3);
    }

    /* screen cycle, repeatable notes, extremes */
    wp_cycle(2, WP_SYNC, 32767, cyc, 64);
    CHECK(cyc[0] != cyc[32]);
    params(p, 6, WP_FM, 20000);
    p[WP_SWEEP] = 10000;
    render(&v, 330, p, 4096);
    memcpy(ref, buf, 4096 * sizeof(int16_t));
    render(&v, 330, p, 4096);
    CHECK(!memcmp(ref, buf, 4096 * sizeof(int16_t)));
    for (i = 0; i < WP_PARAMS; i++)
        p[i] = 32767;
    render(&v, 15000, p, N);
    render(&v, 20, p, N);
    for (i = 0; i < 4; i++)
        p[i] = (int16_t)-32768;
    render(&v, 440, p, N);
    CHECK(peak() > 1000);

    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails != 0;
}
