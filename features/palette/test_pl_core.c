/* Host tests for the PALETTE voice core (plain cc, built with pl_core.c by tools/test_all.py).
 * Measured, not read: every palette parameter and the key tone parameters are swept through their
 * range and their effect may only move one way; waves are checked for aliasing at high notes; the
 * whole bank for level bounds. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pl_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SR      PL_SR
#define N       (SR * 3)
static int16_t buf[N];
static uint32_t incs[128];

static uint32_t inc_hz(double hz) { return (uint32_t)(hz / SR * 4294967296.0); }

static void play(pl_voice *v, double hz, const pl_params *p, int from, int n)
{
    int i, k;

    for (k = 0; k < 128; k++)
        incs[k] = inc_hz(hz);
    for (i = from; i < from + n; i += 128)
        pl_render(v, buf + i, from + n - i < 128 ? from + n - i : 128, incs, p);
}

static void note(pl_voice *v, double hz, const pl_params *p)
{
    pl_voice_init(v);
    pl_note(v, inc_hz(hz), p);
}

static double rms(int from, int n)
{
    double s = 0;
    int i;

    for (i = from; i < from + n; i++)
        s += (double)buf[i] * buf[i];
    return sqrt(s / n);
}

static int peak(int from, int n)
{
    int i, m = 0;

    for (i = from; i < from + n; i++)
        m = abs(buf[i]) > m ? abs(buf[i]) : m;
    return m;
}

/* brightness: rms of the first difference over rms (grows with the spectrum's centroid) */
static double bright(int from, int n)
{
    double s = 0, d = 0;
    int i;

    for (i = from + 1; i < from + n; i++) {
        s += (double)buf[i] * buf[i];
        d += (double)(buf[i] - buf[i - 1]) * (buf[i] - buf[i - 1]);
    }
    return s > 0 ? sqrt(d / s) : 0;
}

/* rising zero crossings with hysteresis per second */
static double freq(int from, int n)
{
    int i, c = 0, low = 1, th = peak(from, n) / 4;

    for (i = from; i < from + n; i++) {
        if (low && buf[i] > th) { c++; low = 0; }
        else if (buf[i] < -th) low = 1;
    }
    return c * (double)SR / n;
}

/* ------------------------------------------------------------------ a test patch: one tone */

static pl_patch tp;

static void one_tone(int wave, int mode)
{
    memset(&tp, 0, sizeof(tp));
    tp.name = "TEST";
    tp.t[0].wave = (uint8_t)wave;
    tp.t[0].level = 100;
    tp.t[0].pitch = PL_PITCH0;
    tp.t[0].mode = (uint8_t)mode;
    tp.t[0].cutoff = 100;
    tp.t[0].at[3] = 40;
    tp.t[0].al[0] = tp.t[0].al[1] = tp.t[0].al[2] = 100;
    tp.lfo[0].rate = 50;
    tp.lfo[1].rate = 50;
    pl_test_patch = &tp;
}

static void params1(pl_params *p)
{
    memset(p, 0, sizeof(*p));
    p->level[0] = 31;
    p->wave[0] = tp.t[0].wave;
    p->pitch[0] = PL_PITCH0;
    p->cut[0] = PL_CUT0;
    p->time[0] = PL_TIME0;
    p->pitch[1] = p->pitch[2] = p->pitch[3] = PL_PITCH0;
    p->cut[1] = p->cut[2] = p->cut[3] = PL_CUT0;
    p->time[1] = p->time[2] = p->time[3] = PL_TIME0;
}

/* a sweep: y(i) must move one way (up = 1: rise; steps back by less than tol), overall by > 5 % */
static int monotonic_t(const double *y, int n, int up, double tol)
{
    int i, ok = 1;

    for (i = 1; i < n; i++)
        if (up ? y[i] < y[i - 1] * (1 - tol) - 1e-9 : y[i] > y[i - 1] * (1 + tol) + 1e-9)
            ok = 0;
    ok &= up ? y[n - 1] > y[0] * 1.05 : y[n - 1] < y[0] * 0.95;
    if (!ok) {
        printf("  sweep:");
        for (i = 0; i < n; i++)
            printf(" %.4g", y[i]);
        printf("\n");
    }
    return ok;
}

static int monotonic(const double *y, int n, int up) { return monotonic_t(y, n, up, 0.001); }

/* inharmonic share of the spectrum (aliasing), dB: DFT (Hann) of 8192 samples, power further than
 * 4 bins from every harmonic of f0 over the total */
