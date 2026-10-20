/* Host tests for the MONO voice core (plain cc, built with mo_core.c by tools/test_all.py). The
 * expected values are the reference's. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mo_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SR      MO_SR
#define N       (SR * 2)
static int16_t buf[N];
static uint32_t incs[128];

static uint32_t inc_hz(double hz) { return (uint32_t)(hz / SR * 4294967296.0); }

/* osc 1 saw at 8', the others silent, filter open, no emphasis / contour / glide, sustains full */
static void base(mo_params *p)
{
    memset(p, 0, sizeof(*p));
    p->cutoff = 32767;
    p->range[0] = p->range[1] = p->range[2] = 3;
    p->wave[0] = p->wave[1] = p->wave[2] = MO_SAW;
    p->freq[0] = p->freq[1] = MO_UNISON;
    p->vol[0] = 15;
    p->fsus = p->lsus = 15;
}

static void play(mo_voice *v, double hz, const mo_params *p, int from, int n)
{
    int i, k;

    for (k = 0; k < 128; k++)
        incs[k] = inc_hz(hz);
    for (i = from; i < from + n; i += 128)
        mo_render(v, buf + i, from + n - i < 128 ? from + n - i : 128, incs, p);
}

static void note(mo_voice *v, double hz, const mo_params *p, int n)
{
    mo_voice_init(v);
    mo_note(v, inc_hz(hz), p);
    play(v, hz, p, 0, n);
}

static double amp(double hz, int from, int len)  /* Goertzel, Hann window */
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
    return sqrt(re * re + im * im) / len;
}

static double rms(int from, int len)
{
    double s = 0;
    int i;

    for (i = from; i < from + len; i++)
        s += (double)buf[i] * buf[i];
    return sqrt(s / len);
}

/* fundamental from rising zero crossings (interpolated, with hysteresis) */
static double hz_of(int from, int to)
{
    double first = -1, last = 0;
    int i, n = 0, armed = 0;

    for (i = from; i < to - 1; i++) {
        if (buf[i] < -300)
            armed = 1;
        if (armed && buf[i] < 0 && buf[i + 1] >= 0) {
            double t = i + (double)-buf[i] / (buf[i + 1] - buf[i]);

            armed = 0;
            if (first < 0)
                first = t;
            else
                n++;
            last = t;
        }
    }
    return n && last > first ? n / ((last - first) / SR) : 0;
}

static double cents(double a, double b) { return 1200 * log2(a / b); }

/* the strongest frequency in [lo, hi] (Goertzel on a grid), over buf[from .. from + len) */
static double peak_hz(double lo, double hi, double step, int from, int len)
{
    double f, best = 0, at = lo;

    for (f = lo; f <= hi; f += step) {
        double a = amp(f, from, len);

        if (a > best) {
            best = a;
            at = f;
        }
    }
    return at;
}

static void test_math(void)
{
    int ok = 1;
    double x;

    for (x = 9e5; x < 2e9; x *= 1.37) {          /* increments: 20 Hz .. below 2^31 */
        double l = mo_log2((uint32_t)x) / 65536.0;

        ok &= fabs(l - log2(x)) < 0.0005;
        ok &= fabs(cents(mo_exp2((int32_t)(log2(x) * 65536)), x)) < 0.5;
    }
    CHECK(ok);
}

/* every wave at its pitch; every range an octave apart (8' = the pitch TE plays, LO = 64') */
static void test_pitch(void)
{
    mo_params p;
    mo_voice v;
    int w, r, ok = 1;

    base(&p);
    for (w = 0; w < MO_WAVES; w++) {
        p.wave[0] = (uint8_t)w;
        note(&v, 220, &p, SR / 2);
        ok &= fabs(cents(hz_of(SR / 10, SR / 2), 220)) < 3;
    }
    CHECK(ok);
    p.wave[0] = MO_TRI;
    for (r = 1; r < MO_RANGES; r++) {           /* 32' .. 2' (LO at 27.5 Hz: too few cycles here) */
        double want = 220 * pow(2, r - 3);

        p.range[0] = (uint8_t)r;
        note(&v, 220, &p, SR / 2);
        if (fabs(cents(hz_of(SR / 10, SR / 2), want)) >= 3)
            printf("  range %d: %.1f Hz, want %.1f\n", r, hz_of(SR / 10, SR / 2), want);
        CHECK(fabs(cents(hz_of(SR / 10, SR / 2), want)) < 3);
    }
}

