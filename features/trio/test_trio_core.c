/* Host tests for the TRIO voice core (plain cc, built with trio_core.c by tools/test_all.py). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "trio_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SR      TR_SR
#define N       (SR / 2)
static int16_t buf[N];
static uint32_t incs[128];

static uint32_t inc_hz(double hz) { return (uint32_t)(hz / SR * 4294967296.0); }
static int16_t zk(int z, int n) { return (int16_t)(((2 * z + 1) * 32768L) / (2 * n)); }
static int16_t semis(int s) { return zk(s + 24, 49); }

/* open LP filter, intervals at unison, nothing else */
static void params(int16_t *p, int set)
{
    memset(p, 0, TR_PARAMS * sizeof(*p));
    p[TR_WAVE] = zk(set, TR_SETS);
    p[TR_CUTOFF] = 32767;
    p[TR_INT2] = p[TR_INT3] = semis(0);
}

static void render(tr_voice *v, double hz, const int16_t *p, int n)
{
    int i, k;

    memset(v, 0, sizeof(*v));
    tr_note_on(v);
    for (k = 0; k < 128; k++)
        incs[k] = inc_hz(hz);
    for (i = 0; i < n; i += 128)
        tr_render(v, buf + i, n - i < 128 ? n - i : 128, incs, p);
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

static int peak(int from, int to)
{
    int i, m = 0;

    for (i = from; i < to; i++)
        m = abs(buf[i]) > m ? abs(buf[i]) : m;
    return m;
}

/* brightness: harmonics 5..12 of hz relative to the fundamental */
static double bright(double hz, int from, int len)
{
    double h = 0;
    int k;

    for (k = 5; k <= 12; k++)
        h += amp(hz * k, from, len);
    return h / (amp(hz, from, len) + 1);
}

int main(void)
{
    tr_voice v;
    int16_t p[TR_PARAMS];
    int set, i;
    double a, b;

    CHECK(tr_set_of(0) == 0 && tr_set_of(32767) == 15 && tr_semis_of(semis(0)) == 0
          && tr_semis_of(0) == -24 && tr_semis_of(32767) == 24 && tr_mode_of(32767) == 3);
    CHECK(!strcmp(tr_set_names[13], "RING") && !strcmp(tr_mode_names[3], "NOTCH"));

    /* SAW3 at unison, open filter: a saw at pitch (harmonic 2 at ~half the fundamental) */
    params(p, 0);
    render(&v, 220, p, N);
    a = amp(220, 4410, 16384);
    b = amp(440, 4410, 16384);
    printf("saw3 220: f %.0f h2 %.0f peak %d\n", a, b, peak(4410, N));
    CHECK(a > 8000 && b > a * 0.35 && b < a * 0.65);

    /* every set sounds, bounded */
    for (set = 0; set < TR_SETS; set++) {
        params(p, set);
        render(&v, 220, p, N);
        printf("%-10s peak %5d  f0 %6.0f  bright %6.2f\n", tr_set_names[set], peak(4410, N),
               amp(220, 4410, 16384), bright(220, 4410, 16384));
        CHECK(peak(4410, N) > 3000 && peak(4410, N) < 32767);     /* no clipping */
    }
    /* open LP is stable: a triangle stays duller than a saw */
    params(p, 2);
    render(&v, 220, p, N);
    a = bright(220, 4410, 16384);
    params(p, 0);
    render(&v, 220, p, N);
    CHECK(a < bright(220, 4410, 16384) * 0.5);

    /* intervals: INT2 +12 on SAW+PLS puts energy an octave up; DETUNE beats */
    params(p, 3);
    p[TR_INT2] = semis(12);
    render(&v, 220, p, N);
    CHECK(amp(440, 4410, 16384) > amp(220, 4410, 16384) * 0.6);
    params(p, 0);
    p[TR_DETUNE] = 32767;
    render(&v, 220, p, N);
    {
        int lo = 1 << 30, hi = 0, w;

        for (w = 4410; w + 2205 <= N; w += 2205) {
            int pk = peak(w, w + 2205);

            lo = pk < lo ? pk : lo;
            hi = pk > hi ? pk : hi;
        }
        printf("detune: window peaks %d .. %d\n", lo, hi);
        CHECK(hi > lo * 1.3);
    }

    /* SYNC: the synced osc's pitch knob changes the timbre, not the pitch (fundamental stays) */
    params(p, 10);
    p[TR_INT2] = semis(7);
    render(&v, 220, p, N);
    a = amp(220, 4410, 16384);
    CHECK(a > 1500);

    /* RING: no energy at the plain fundamental-only position; differs from plain TRI */
    params(p, 13);
    p[TR_INT3] = semis(7);
    render(&v, 220, p, N);
    b = amp(220, 4410, 16384);
    params(p, 2);
    render(&v, 220, p, N);
    printf("ring vs tri: f0 %.0f vs %.0f\n", b, amp(220, 4410, 16384));
    CHECK(b < amp(220, 4410, 16384) * 0.7);

    /* filter: LP closes (darker), resonance peaks near the cutoff, HP removes the fundamental */
    params(p, 0);
    p[TR_CUTOFF] = 32767;
    render(&v, 110, p, N);
    a = bright(110, 4410, 16384);
    p[TR_CUTOFF] = 16000;
    render(&v, 110, p, N);
    b = bright(110, 4410, 16384);
    printf("lp open %.3f, closed %.3f\n", a, b);
    CHECK(b < a * 0.5);
    p[TR_RES] = 32767;
    render(&v, 110, p, N);
    CHECK(peak(4410, N) <= 32768);
    p[TR_RES] = 0;
    p[TR_CUTOFF] = 14000;
    p[TR_MODE] = zk(2, 4);                     /* HP */
    render(&v, 110, p, N);
    CHECK(amp(110, 4410, 16384) < 2000);

    /* filter envelope: bright at the start, darker after the decay */
    params(p, 0);
    p[TR_CUTOFF] = 6000;
    p[TR_FENV] = 32767;
    p[TR_FDECAY] = 8000;
    render(&v, 110, p, N);
    a = bright(110, 0, 4096);
    b = bright(110, N - 8192, 8192);
    printf("fenv: early %.3f late %.3f\n", a, b);
    CHECK(a > b * 3);

    /* PW: narrow pulse = more harmonics relative to the fundamental */
    params(p, 1);
    render(&v, 220, p, N);
    a = amp(440, 4410, 16384);                  /* 50 %: (almost) no even harmonics */
    p[TR_PW] = 24000;
    render(&v, 220, p, N);
    CHECK(amp(440, 4410, 16384) > a * 3 + 500);

    /* drive, noise, extremes */
    params(p, 2);
    render(&v, 220, p, N);
    a = bright(220, 4410, 16384);
    p[TR_DRIVE] = 32767;
    render(&v, 220, p, N);
    printf("drive: brightness %.3f -> %.3f\n", a, bright(220, 4410, 16384));
    CHECK(peak(0, N) <= 32768 && bright(220, 4410, 16384) > a * 1.5);
    p[TR_NOISE] = 32767;
    render(&v, 220, p, N);
    CHECK(peak(0, N) <= 32768);
    for (i = 0; i < TR_PARAMS; i++)
        p[i] = 32767;
    render(&v, 12000, p, N);
    render(&v, 20, p, N);
    for (i = 0; i < 4; i++)
        p[i] = (int16_t)-32768;
    render(&v, 440, p, N);
    CHECK(peak(0, N) <= 32768);

    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails != 0;
}