static double alias_db(int from, double f0)
{
    static double re[4096];
    const int M = 8192;
    double tot = 0, bad = 0;
    int k, i;

    for (k = 1; k < M * 20000 / SR; k++) {     /* below 20 kHz: TE's decimation removes 22 .. 44 kHz */
        double c = 0, s = 0, w = 2 * M_PI * k / M, fk = (double)k * SR / M, h, dist;

        for (i = 0; i < M; i++) {
            double x = buf[from + i] * (0.5 - 0.5 * cos(2 * M_PI * i / M));
            c += x * cos(w * i);
            s += x * sin(w * i);
        }
        re[k] = c * c + s * s;
        h = floor(fk / f0 + 0.5);
        dist = fabs(fk - h * f0) / ((double)SR / M);
        tot += re[k];
        if (dist > 4 && fk > 30 && fk < 20000)     /* TE's decimation removes 22 .. 44 kHz */
            bad += re[k];
    }
    return 10 * log10((bad + 1e-9) / tot);
}

static void test_waves_bandlimited(void)
{
    static const double hz[3] = { 1013.0, 3010.0, 6007.0 };
    pl_voice v;
    pl_params p;
    int w, h;

    for (w = 0; w < PL_WHITE; w++) {
        for (h = 0; h < 3; h++) {
            double a;

            if (w == PL_FATSAW || (w < PL_TRI && h == 2))
                continue;                       /* fat saw: detuned by design; polyBLEP: to 3 kHz */
            one_tone(w, PL_FOFF);
            tp.lfo[0].delay = 100;              /* PWM: a still width (its sweep spreads the spectrum) */
            params1(&p);
            note(&v, hz[h], &p);
            play(&v, hz[h], &p, 0, 8192 + 4096);
            a = alias_db(4096, hz[h]);
            if (a > (w < PL_TRI ? -40 : -45))
                printf("  wave %s at %.0f Hz: inharmonic %.1f dB\n", pl_wave_names[w], hz[h], a);
            CHECK(a < (w < PL_TRI ? -40 : -45));    /* polyBLEP waves -40 dB, tables -45 dB */
            CHECK(rms(4096, 4096) > 300);       /* and it sounds (formant waves are thin up here) */
        }
    }
    pl_test_patch = 0;
}

static void test_palette_sweeps(void)
{
    pl_voice v;
    pl_params p;
    double y[32];
    int i;

    /* LEVEL: louder */
    one_tone(PL_SAW, PL_LPF);
    for (i = 0; i < 32; i++) {
        params1(&p);
        p.level[0] = (uint8_t)i;
        note(&v, 220, &p);
        play(&v, 220, &p, 0, 8192);
        y[i] = rms(4096, 4096) + i * 1e-6;
    }
    CHECK(y[0] < 1 && monotonic(y + 1, 31, 1));

    /* CUTOFF offset: brighter */
    one_tone(PL_SAW, PL_LPF);
    tp.t[0].cutoff = 60;                        /* 25 .. 100: above the fundamental (else the */
    for (i = 0; i < PL_CUTS; i++) {             /* measure sees a sine whatever the cutoff) */
        params1(&p);
        p.cut[0] = (uint8_t)i;
        note(&v, 55, &p);
        play(&v, 55, &p, 0, 8192);
        y[i] = bright(4096, 4096);
    }
    CHECK(monotonic(y, PL_CUTS, 1));

    /* PITCH: higher */
    one_tone(PL_SINE, PL_FOFF);
    for (i = 0; i < PL_PITCHES; i++) {
        params1(&p);
        p.pitch[0] = (uint8_t)i;
        note(&v, 110, &p);
        play(&v, 110, &p, 0, 22050);
        y[i] = freq(4096, 17954);
    }
    CHECK(monotonic(y, PL_PITCHES, 1));
    CHECK(fabs(y[PL_PITCH0] - 110) < 3 && fabs(y[PL_PITCH0 + 12] - 220) < 4 && fabs(y[0] - 27.5) < 3);

    /* TIME: the attack (T1 = 60) takes longer: samples until 50 % of the sustain level */
    one_tone(PL_SAW, PL_FOFF);
    tp.t[0].at[0] = 60;
    for (i = 0; i < PL_TIMES; i++) {
        int k;
        double full;

        params1(&p);
        p.time[0] = (uint8_t)i;
        note(&v, 220, &p);
        play(&v, 220, &p, 0, N);
        full = peak(N - 4096, 4096);
        for (k = 0; k + 512 < N && peak(k, 512) < full * 0.5; k += 512)
            ;
        y[i] = k + 1;
    }
    CHECK(monotonic(y, PL_TIMES, 1));

    /* WAVE: every wave sounds at LEVEL 31, none is silent */
    for (i = 0; i < PL_WAVES; i++) {
        one_tone(i, PL_FOFF);
        params1(&p);
        note(&v, 220, &p);
        play(&v, 220, &p, 0, 8192);
        if (rms(4096, 4096) <= 3000)
            printf("  wave %s at 220 Hz: rms %.0f\n", pl_wave_names[i], rms(4096, 4096));
        CHECK(rms(4096, 4096) > 3000);
    }
    pl_test_patch = 0;
}