/* osc 2 / 3 frequency: the steps in cents (osc 2 alone; osc 3 alone) */
static void test_freq(void)
{
    static const int steps[] = { 0, 5, 9, 14, 15, 16, 21, 25, 30 };   /* -7 st .. -30 ct .. 0 .. +7 st */
    mo_params p;
    mo_voice v;
    unsigned int i;
    int ok = 1, o;

    for (o = 0; o < 2; o++)
        for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
            double want = 220 * pow(2, mo_cents(steps[i]) / 1200.0), got;

            base(&p);
            p.vol[0] = 0;
            p.vol[1 + o] = 15;
            p.wave[1 + o] = MO_TRI;
            p.freq[o] = (uint8_t)steps[i];
            note(&v, 220, &p, N);
            got = peak_hz(want - 3, want + 3, 0.05, SR / 4, N - SR / 4);
            ok &= fabs(cents(got, want)) < 1;
        }
    CHECK(ok);
    CHECK(mo_cents(MO_UNISON) == 0 && mo_cents(0) == -700 && mo_cents(MO_FREQS - 1) == 700);
}

static void test_blep(void)
{
    /* a saw at 3.7 kHz: harmonics alias below it; the band-limited one has far less there */
    mo_params p;
    mo_voice v;
    double alias = 0, h = 0;
    int k;

    base(&p);
    note(&v, 3700, &p, SR / 2);
    for (k = 1; k < 6; k++)
        h += amp(3700.0 * k, 8192, 16384);
    for (k = 0; k < 40; k++) {                  /* energy between the harmonics (aliases) */
        double f = 200 + k * 85.0;

        if (fmod(f, 3700) > 150 && fmod(f, 3700) < 3550)
            alias += amp(f, 8192, 16384);
    }
    CHECK(alias < h * 0.05);
}

/* the waves' shapes and levels as the reference's: saw harmonics 1/n, square odd only, wide pulse
 * (1/3) without the 3rd, narrow (0.23): 2nd at -2.5 dB; triangle 1.25x the saw's peak */
static void test_waves(void)
{
    mo_params p;
    mo_voice v;
    double h[4];
    int w, k;

    base(&p);
    for (w = MO_SAW; w <= MO_NARROW; w++) {
        p.wave[0] = (uint8_t)w;
        note(&v, 110, &p, SR);
        for (k = 0; k < 4; k++)
            h[k] = amp(110.0 * (k + 1), SR / 4, SR / 2);
        if (w == MO_SAW)
            CHECK(fabs(h[1] / h[0] - 0.5) < 0.03 && fabs(h[2] / h[0] - 0.333) < 0.03);
        if (w == MO_SQUARE)
            CHECK(h[1] < h[0] * 0.01 && fabs(h[2] / h[0] - 0.333) < 0.03);
        if (w == MO_WIDE)
            CHECK(h[2] < h[0] * 0.02 && fabs(h[1] / h[0] - 0.5) < 0.03);
        if (w == MO_NARROW)
            CHECK(fabs(20 * log10(h[1] / h[0]) + 2.5) < 0.5);
    }
    p.wave[0] = MO_SAW;
    note(&v, 110, &p, SR);
    h[0] = rms(SR / 4, SR / 2) * sqrt(3);       /* the saw's peak */
    p.wave[0] = MO_TRI;
    note(&v, 110, &p, SR);
    h[1] = rms(SR / 4, SR / 2) * sqrt(3);
    CHECK(fabs(h[1] / h[0] - 1.25) < 0.05);
}

/* self-oscillation alone (no oscillators): its pitch, from a note at key k */
static double selfosc(mo_params *p, int key)
{
    mo_voice v;

    p->vol[0] = 0;
    note(&v, 440 * pow(2, (key - 69) / 12.0), p, N);
    return hz_of(N / 2, N);
}

