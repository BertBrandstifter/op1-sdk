/* Host tests for the MALLET voice core (plain cc, built with mallet_core.c by tools/test_all.py). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mallet_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SR      ML_SR
#define N       (SR * 4)                        /* four seconds */
static int16_t buf[N + 128];                    /* whole 128-sample blocks */
static uint32_t incs[128];
static ml_voice v;
static uint32_t motor_ph;

static uint32_t inc_hz(double hz) { return (uint32_t)(hz / SR * 4294967296.0); }
static int16_t zone(int z, int n) { return (int16_t)(((2 * z + 1) * 32768L) / (2 * n)); }

static void params(int16_t *p, int instr)
{
    p[ML_INSTR] = zone(instr, ML_INSTRS);
    p[ML_MALLET] = 16384;
    p[ML_DECAY] = 16384;
    p[ML_STRIKE] = 16384;
    p[ML_MOTOR] = 0;
    p[ML_TUBE] = 0;
    p[ML_DETUNE] = 16384;
    p[ML_TONE] = 32767;
}

/* one strike of hz, rendered for n samples in 128-sample blocks (motor advanced per block) */
static void strike(double hz, const int16_t *p, int n, int fresh)
{
    int i, k;

    if (fresh)
        memset(&v, 0, sizeof(v));
    ml_note_on(&v);
    for (k = 0; k < 128; k++)
        incs[k] = inc_hz(hz);
    for (i = 0; i < n; i += 128)
        ml_render(&v, buf + i, 128, incs, p, ml_motor(&motor_ph, p, 128));
}

/* amplitude of hz in buf[from, from + len) (Goertzel, Hann window) */
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
    return 4 * sqrt(re * re + im * im) / len;
}

static double rms(int from, int len)
{
    double a = 0;
    int i;

    for (i = 0; i < len; i++)
        a += (double)buf[from + i] * buf[from + i];
    return sqrt(a / len);
}

static int peak(int from, int len)
{
    int i, m = 0;

    for (i = 0; i < len; i++)
        m = buf[from + i] > m ? buf[from + i] : -buf[from + i] > m ? -buf[from + i] : m;
    return m;
}

/* frequency of the strongest component near hz (+-3 %, 0.5 cent steps) */
static double find(double hz, int from, int len)
{
    double f, best = hz, a, top = 0;

    for (f = hz * 0.97; f < hz * 1.03; f *= 1.000289) {
        a = amp(f, from, len);
        if (a > top) {
            top = a;
            best = f;
        }
    }
    return best;
}

/* seconds until the 10 ms rms falls 60 dB below its first maximum (from 20 ms on) */
static double t60(int n)
{
    double top = 0, r;
    int i, w = SR / 100;

    for (i = 0; i + w <= n; i += w) {
        r = rms(i, w);
        if (i <= SR / 50 && r > top)
            top = r;
        if (i > SR / 50 && r < top / 1000)
            return (double)i / SR;
    }
    return (double)n / SR;
}