static void test_tone_sweeps(void)
{
    pl_voice v;
    pl_params p;
    double y[16];
    int i;

    /* TVF cutoff (LPF, BPF centre, HPF): brighter */
    for (int mode = PL_LPF; mode <= PL_HPF; mode++) {
        for (i = 0; i < 11; i++) {
            one_tone(mode == PL_LPF ? PL_SAW : PL_WHITE, mode);
            tp.t[0].cutoff = (uint8_t)(i * 10);
            params1(&p);
            note(&v, 110, &p);
            play(&v, 110, &p, 0, 16384);
            y[i] = bright(4096, 12288) + i * 1e-6;
        }
        CHECK(monotonic_t(y + 3, 8, 1, mode == PL_LPF ? 0.001 : 0.01));   /* noise: 1 % jitter */
    }

    /* resonance: a sine at the cutoff (60 = 1.32 kHz) comes out louder */
    for (i = 0; i < 11; i++) {
        one_tone(PL_SINE, PL_LPF);
        tp.t[0].cutoff = 60;
        tp.t[0].reso = (uint8_t)(i * 10);
        params1(&p);
        note(&v, 1316, &p);
        play(&v, 1316, &p, 0, 16384);
        y[i] = rms(8192, 8192);
    }
    CHECK(monotonic(y, 11, 1) && y[10] > 4 * y[0]);

    /* TVF envelope depth: brighter at the peak (L1 = 50, T1 = 0, decays slowly) */
    for (i = 0; i < 6; i++) {
        one_tone(PL_SAW, PL_LPF);
        tp.t[0].cutoff = 30;
        tp.t[0].f_depth = (int8_t)(i * 10);
        tp.t[0].ft[1] = 80;
        tp.t[0].fl[0] = 50;
        params1(&p);
        note(&v, 110, &p);
        play(&v, 110, &p, 0, 8192);
        y[i] = bright(2048, 4096);
    }
    CHECK(monotonic(y, 6, 1));

    /* TVF key follow: at C6 the filter opens more with key follow */
    for (i = 0; i < 6; i++) {
        one_tone(PL_SAW, PL_LPF);
        tp.t[0].cutoff = 50;
        tp.t[0].f_kf = (int16_t)(i * 30);
        params1(&p);
        note(&v, 1046.5, &p);
        play(&v, 1046.5, &p, 0, 8192);
        y[i] = bright(4096, 4096);
    }
    CHECK(monotonic(y, 6, 1));

    /* TVA sustain level: louder */
    for (i = 0; i < 11; i++) {
        one_tone(PL_SAW, PL_FOFF);
        tp.t[0].al[2] = (uint8_t)(i * 10);
        tp.t[0].at[2] = 10;
        params1(&p);
        note(&v, 220, &p);
        play(&v, 220, &p, 0, 16384);
        y[i] = rms(8192, 8192) + i * 1e-6;
    }
    CHECK(y[0] < 1 && monotonic(y + 1, 10, 1));

    /* fine tune: higher */
    for (i = 0; i < 6; i++) {
        one_tone(PL_SINE, PL_FOFF);
        tp.t[0].fine = (int8_t)(-100 + i * 40);
        params1(&p);
        note(&v, 440, &p);
        play(&v, 440, &p, 0, N);
        y[i] = freq(4096, N - 4096);
    }
    CHECK(monotonic(y, 6, 1));

    /* pitch envelope L0: the note starts higher, then settles on its pitch */
    for (i = 0; i < 5; i++) {
        one_tone(PL_SINE, PL_FOFF);
        tp.t[0].pe_l0 = (int16_t)(i * 600);
        tp.t[0].pe_t1 = 70;
        params1(&p);
        note(&v, 220, &p);
        play(&v, 220, &p, 0, N);
        y[i] = freq(0, 8820);
        CHECK(fabs(freq(N - 22050, 22050) - 220) < 5);
    }
    CHECK(monotonic(y, 5, 1));

    /* LFO rate: a faster vibrato (pitch depth 50, LFO 1 square): more pitch changes per second */
    for (i = 0; i < 5; i++) {
        int k, flips = 0, prev = 0;

        one_tone(PL_SINE, PL_FOFF);
        tp.t[0].p_lfo = 50;
        tp.lfo[0].rate = (uint8_t)(30 + i * 10);
        tp.lfo[0].wave = PL_LSQR;
        params1(&p);
        note(&v, 440, &p);
        play(&v, 440, &p, 0, N);
        for (k = 0; k + 2205 <= N; k += 2205) {
            int hi = freq(k, 2205) > 440;

            flips += k && hi != prev;
            prev = hi;
        }
        y[i] = flips + i * 1e-6;
    }
    CHECK(monotonic(y, 5, 1));

    /* pitch key follow 0: every key plays C4 */
    one_tone(PL_SINE, PL_FOFF);
    tp.t[0].p_kf = -100;
    params1(&p);
    note(&v, 880, &p);
    play(&v, 880, &p, 0, 44100);
    CHECK(fabs(freq(4096, 40000) - 261.6) < 4);
    pl_test_patch = 0;
}