static void test_filter(void)
{
    static const double cuts[] = { 0.1, 0.3, 0.5, 0.7 };
    mo_params p;
    mo_voice v;
    double open, closed, lo, hi;
    unsigned int i;
    int ok = 1, c, bad = 0;

    base(&p);                                   /* 24 dB / oct */
    note(&v, 110, &p, SR / 2);
    open = amp(1100, 8192, 16384);
    p.cutoff = 13107;                           /* 0.4: 654 Hz */
    note(&v, 110, &p, SR / 2);
    closed = amp(2640, 8192, 16384) / amp(1320, 8192, 16384);
    CHECK(closed < 0.09 && closed > 0.04);      /* an octave: 1/16 (+ a little) */
    CHECK(amp(1100, 8192, 16384) < open * 0.3);
    for (i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {   /* the reference law: 40.9 Hz * 2^(10 c) */
        double want = 40.9 * pow(2, 10 * cuts[i]), f;

        base(&p);
        p.cutoff = (int32_t)(cuts[i] * 32767);
        p.emph = 32767;
        f = selfosc(&p, 41);
        if (fabs(cents(f, want)) > 20)
            printf("  self-oscillation at cutoff %.1f: %.1f Hz, want %.1f\n", cuts[i], f, want);
        ok &= fabs(cents(f, want)) <= 20;
        ok &= rms(N / 2, N / 2) > 6000;         /* loud: the reference's 0.44 rms */
    }
    CHECK(ok);
    base(&p);                                   /* threshold: 0.88 rings out, 0.95 oscillates */
    p.cutoff = 13107;
    p.emph = (int32_t)(0.88 * 32767);
    selfosc(&p, 41);
    CHECK(rms(N / 2, N / 2) < 100);
    p.emph = (int32_t)(0.95 * 32767);
    selfosc(&p, 41);
    CHECK(rms(N / 2, N / 2) > 3000);
    base(&p);                                   /* the bass loss: -10 dB at emphasis 0.5 (reference 9..10) */
    p.wave[0] = MO_TRI;
    p.cutoff = 16384;
    note(&v, 55, &p, SR);
    lo = amp(55, SR / 4, SR / 2);
    p.emph = 16384;
    note(&v, 55, &p, SR);
    hi = amp(55, SR / 4, SR / 2);
    CHECK(fabs(20 * log10(hi / lo) + 10) < 1.5);
    base(&p);                                   /* stable everywhere: all at full, emphasis full */
    p.vol[1] = p.vol[2] = p.vol[3] = 15;
    p.emph = 32767;
    for (c = 0; c <= 32767; c += 4096) {
        p.cutoff = c;
        note(&v, 55, &p, SR / 4);
        bad += !(rms(SR / 8, SR / 8) < 30000);
    }
    CHECK(!bad);
}

/* keyboard control 1/3, 2/3, full around F2 (key 41), (the reference's resonance runs
 * sharp of its own cutoff law above ~2.5 kHz: +29 ct at 2.6 kHz, +89 ct at 5.2 kHz; ours does not) */
static void test_kbd(void)
{
    static const double want[4][3] = {
        { 654.9, 654.9, 654.9 }, { 594.6, 946.0, 1507.7 }, { 539.8, 1367.9, 3517.7 }, { 490.1, 1981.8, 8897.0 },
    };
    static const int keys[3] = { 36, 60, 84 };
    mo_params p;
    int k, j, ok = 1;

    for (k = 0; k < MO_KBDS; k++)
        for (j = 0; j < 3; j++) {
            double f, tol = want[k][j] > 5000 ? 250 : want[k][j] > 2500 ? 60 : 30;

            base(&p);
            p.cutoff = 13107;
            p.emph = 32767;
            p.kbd = (uint8_t)k;
            f = selfosc(&p, keys[j]);
            if (fabs(cents(f, want[k][j])) > tol)
                printf("  kbd %d key %d: %.1f Hz, ref %.1f\n", k, keys[j], f, want[k][j]);
            ok &= fabs(cents(f, want[k][j])) <= tol;
        }
    CHECK(ok);
}

/* the contour adds Hz: 4950 Hz x amount^2 at its peak (cutoff 0.2 = 163.6 Hz + ...) */
static void test_contour_amount(void)
{
    static const double am[] = { 0.25, 0.5, 0.75 };
    mo_params p;
    unsigned int i;
    int ok = 1;

    for (i = 0; i < sizeof(am) / sizeof(am[0]); i++) {
        double want = 163.6 + 4950 * am[i] * am[i], f;

        base(&p);
        p.cutoff = (int32_t)(0.2 * 32767);
        p.emph = 32767;
        p.contour = (int32_t)(am[i] * 32767);
        f = selfosc(&p, 41);
        ok &= fabs(cents(f, want)) < 35;
    }
    CHECK(ok);
}

/* the loudness contour's shape: decay (1 - t/T)^3.3, attack 1 - (1 - t/T)^3.3 (+-8 %); release */
static double env_at(int from)                  /* level of a 1 kHz square around buf[from] */
{
    return rms(from, 441);
}

static void test_envelopes(void)
{
    static const int steps[] = { 14, 18, 22 };  /* 64 ms, 210 ms, 690 ms */
    mo_params p;
    mo_voice v;
    unsigned int i;
    int ok = 1;

    for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        double T = mo_time_10us(steps[i]) / 10000.0, full, l;
        int t;

        base(&p);
        p.wave[0] = MO_SQUARE;
        p.lsus = 0;
        p.ldec = (uint8_t)steps[i];
        note(&v, 1000, &p, N);
        full = env_at(0);
        for (t = 1; t < 4; t++) {               /* at T/4, T/2, 3T/4 */
            double want = pow(1 - t / 4.0, 3.3);

            l = env_at((int)(t / 4.0 * T * SR)) / full;
            ok &= fabs(l - want) < 0.08;
        }
        base(&p);
        p.wave[0] = MO_SQUARE;
        p.latt = (uint8_t)steps[i];
        note(&v, 1000, &p, N);
        full = env_at((int)(1.2 * T * SR));
        for (t = 1; t < 4; t++) {
            double want = 1 - pow(1 - t / 4.0, 3.3);

            l = env_at((int)(t / 4.0 * T * SR)) / full;
            ok &= fabs(l - want) < 0.08;
        }
    }
    CHECK(ok);
    base(&p);                                   /* release off: the key up changes nothing (TE's envelope
                                                 * ends the note); on: the decay time */
    p.wave[0] = MO_SQUARE;
    p.ldec = 22;
    note(&v, 1000, &p, SR / 4);
    mo_release(&v, &p);
    play(&v, 1000, &p, SR / 4, SR / 4);
    CHECK(env_at(SR / 4 + 2205) > env_at(SR / 4 - 882) * 0.9 && env_at(SR / 2 - 882) > env_at(SR / 4 - 882) * 0.9);
    p.release = 1;
    note(&v, 1000, &p, SR / 4);
    mo_release(&v, &p);
    play(&v, 1000, &p, SR / 4, SR / 4);
    CHECK(env_at(SR / 4 + 2205) > env_at(SR / 4 - 882) * 0.5);
    CHECK(env_at(SR / 2 - 882) < env_at(SR / 4 - 882) * 0.4);   /* ldec 22 = 0.69 s: ~0.23 at 0.24 s */
}

