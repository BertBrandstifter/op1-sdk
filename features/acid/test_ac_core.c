/* Host unit tests for ac_core.c (cc -std=c99 test_ac_core.c ac_core.c). Every parameter is swept
 * through its range and its effect measured (it may only move one way); levels, pitch, slide,
 * accent stacking, stability at full resonance. Exit 0 = pass. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ac_core.h"

#define N 128
static int fails;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        fails++;
    }
}

static ac_params base(void)
{
    ac_params p;

    memset(&p, 0, sizeof(p));
    p.cutoff = 110; p.res = 150; p.envmod = 120; p.decay = 110;
    p.accent = 40; p.accdec = 6; p.sweep = AC_SWEEP_NORM; p.speed = 1;
    p.wave = AC_SAW; p.sub = 0; p.tune = 0; p.slide = 8; p.soft = 4;
    p.track = 0; p.fm = 0; p.drive = 0; p.muff = AC_MUFF_OFF; p.level = 10;
    return p;
}

static uint32_t inc_of(double hz) { return (uint32_t)(hz / 88200.0 * 4294967296.0); }

/* render ms milliseconds at hz into buf (doubles), returns samples */
static int render(ac_voice *v, const ac_params *p, double hz, double ms, double *buf)
{
    uint32_t inc[N];
    int16_t out[N];
    int i, b, n = 0, blocks = (int)(ms * 88.2 / N + 0.5);

    for (i = 0; i < N; i++)
        inc[i] = inc_of(hz);
    for (b = 0; b < blocks; b++) {
        ac_render(v, out, N, inc, p);
        for (i = 0; i < N; i++)
            buf[n++] = out[i];
    }
    return n;
}

static double rms(const double *x, int a, int b)
{
    double s = 0;
    int i;

    for (i = a; i < b; i++)
        s += x[i] * x[i];
    return sqrt(s / (b - a > 0 ? b - a : 1));
}

static double peak(const double *x, int a, int b)
{
    double m = 0;
    int i;

    for (i = a; i < b; i++)
        m = fabs(x[i]) > m ? fabs(x[i]) : m;
    return m;
}

/* brightness: rms of the first difference / rms */
static double bright(const double *x, int a, int b)
{
    double s = 0, r = rms(x, a, b);
    int i;

    for (i = a + 1; i < b; i++)
        s += (x[i] - x[i - 1]) * (x[i] - x[i - 1]);
    return r > 1 ? sqrt(s / (b - a - 1)) / r : 0;
}