static void test_release(void)
{
    pl_voice v;
    pl_params p;

    /* sustain holds; key up: the release (T4 = 40, 52 ms) ends the tone, the voice stops */
    one_tone(PL_SAW, PL_FOFF);
    params1(&p);
    note(&v, 220, &p);
    play(&v, 220, &p, 0, 44100);
    CHECK(rms(40000, 4096) > 3000);
    pl_release(&v, &p);
    play(&v, 220, &p, 44100, 22050);
    CHECK(rms(44100, 1024) > 1000 && rms(44100 + 11025, 4096) == 0);
    CHECK(!pl_sounding(&v));
    /* a longer T4: still sounding after 100 ms */
    tp.t[0].at[3] = 70;
    note(&v, 220, &p);
    play(&v, 220, &p, 0, 44100);
    pl_release(&v, &p);
    play(&v, 220, &p, 44100, 22050);
    CHECK(rms(44100 + 8820, 1024) > 1000 && pl_sounding(&v));
    pl_test_patch = 0;
}

/* the bank: the palette a pick sets, every patch sounds, peaks stay under the limiter's ceiling with
 * little limiting, held and released */
static void test_bank(void)
{
    pl_voice v;
    pl_params p;
    int e, i;

    CHECK(pl_npatches >= 20 && pl_npatches <= 32);
    for (e = 0; e < pl_npatches; e++) {
        static const double hz[3] = { 65.4, 261.6, 1046.5 };
        int h, over = 0, total = 0;

        pl_patch_params(e, &p);
        CHECK(p.patch == e);
        for (i = 0; i < PL_TONES; i++)
            CHECK(p.level[i] <= 31 && p.wave[i] < PL_WAVES && p.pitch[i] < PL_PITCHES
                  && p.cut[i] == PL_CUT0 && p.time[i] == PL_TIME0);
        for (h = 0; h < 3; h++) {
            double r;
            int k, pk;

            note(&v, hz[h], &p);
            play(&v, hz[h], &p, 0, 2 * SR);
            pl_release(&v, &p);
            play(&v, hz[h], &p, 2 * SR, SR);
            r = rms(0, 2 * SR);
            pk = peak(0, 3 * SR);
            for (k = 0; k < 3 * SR; k++) {
                over += abs(buf[k]) > 22000;
                total++;
            }
            if (r < 500 || pk > 28100)
                printf("  %s at %.0f Hz: rms %.0f peak %d\n", pl_patches[e].name, hz[h], r, pk);
            CHECK(r > 500 && pk <= 28100);
        }
        if (over * 100 > total)
            printf("  %s: %d of %d samples limited\n", pl_patches[e].name, over, total);
        CHECK(over * 100 <= total);
    }
}

int main(void)
{
    test_waves_bandlimited();
    test_palette_sweeps();
    test_tone_sweeps();
    test_release();
    test_bank();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