/* glide: constant time, linear in pitch: half way at T/2 for an octave and for two */
static void test_glide(void)
{
    mo_params p;
    mo_voice v;
    double T, f;
    int oct;

    base(&p);
    p.wave[0] = MO_TRI;
    p.glide = 12;
    T = mo_glide_ms(12) / 1000.0;
    for (oct = 1; oct <= 2; oct++) {
        double from = 440 / pow(2, oct);

        mo_voice_init(&v);
        mo_note(&v, inc_hz(from), &p);
        play(&v, from, &p, 0, SR / 4);
        mo_note(&v, inc_hz(440), &p);
        play(&v, 440, &p, 0, N);
        f = hz_of((int)(T / 2 * SR) - 2205, (int)(T / 2 * SR) + 2205);
        CHECK(fabs(cents(f, 440 / pow(2, oct / 2.0))) < 60);
        CHECK(fabs(cents(hz_of((int)(T * SR) + 4410, N), 440)) < 3);
    }
    p.glide = 0;                                /* off: jumps */
    mo_note(&v, inc_hz(220), &p);
    play(&v, 220, &p, 0, 8820);
    CHECK(fabs(cents(hz_of(0, 8820), 220)) < 5);
}

/* every note starts the same (the oscillators reset together) */
static void test_reset(void)
{
    static int16_t first[2048];
    mo_params p;
    mo_voice v;

    base(&p);
    p.vol[1] = 15;
    p.freq[0] = MO_UNISON + 3;
    note(&v, 220, &p, 2048);
    memcpy(first, buf, sizeof(first));
    play(&v, 220, &p, 0, 10000);
    mo_note(&v, inc_hz(220), &p);
    play(&v, 220, &p, 0, 2048);
    CHECK(rms(1024, 1024) > 1000);
    {
        int i, same = 1;

        for (i = 1024; i < 2048; i++)           /* after the attack: the same waveform */
            same &= abs(buf[i] - first[i]) < 200;
        CHECK(same);
    }
}