int main(void)
{
    int16_t p[ML_PARAMS];
    int t, i, ok;

    /* names and zones */
    for (t = 0, ok = 1; t < ML_INSTRS; t++)
        ok &= ml_instr(zone(t, ML_INSTRS)) == t && ml_modes(t) >= 2 && ml_modes(t) <= ML_MODES;
    CHECK(ok && strcmp(ml_instr_name(ML_STEELPAN), "STEELPAN") == 0 && ml_instr(32767) == ML_INSTRS - 1
          && ml_ratio(ML_MARIMBA, 1) > 4 * 4000);

    /* pitch: every instrument's main partial within 3 cents over C2 .. C6 (C2 checks the
     * resonators' precision at low notes) */
    {
        static const double notes[4] = { 65.41, 261.63, 440.0, 1046.5 };
        double worst = 0;

        for (t = 0; t < ML_INSTRS; t++)
            for (i = 0; i < 4; i++) {
                double hz = notes[i], target = t == ML_TUBULAR ? hz * 2 : hz, got, c;

                params(p, t);
                strike(hz, p, SR, 1);
                got = find(target, SR / 10, SR / 2);
                c = fabs(1200 * log2(got / target));
                worst = c > worst ? c : worst;
            }
        printf("pitch: worst %.2f cents over 8 instruments, C2 .. C6\n", worst);
        CHECK(worst < 3);
    }

    /* modes: marimba's 2nd partial at 4x (3.99), vibes' at 4x, xylophone's at 3x */
    {
        double m2, x2, x3;

        params(p, ML_MARIMBA);
        p[ML_MALLET] = 30000;
        strike(220, p, SR, 1);
        m2 = amp(220 * 3.99, 200, 8192) / amp(220, 200, 8192);
        params(p, ML_XYLO);
        p[ML_MALLET] = 30000;
        strike(440, p, SR, 1);
        x3 = amp(440 * 3, 200, 8192) / amp(440, 200, 8192);
        x2 = amp(440 * 2, 200, 8192) / amp(440, 200, 8192);
        printf("modes: marimba 4x/1x %.2f; xylophone 3x/1x %.2f, 2x/1x %.3f\n", m2, x3, x2);
        CHECK(m2 > 0.1 && x3 > 0.12 && x2 < 0.05);
    }

    /* decay: vibes ring long (several s at C4), marimba short, xylophone shorter; DECAY scales */
    {
        double tv, tm, tx, tm_long, tm_short;

        params(p, ML_VIBES);
        strike(261.63, p, N, 1);
        tv = t60(N);
        params(p, ML_MARIMBA);
        strike(261.63, p, N, 1);
        tm = t60(N);
        p[ML_DECAY] = 32767;
        strike(261.63, p, N, 1);
        tm_long = t60(N);
        p[ML_DECAY] = 0;
        strike(261.63, p, N, 1);
        tm_short = t60(N);
        params(p, ML_XYLO);
        strike(261.63, p, N, 1);
        tx = t60(N);
        printf("T60 at C4: vibes %.2f s, marimba %.2f s (DECAY min %.2f, max %.2f), xylophone %.2f s\n",
               tv, tm, tm_short, tm_long, tx);
        CHECK(tv > 3.5 && tm > 0.6 && tm < 2 && tx < tm && tm_long > 2.5 * tm && tm_short < tm / 2.5);
    }

    /* mallet: a hard mallet brings out the upper modes (and a click), a soft one barely */
    {
        double soft, hard;

        params(p, ML_MARIMBA);
        p[ML_MALLET] = 0;
        strike(261.63, p, SR, 1);
        soft = amp(261.63 * 3.99, 200, 4096) / amp(261.63, 200, 4096);
        p[ML_MALLET] = 32767;
        strike(261.63, p, SR, 1);
        hard = amp(261.63 * 3.99, 200, 4096) / amp(261.63, 200, 4096);
        printf("mallet: marimba 4x/1x soft %.3f, hard %.3f\n", soft, hard);
        CHECK(hard > 4 * soft && soft < 0.05);
    }

    /* motor: vibes with MOTOR on swing the level (5 ms windows over 0.25 s) far more than the
     * decay alone does */
    {
        double lo = 1e9, hi = 0, lo0 = 1e9, hi0 = 0, r;

        params(p, ML_VIBES);
        strike(440, p, SR * 2, 1);
        for (i = SR / 2; i + 441 < SR * 3 / 4; i += 441) {
            r = rms(i, 441);
            lo0 = r < lo0 ? r : lo0;
            hi0 = r > hi0 ? r : hi0;
        }
        p[ML_MOTOR] = 20000;
        strike(440, p, SR * 2, 1);
        for (i = SR / 2; i + 441 < SR * 3 / 4; i += 441) {
            r = rms(i, 441);
            lo = r < lo ? r : lo;
            hi = r > hi ? r : hi;
        }
        printf("motor: 5 ms rms %.0f .. %.0f (off: %.0f .. %.0f)\n", lo, hi, lo0, hi0);
        CHECK(hi / lo > 2.5 * (hi0 / lo0));
    }

    /* level: one strike of every instrument at C4, defaults, peaks 6000 .. 24000; TUBE louder */
    {
        int lo = 99999, hi = 0, pk, t0, t1;

        for (t = 0; t < ML_INSTRS; t++) {
            params(p, t);
            strike(261.63, p, SR / 2, 1);
            pk = peak(0, SR / 2);
            printf("level %-9s C4 peak %5d, rms 0.1-0.3 s %5.0f\n", ml_instr_name(t), pk, rms(SR / 10, SR / 5));
            lo = pk < lo ? pk : lo;
            hi = pk > hi ? pk : hi;
        }
        params(p, ML_MARIMBA);
        strike(261.63, p, SR, 1);
        t0 = (int)rms(SR / 2, SR / 4);
        p[ML_TUBE] = 32767;
        strike(261.63, p, SR, 1);
        t1 = (int)rms(SR / 2, SR / 4);
        printf("level: peaks %d .. %d; marimba 0.5 s rms TUBE 0 %d, full %d\n", lo, hi, t0, t1);
        CHECK(lo > 6000 && hi < 24000 && t1 > 2 * t0);
    }

    /* stability: every knob at 0 and at full, notes from 20 Hz to 8 kHz, 40 fast restrikes of
     * one voice: no overflow (UBSan), bounded; short settings (DECAY 0) then decay to an eighth
     * of the start within 4 s (TUBULAR at 20 Hz rings ~7 s even so) */
    {
        static const double hz[5] = { 20, 65.41, 1046.5, 4186, 8000 };
        int k, s, worst = 0, ok2 = 1;

        for (t = 0; t < ML_INSTRS; t++)
            for (s = 0; s < 2; s++)
                for (k = 0; k < 5; k++) {
                    for (i = 0; i < ML_PARAMS; i++)
                        p[i] = s ? 32767 : 0;
                    p[ML_INSTR] = zone(t, ML_INSTRS);
                    memset(&v, 0, sizeof(v));
                    for (i = 0; i < 40; i++)
                        strike(hz[k], p, 1024, 0);
                    worst = peak(0, 1024) > worst ? peak(0, 1024) : worst;
                    if (!s) {
                        strike(hz[k], p, N, 0);
                        ok2 &= peak(N - SR / 4, SR / 4) <= peak(0, SR / 4) / 8;
                    }
                }
        printf("stability: restrike peak %d, short settings decay %s\n", worst, ok2 ? "yes" : "NO");
        CHECK(worst <= 32767 && ok2);
    }

    /* a fresh voice without note_on renders silence */
    memset(&v, 0, sizeof(v));
    params(p, ML_MARIMBA);
    for (i = 0; i < 128; i++)
        incs[i] = inc_hz(440);
    ml_render(&v, buf, 128, incs, p, 32768);
    CHECK(peak(0, 128) == 0);

    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