/* Goertzel magnitude at hz */
static double goertzel(const double *x, int a, int b, double hz)
{
    double w = 2 * M_PI * hz / 88200, c = 2 * cos(w), s1 = 0, s2 = 0, s0;
    int i;

    for (i = a; i < b; i++) {
        s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / (b - a);
}

/* pitch by rising zero crossings with hysteresis */
static double pitch(const double *x, int a, int b)
{
    int i, n = 0, first = -1, last = -1, up = 0;

    for (i = a; i < b; i++) {
        if (!up && x[i] > 800) {
            up = 1;
            if (first < 0)
                first = i;
            last = i;
            n++;
        } else if (up && x[i] < -800) {
            up = 0;
        }
    }
    return n > 1 ? (n - 1) * 88200.0 / (last - first) : 0;
}

static double buf[88200 * 3];

/* one fresh note: measure fn over [a_ms, b_ms) */
typedef double (*meas_fn)(const double *, int, int);
static double note_meas(const ac_params *p, double hz, int accent, double a_ms, double b_ms, meas_fn f)
{
    ac_voice v;

    ac_init(&v);
    ac_note(&v, 0, accent);
    render(&v, p, hz, b_ms + 3, buf);
    return f(buf, (int)(a_ms * 88.2), (int)(b_ms * 88.2));
}

/* a parameter swept, the measure must move one way (dir +1 up, -1 down) by at least min_ratio overall */
static void sweep(const char *name, int lo, int hi, int steps, void (*set)(ac_params *, int), double hz,
                  int accent, double a_ms, double b_ms, meas_fn f, int dir, double min_ratio)
{
    double prev = 0, first = 0, m;
    int i, ok = 1;
    char what[160];

    for (i = 0; i < steps; i++) {
        ac_params p = base();
        int val = lo + (hi - lo) * i / (steps - 1);

        set(&p, val);
        m = note_meas(&p, hz, accent, a_ms, b_ms, f);
        if (i == 0)
            first = m;
        else if ((m - prev) * dir < -1e-3 * fabs(prev) - 1e-9) {
            snprintf(what, sizeof(what), "%s: not monotonic at %d (%.4f after %.4f)", name, val, m, prev);
            check(0, what);
            ok = 0;
        }
        prev = m;
    }
    if (ok) {
        double r = dir > 0 ? prev / (first > 1e-9 ? first : 1e-9) : first / (prev > 1e-9 ? prev : 1e-9);

        snprintf(what, sizeof(what), "%s: range too small (x%.3f)", name, r);
        check(r >= min_ratio, what);
        printf("  %-12s %8.4f .. %8.4f\n", name, first, prev);
    }
}

static void s_cutoff(ac_params *p, int v) { p->cutoff = v; p->envmod = 0; }
static void s_envmod(ac_params *p, int v) { p->envmod = v; p->cutoff = 60; }
static void s_decay(ac_params *p, int v) { p->decay = v; p->envmod = 200; p->cutoff = 40; }
static void s_accent(ac_params *p, int v) { p->accent = v; }
static void s_accdec(ac_params *p, int v) { p->accdec = v; p->envmod = 120; p->accent = 63; p->sweep = AC_SWEEP_OFF; }
static void s_sub(ac_params *p, int v) { p->sub = v; }
static void s_soft(ac_params *p, int v) { p->soft = v; p->envmod = 255; p->cutoff = 30; p->res = 0; p->decay = 255; }   /* rc1: the filter envelope's attack */
static void s_track(ac_params *p, int v) { p->track = v; p->envmod = 0; p->cutoff = 60; p->res = 0; }
static void s_drive(ac_params *p, int v) { p->drive = v; p->envmod = 0; p->cutoff = 90; p->res = 0; }
static void s_level(ac_params *p, int v) { p->level = v; }
static void s_fm(ac_params *p, int v) { p->fm = v; p->envmod = 0; p->cutoff = 70; p->res = 60; }
static void s_muff(ac_params *p, int v) { p->muff = v; p->envmod = 0; p->cutoff = 200; p->res = 0; p->drive = 20; }
static void s_res(ac_params *p, int v) { p->res = v; p->envmod = 0; p->cutoff = 140; p->decay = 0; }

static double m_sub(const double *x, int a, int b) { return goertzel(x, a, b, 55.0); }
static double m_crest(const double *x, int a, int b) { return peak(x, a, b) / rms(x, a, b); }
/* resonance: the peak harmonic (CUTOFF 140 = 958 Hz, DECAY short: the envelope gone by 100 ms) against
 * the fundamental */
static double m_reso(const double *x, int a, int b)
{
    double m = 0, g;
    int h;

    for (h = 4; h <= 12; h++)                   /* the resonant peak (wherever it sits) / the fundamental */
        m = (g = goertzel(x, a, b, 110.0 * h)) > m ? g : m;
    return m / goertzel(x, a, b, 110.0);
}
/* the 3rd harmonic against the fundamental of a 440 Hz note (cutoff position) */
static double m_h3(const double *x, int a, int b) { return goertzel(x, a, b, 1320.0) / goertzel(x, a, b, 440.0); }
static double m_h3abs(const double *x, int a, int b) { return goertzel(x, a, b, 1320.0); }

int main(void)
{
    ac_params p;
    ac_voice v;
    double f, a, b, c;
    int n, i;

    printf("sweeps (110 Hz saw):\n");
    sweep("CUTOFF", 0, 255, 18, s_cutoff, 110, 0, 50, 150, bright, +1, 3.0);
    sweep("RESONANCE", 0, 255, 12, s_res, 110, 0, 100, 200, m_reso, +1, 3.0);
    sweep("ENV MOD", 0, 255, 12, s_envmod, 110, 0, 5, 40, bright, +1, 2.0);
    sweep("DECAY", 0, 255, 12, s_decay, 110, 0, 250, 350, bright, +1, 1.5);
    sweep("ACCENT", 0, 63, 10, s_accent, 110, 1, 2, 40, rms, +1, 1.4);
    sweep("ACC DECAY", 0, 31, 10, s_accdec, 110, 1, 250, 350, bright, +1, 1.5);
    sweep("SUB", 0, 3, 4, s_sub, 110, 0, 50, 150, m_sub, +1, 5.0);
    sweep("SOFT ATK", 0, 31, 10, s_soft, 440, 0, 1, 11, m_h3abs, -1, 1.3);
    sweep("TRACKING", 0, 31, 10, s_track, 440, 0, 50, 150, m_h3, +1, 2.0);
    sweep("OVERDRIVE", 0, 31, 10, s_drive, 110, 0, 50, 150, rms, +1, 1.5);
    sweep("MUFFLER", 0, 2, 3, s_muff, 110, 0, 50, 150, m_crest, -1, 1.1);
    sweep("LEVEL", 0, 15, 16, s_level, 110, 0, 50, 150, rms, +1, 10.0);

    /* filter FM: the sound moves away from FM 0 the more FM there is */
    {
        static double ref[88200];
        double prev = 0, d;
        int k, j, m;

        for (k = 0; k <= 31; k += 31 / 6) {
            p = base();
            s_fm(&p, k);
            ac_init(&v);
            ac_note(&v, 0, 0);
            m = render(&v, &p, 110, 150, buf);
            if (!k)
                memcpy(ref, buf, sizeof(double) * m);
            for (d = 0, j = 50 * 88; j < m; j++)
                d += (buf[j] - ref[j]) * (buf[j] - ref[j]);
            d = sqrt(d / (m - 50 * 88)) / rms(ref, 50 * 88, m);
            if (k)
                check(d > prev, "FILTER FM moves the sound more and more");
            prev = d;
        }
        printf("  FILTER FM    difference at 31: %.3f\n", prev);
        check(prev > 0.3, "FILTER FM range");
    }

    /* level: a default saw note peaks in TE's range (~12k), not clipping */
    p = base();
    a = note_meas(&p, 110, 0, 20, 200, peak);
    printf("default peak %.0f\n", a);
    check(a > 7000 && a < 20000, "default level");

    /* pitch: 110 Hz, TUNE +12 doubles, -12 halves; square the same */
    p = base();
    p.envmod = 0; p.cutoff = 255; p.res = 0;
    f = note_meas(&p, 110, 0, 20, 400, pitch);
    printf("pitch %.2f Hz\n", f);
    check(fabs(f - 110) < 1.0, "pitch 110 Hz");
    for (i = -12; i <= 12; i += 6) {
        p.tune = (int8_t)i;
        f = note_meas(&p, 110, 0, 20, 400, pitch);
        check(fabs(f - 110 * pow(2, i / 12.0)) < 1.5, "TUNE semitones");
    }
    p.tune = 0;
    p.wave = AC_SQUARE;
    f = note_meas(&p, 110, 0, 20, 400, pitch);
    check(fabs(f - 110) < 1.0, "square pitch");

    /* slide: a slid note reaches the new pitch slower with a longer SLIDE; envelope not retriggered */
    {
        double fs[2];
        int k;

        for (k = 0; k < 2; k++) {
            p = base();
            p.envmod = 0; p.cutoff = 255; p.res = 0;
            p.slide = (uint8_t)(k ? 31 : 0);
            ac_init(&v);
            ac_note(&v, 0, 0);
            render(&v, &p, 110, 200, buf);
            ac_note(&v, 1, 0);
            n = render(&v, &p, 220, 60, buf);
            fs[k] = pitch(buf, 10 * 88, 40 * 88);
            (void)n;
        }
        printf("slide: 10-40 ms after: %.1f Hz (10 ms) %.1f Hz (360 ms)\n", fs[0], fs[1]);
        check(fs[0] > 200 && fs[1] < 180 && fs[1] > 112, "SLIDE time");
        /* fully there after a while */
        n = render(&v, &p, 220, 1500, buf);
        f = pitch(buf, n - 300 * 88, n);
        check(fabs(f - 220) < 2.0, "slide arrives");
    }
    {
        double br[2];
        int k;

        for (k = 0; k < 2; k++) {               /* retriggered vs slid: brightness right after */
            p = base();
            p.envmod = 220; p.cutoff = 30; p.decay = 60;
            ac_init(&v);
            ac_note(&v, 0, 0);
            render(&v, &p, 110, 600, buf);
            ac_note(&v, k, 0);
            render(&v, &p, 110, 40, buf);
            br[k] = bright(buf, 5 * 88, 35 * 88);
        }
        printf("retrigger %.4f, slid %.4f\n", br[0], br[1]);
        check(br[0] > 1.5 * br[1], "slide does not retrigger the envelope");
    }

    /* accent sweep: quick accents stack with NORM (the 4th brighter than the 1st), not with OFF */
    {
        double r[3];
        int sw;

        for (sw = 0; sw < 3; sw++) {
            double b1 = 0, b4 = 0;

            p = base();
            p.sweep = (uint8_t)sw; p.accent = 63; p.envmod = 60; p.cutoff = 40; p.accdec = 8; p.speed = 0;
            ac_init(&v);
            for (i = 0; i < 4; i++) {
                ac_note(&v, 0, 1);
                render(&v, &p, 110, 90, buf);
                if (i == 0)
                    b1 = bright(buf, 10 * 88, 60 * 88);
                b4 = bright(buf, 10 * 88, 60 * 88);
            }
            r[sw] = b4 / b1;
        }
        printf("accent stacking 4th/1st: OFF %.3f NORM %.3f HIGH %.3f\n", r[0], r[1], r[2]);
        check(r[0] < 1.07 && r[1] > r[0] * 1.05 && r[2] > r[1], "accent sweep stacks (SLOW: NORM, HIGH)");
    }
    /* sweep speed: right after an accent the sweep circuit has charged more when FAST */
    {
        double r[3];
        int sp;

        for (sp = 0; sp < 3; sp++) {
            p = base();
            p.sweep = AC_SWEEP_NORM; p.accent = 63; p.envmod = 0; p.cutoff = 40; p.speed = (uint8_t)sp;
            r[sp] = note_meas(&p, 110, 1, 3, 15, bright);
        }
        printf("sweep speed: %.4f %.4f %.4f\n", r[0], r[1], r[2]);
        check(r[0] < r[1] && r[1] < r[2], "SPEED");
    }

    /* stability: full resonance (and HIGH accents) at every cutoff stays bounded, keeps sounding */
    for (i = 0; i <= 255; i += 15) {
        int k;

        for (k = 0; k < 2; k++) {
            p = base();
            p.res = 255; p.cutoff = i; p.envmod = 255; p.sweep = AC_SWEEP_HIGH; p.accent = 63;
            p.drive = (uint8_t)(k ? 31 : 0); p.fm = (uint8_t)(k ? 31 : 0); p.track = 31;
            ac_init(&v);
            ac_note(&v, 0, 1);
            n = render(&v, &p, k ? 880 : 55, 400, buf);
            a = rms(buf, n - 100 * 88, n);
            b = peak(buf, 0, n);
            check(b <= 32767, "bounded");
            check(a > 50, "keeps sounding at full resonance");
            c = fabs(rms(buf, n - 100 * 88, n - 50 * 88) - rms(buf, n - 50 * 88, n));
            (void)c;
        }
    }
    /* no note: silence */
    ac_init(&v);
    p = base();
    n = render(&v, &p, 110, 20, buf);
    check(peak(buf, 0, n) == 0, "silent before a note");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