/* -- every parameter through its range: its effect moves one way only ----------------------- */

enum { Q_CUTOFF, Q_EMPH, Q_CONTOUR, Q_GLIDE, Q_RANGE, Q_FREQ, Q_VOL1, Q_VOL2, Q_VOL3, Q_NOISE,
       Q_FATT, Q_FDEC, Q_FSUS, Q_LATT, Q_LDEC, Q_LSUS, Q_KBD, Q_N };

/* white noise through the filter alone (the oscillators' phases do not matter): a note at 220 Hz
 * starts the contour; brightness = the mean level over 1.5 .. 3 kHz */
static void noise_through(mo_voice *v, const mo_params *p, int n, int key)
{
    static int32_t z[128];
    uint32_t r = 12345;
    int i, j;

    mo_voice_init(v);
    mo_note(v, inc_hz(220), p);
    for (i = 0; i < n; i += 128) {
        int m = n - i < 128 ? n - i : 128;

        for (j = 0; j < m; j++) {
            r ^= r << 13;
            r ^= r >> 17;
            r ^= r << 5;
            z[j] = (int32_t)(r >> 17) - 16384;
        }
        mo_filter(v, z, buf + i, m, key << 16, p);
    }
}

static double bright(int from, int len)
{
    double a = 0, f;

    for (f = 1500; f <= 3000; f += 100)
        a += amp(f, from, len) / 16;
    return a;
}

static int qmax(int q)
{
    switch (q) {
    case Q_CUTOFF: case Q_EMPH: case Q_CONTOUR: return 32767;
    case Q_GLIDE: return MO_GLIDES - 1;
    case Q_RANGE: return MO_RANGES - 1;
    case Q_FREQ: return MO_FREQS - 1;
    case Q_FATT: case Q_FDEC: case Q_LATT: case Q_LDEC: return MO_TIMES - 1;
    case Q_FSUS: case Q_LSUS: case Q_VOL1: case Q_VOL2: case Q_VOL3: case Q_NOISE: return 15;
    default: return MO_KBDS - 1;
    }
}

static double effect(int q, int val)
{
    mo_params p;
    mo_voice v;

    base(&p);
    switch (q) {
    case Q_CUTOFF:
        p.cutoff = val;
        noise_through(&v, &p, SR / 2, 41);
        return bright(8192, 16384);
    case Q_EMPH:                                /* the peak at the cutoff against the passband */
        {
            double pk = 0, pass = 0, f;
            int j;

            p.cutoff = 16384;                   /* 1.3 kHz */
            p.emph = val > 31000 ? 31000 : val; /* below self-oscillation */
            noise_through(&v, &p, N, 41);
            for (j = 0; j < 8; j++) {           /* 8 windows of 8192 (+-20 Hz lobes), a 10 Hz grid */
                double m = 0;

                for (f = 900; f < 1600; f += 10)
                    m = fmax(m, amp(f, 16384 + j * 16384, 8192));
                pk += m / 8;
                for (f = 200; f < 400; f += 10)
                    pass += amp(f, 16384 + j * 16384, 8192) / (8 * 20);
            }
            return pk / (pass + 0.01);
        }
    case Q_CONTOUR:
        p.cutoff = 9000;
        p.contour = val;
        p.fsus = 0;
        p.fdec = 20;
        noise_through(&v, &p, SR / 4, 41);
        return bright(0, 4096);
    case Q_GLIDE:                               /* how far from the new pitch after 50 ms */
        p.glide = (uint8_t)val;
        mo_voice_init(&v);
        mo_note(&v, inc_hz(220), &p);
        play(&v, 220, &p, 0, 4096);
        mo_note(&v, inc_hz(440), &p);
        play(&v, 440, &p, 0, 4410);
        return (double)labs((long)v.glide);
    case Q_RANGE:                               /* osc 1's pitch */
        p.wave[0] = MO_TRI;
        p.range[0] = (uint8_t)val;
        note(&v, 440, &p, SR / 2);
        return hz_of(SR / 4, SR / 2);
    case Q_FREQ:                                /* osc 2's pitch (alone) */
        p.vol[0] = 0;
        p.vol[1] = 15;
        p.wave[1] = MO_TRI;
        p.freq[0] = (uint8_t)val;
        note(&v, 220, &p, SR / 2);
        return hz_of(SR / 4, SR / 2);
    case Q_VOL1: case Q_VOL2: case Q_VOL3:      /* each oscillator's level (alone) */
        p.vol[0] = 0;
        p.vol[q - Q_VOL1] = (uint8_t)val;
        note(&v, 220, &p, SR / 4);
        return rms(8192, 8192);
    case Q_NOISE:
        p.vol[0] = 0;
        p.vol[3] = (uint8_t)val;
        note(&v, 220, &p, SR / 4);
        return rms(8192, 8192);
    case Q_FATT:                                /* filter still closed early: slower */
        p.cutoff = 9000;
        p.contour = 32767;
        p.fatt = (uint8_t)val;
        noise_through(&v, &p, SR / 4, 41);
        return -bright(0, 2048);
    case Q_FDEC:                                /* still open later */
        p.cutoff = 9000;
        p.contour = 32767;
        p.fsus = 0;
        p.fdec = (uint8_t)val;
        noise_through(&v, &p, SR / 2, 41);
        return bright(SR / 4, 16384);
    case Q_FSUS:
        p.cutoff = 9000;
        p.contour = 32767;
        p.fdec = 5;
        p.fsus = (uint8_t)val;
        noise_through(&v, &p, SR / 2, 41);
        return bright(SR / 4, 16384);
    case Q_LATT:                                /* quiet early: slower */
        p.latt = (uint8_t)val;
        note(&v, 220, &p, SR / 4);
        return -rms(0, 2048);
    case Q_LDEC:                                /* still loud later */
        p.lsus = 0;
        p.ldec = (uint8_t)val;
        note(&v, 220, &p, SR / 2);
        return rms(SR / 4, 8192);
    case Q_LSUS:
        p.ldec = 5;
        p.lsus = (uint8_t)val;
        note(&v, 220, &p, SR / 2);
        return rms(SR / 4, 8192);
    default:                                    /* KBD: the cutoff of a high key (filter alone) */
        p.cutoff = 9000;
        p.kbd = (uint8_t)val;
        noise_through(&v, &p, SR / 2, 84);
        return bright(8192, 16384);
    }
}

static void test_monotonic(void)
{
    static const char *const names[Q_N] = { "CUTOFF", "EMPHASIS", "CONTOUR", "GLIDE", "RANGE", "FREQ",
        "VOL 1", "VOL 2", "VOL 3", "NOISE", "F.ATTACK", "F.DECAY", "F.SUSTAIN", "L.ATTACK", "L.DECAY",
        "L.SUSTAIN", "KEYBOARD" };
    int q;

    for (q = 0; q < Q_N; q++) {
        int max = qmax(q), stepv = max > 64 ? max / 8 : 1, from = q == Q_CUTOFF ? 6000 : 0, v, bad = 0;
        double prev = effect(q, from), first = prev, e;
        double eps = q == Q_CUTOFF || q == Q_CONTOUR || q == Q_FATT || q == Q_FDEC || q == Q_FSUS || q == Q_KBD ? 1.0 : 0;

        if (q == Q_GLIDE) {                     /* more glide = further away (0 = off = there) */
            prev = first = -effect(q, 0) - 1;
        }
        for (v = from + stepv; v <= max; v += stepv) {
            e = effect(q, v);
            bad += e < prev - 0.02 * fabs(prev) - eps - 1e-9;
            prev = e;
        }
        e = effect(q, max);
        if (bad || !(e > first))
            printf("  %s: not monotonic (%d steps back, %.1f -> %.1f)\n", names[q], bad, first, e);
        CHECK(!bad && e > first);
    }
}

int main(void)
{
    test_math();
    test_pitch();
    test_freq();
    test_blep();
    test_waves();
    test_filter();
    test_kbd();
    test_contour_amount();
    test_envelopes();
    test_glide();
    test_reset();
    test_monotonic();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "OK", checks, fails);
    return fails != 0;
}
