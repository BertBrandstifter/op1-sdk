/* Host tests for the M1 voice core (plain cc, built with m1_core.c by tools/test_all.py). Run with
 * the built-in waves, and with build/m1_data.inc when gen_m1.py --sf2 made one (extra checks). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m1_core.h"
#include "m1_testvec.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SR      M1_SR
#define N       (SR * 2)
static int16_t buf[N];
static uint32_t incs[128];

static uint32_t inc_hz(double hz) { return (uint32_t)(hz / SR * 4294967296.0); }
static int16_t zk(int z, int n) { return (int16_t)(((2 * z + 1) * 32768L) / (2 * n)); }
static int16_t field(int v) { return (int16_t)(v * 520); }

static int find(const char *a, const char *b)  /* multisound by name, -1 if none */
{
    int w;

    for (w = 0; w < m1_nwaves(); w++)
        if (!strcmp(m1_wave_name(w), a) || (b && !strcmp(m1_wave_name(w), b)))
            return w;
    return -1;
}

/* osc A = w, B off, filter open, nothing else */
static void params(int16_t *p, int w)
{
    memset(p, 0, M1_PARAMS * sizeof(*p));
    p[M1_WAVE_A] = zk(w, m1_nwaves());
    p[M1_WAVE_B] = zk(0, m1_nwaves() + 1);      /* off */
    p[M1_CUTOFF] = 32767;
    p[M1_BPITCH] = zk(24, 49);
}

static void render(m1_voice *v, uint32_t inc, const int16_t *p, int n)
{
    int i, k;

    memset(v, 0, sizeof(*v));
    for (k = 0; k < 128; k++)
        incs[k] = inc;
    m1_note_on(v, inc, p);
    for (i = 0; i < n; i += 128)
        m1_render(v, buf + i, n - i < 128 ? n - i : 128, incs, p);
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

static int crossings(int from, int len)
{
    int i, n = 0, low = 1;

    for (i = from; i < from + len; i++) {
        if (low && buf[i] > 3000) { n++; low = 0; }
        else if (buf[i] < -3000) low = 1;
    }
    return n;
}

/* fundamental from rising zero crossings (interpolated; one counts after the wave was below -2000,
 * so ripple around zero does not), for waves with one such crossing per cycle */
static double hz_of(int from, int to)
{
    double first = -1, last = 0;
    int i, n = 0, armed = 0;

    for (i = from; i < to - 1; i++) {
        if (buf[i] < -2000)
            armed = 1;
        if (armed && buf[i] < 0 && buf[i + 1] >= 0) {
            armed = 0;
            double t = i + (double)-buf[i] / (buf[i + 1] - buf[i]);

            if (first < 0)
                first = t;
            else
                n++;
            last = t;
        }
    }
    return n && last > first ? n / ((last - first) / SR) : 0;
}

static void test_decoder(void)
{
    static m1_sample s;
    m1_osc o;
    int i, ok = 1;

    s.off = 0;
    s.len = TV_LEN;
    s.loop = TV_LOOP;
    s.h1 = TV_H1;
    s.h2 = TV_H2;
    s.fmt = 0;
    memset(&o, 0, sizeof(o));
    o.s = &s;
    o.on = 1;
    for (i = 0; i < TV_REF_LEN; i++)
        ok &= m1_osc_next(&o, tv_data) == tv_ref[i];
    CHECK(ok);                                  /* bit-exact with gen_m1.py, two loop wraps */
    s.loop = M1_NOLOOP;                         /* one-shot: silent after the end */
    memset(&o, 0, sizeof(o));
    o.s = &s;
    o.on = 1;
    for (i = 0; i < TV_LEN; i++)
        m1_osc_next(&o, tv_data);
    CHECK(m1_osc_next(&o, tv_data) == 0 && !o.on);
}

static void test_keys(void)
{
    int k, ok = 1;

    for (k = 0; k < 128; k++)
        ok &= m1_key_of_inc(m1_inc_of_key(k)) == k;
    CHECK(ok);
    CHECK(m1_key_of_inc(inc_hz(440)) == 69 && m1_key_of_inc(inc_hz(452)) == 69);
    CHECK(m1_key_of_inc(inc_hz(466.2)) == 70 && m1_key_of_inc(inc_hz(261.6)) == 60);
    CHECK(labs((long)m1_inc_of_key(69) - (long)inc_hz(440)) < 2 && labs((long)m1_inc_of_key(57) - (long)inc_hz(220)) < 2);
}

static void test_pitch(void)
{
    int16_t p[M1_PARAMS];
    m1_voice v;
    int w = find("SAW", "SAW WAVE"), n;

    CHECK(w >= 0);
    if (w < 0)
        return;
    params(p, w);
    render(&v, inc_hz(440), p, SR);             /* 1 s */
    n = crossings(0, SR);
    CHECK(n >= 437 && n <= 443);
    render(&v, m1_inc_of_key(96), p, SR);       /* C7 and C8: octave-down copies keep the pitch */
    n = crossings(0, SR);
    CHECK(n >= 2093 - 21 && n <= 2093 + 21);
    render(&v, m1_inc_of_key(108), p, SR);
    n = crossings(0, SR);
    CHECK(n >= 4186 - 42 && n <= 4186 + 42);
    p[M1_WAVE_B] = zk(w + 1, m1_nwaves() + 1);  /* B = the same wave an octave up, A silent */
    p[M1_BPITCH] = zk(24 + 12, 49);
    p[M1_MIX] = 32767;
    render(&v, inc_hz(220), p, SR);
    n = crossings(0, SR);
    CHECK(n >= 437 && n <= 443);
}

static void test_all_waves(void)
{
    static const int keys[] = { 24, 48, 60, 72, 96, 108 };
    int16_t p[M1_PARAMS];
    m1_voice v;
    int w, k, i, bad = 0, silent = 0, dead_loops = 0;

    for (w = 0; w < m1_nwaves(); w++)
        for (k = 0; k < (int)(sizeof(keys) / sizeof(keys[0])); k++) {
            const m1_sample *s = m1_pick(w, keys[k]);
            int clip = 0;

            params(p, w);
            render(&v, m1_inc_of_key(keys[k]), p, SR);
            for (i = 0; i < SR; i++)
                clip += buf[i] == 32767 || buf[i] == -32768;
            bad += clip > 8;
            silent += rms(0, SR / 10) < 30;           /* BREATH at the lowest key: ~100 */
            if (s->loop != M1_NOLOOP && rms(SR - 4410, 4410) < 30)
                dead_loops++;
        }
    CHECK(bad == 0);
    CHECK(silent == 0);
    CHECK(dead_loops == 0);                     /* looped sounds keep sounding */
}

static void test_filter_levels(void)
{
    int16_t p[M1_PARAMS];
    m1_voice v;
    int w = find("SAW", "SAW WAVE");
    double h10_open, h10_closed, a0, a1;

    if (w < 0)
        return;
    params(p, w);
    render(&v, inc_hz(220), p, SR / 2);
    h10_open = amp(2200, 8192, 8192);
    p[M1_CUTOFF] = 12000;                       /* ~300 Hz */
    render(&v, inc_hz(220), p, SR / 2);
    h10_closed = amp(2200, 8192, 8192);
    CHECK(h10_closed < h10_open * 0.1);
    p[M1_FENV] = field(63);                     /* envelope opens it at the start */
    p[M1_FDECAY] = field(40);
    render(&v, inc_hz(220), p, SR / 2);
    CHECK(amp(2200, 0, 4096) > h10_closed * 3);
    p[M1_RES] = 32767;                          /* resonance stays bounded */
    render(&v, inc_hz(220), p, SR / 2);
    CHECK(rms(0, SR / 2) < 20000);

    params(p, w);                               /* A DECAY: quieter after 1 s (amount 50: ~0.15 s) */
    p[M1_ADECAY] = field(50);
    render(&v, inc_hz(220), p, SR + SR / 2);
    a0 = rms(0, 4096);
    a1 = rms(SR, 4096);
    CHECK(a1 < a0 * 0.3);
    w = find("SINE", "DWGS SINE");             /* B FADE: B (an octave up) rises, A steady */
    CHECK(w >= 0);
    params(p, w);
    p[M1_WAVE_B] = zk(w + 1, m1_nwaves() + 1);
    p[M1_BPITCH] = zk(24 + 12, 49);
    p[M1_MIX] = 16384;
    p[M1_BFADE] = field(40);
    render(&v, inc_hz(220), p, SR + SR / 2);
    CHECK(amp(440, 0, 4096) < amp(440, SR, 4096) * 0.5);
    CHECK(fabs(amp(220, 0, 4096) - amp(220, SR, 4096)) < amp(220, SR, 4096) * 0.2);
    p[M1_MIX] = 0;                              /* MIX: all A */
    p[M1_BFADE] = 0;
    render(&v, inc_hz(220), p, SR / 2);
    CHECK(amp(440, 8192, 8192) < amp(220, 8192, 8192) * 0.05);
}

/* every page 2 / 3 parameter and MIX / CUTOFF stepped through its range: the measured effect only
 * moves one way (knob, bar and sound agree); a SAW at 220 Hz, B = SAW */
static double effect(int q, int v)
{
    int16_t p[M1_PARAMS];
    m1_voice vc;
    int w = find("SAW", "SAW WAVE");

    params(p, w);
    p[q] = (int16_t)v;
    switch (q) {
    case M1_CUTOFF:                             /* brightness: the 10th harmonic */
        render(&vc, inc_hz(220), p, SR / 4);
        return amp(2200, 8192, 8192);
    case M1_RES:                                /* resonance: the peak at a low cutoff */
        p[M1_CUTOFF] = 14000;
        render(&vc, inc_hz(110), p, SR / 4);
        return rms(8192, 8192);
    case M1_FENV:                               /* brightness at the start */
        p[M1_CUTOFF] = 8000;
        p[M1_FDECAY] = field(30);
        render(&vc, inc_hz(220), p, SR / 4);
        return amp(2200, 0, 2048);
    case M1_FDECAY:                             /* brightness later in the note */
        p[M1_CUTOFF] = 8000;
        p[M1_FENV] = field(63);
        render(&vc, inc_hz(220), p, SR / 2);
        return amp(2200, 16384, 4096);
    case M1_KEYTRK:                             /* brightness of a high note */
        p[M1_CUTOFF] = 12000;
        render(&vc, m1_inc_of_key(96), p, SR / 4);
        return amp(2093.0 * 2, 8192, 8192);
    case M1_ADECAY:                             /* level after 0.5 s: less with more decay */
        render(&vc, inc_hz(220), p, SR);
        return -rms(SR / 2, 4096);
    /* B a fifth up (330 Hz): not one of A's harmonics, which would add to / cancel B by phase */
    case M1_BFADE:                              /* B at the start: less with more fade */
        p[M1_WAVE_B] = zk(w + 1, m1_nwaves() + 1);
        p[M1_BPITCH] = zk(24 + 7, 49);
        p[M1_MIX] = 16384;
        render(&vc, inc_hz(220), p, SR / 4);
        return -amp(220 * 1.4983, 0, 4096);
    case M1_MIX:                                /* B against A */
        p[M1_WAVE_B] = zk(w + 1, m1_nwaves() + 1);
        p[M1_BPITCH] = zk(24 + 7, 49);
        render(&vc, inc_hz(220), p, SR / 4);
        return amp(220 * 1.4983, 8192, 8192) / (amp(220, 8192, 8192) + 1);
    case M1_DETUNE:                             /* beating of A + B: the level's spread over 1 s */
        {
            double lo = 1e9, hi = 0;
            int i;

            p[M1_WAVE_B] = zk(w + 1, m1_nwaves() + 1);
            p[M1_MIX] = 16384;
            render(&vc, inc_hz(220), p, SR);
            for (i = 0; i < SR - 4096; i += 4096) {
                double r = amp(220, i, 4096);

                lo = r < lo ? r : lo;
                hi = r > hi ? r : hi;
            }
            return hi - lo;
        }
    }
    return 0;
}

static void test_monotonic(void)
{
    static const struct { int q, from; const char *name; } par[] = {
        { M1_CUTOFF, 8000, "CUTOFF" }, { M1_RES, 0, "RES" }, { M1_FENV, 0, "F.ENV" },
        { M1_FDECAY, 0, "F.DECAY" }, { M1_KEYTRK, 0, "KEY TRK" }, { M1_ADECAY, 520, "A DECAY" },
        { M1_BFADE, 0, "B FADE" }, { M1_MIX, 0, "MIX" }, { M1_DETUNE, 520, "DETUNE" },
    };
    unsigned int i;

    for (i = 0; i < sizeof(par) / sizeof(par[0]); i++) {
        double prev = effect(par[i].q, par[i].from), first = prev, e;
        int v, bad = 0;

        for (v = par[i].from; v <= 32767; v += (32767 - par[i].from) / 8) {
            e = effect(par[i].q, v);
            bad += e < prev - 0.02 * (fabs(prev) + 1);   /* never back down (2 % tolerance) */
            prev = e;
        }
        e = effect(par[i].q, 32767);
        if (bad || !(e > first))
            printf("  %s: not monotonic (%d steps back, %.1f -> %.1f)\n", par[i].name, bad, first, e);
        CHECK(!bad && e > first);
    }
}

static void test_restart(void)
{
    static int16_t first[128];
    int16_t p[M1_PARAMS];
    m1_voice v;

    params(p, 0);
    render(&v, inc_hz(330), p, 128);
    memcpy(first, buf, sizeof(first));
    render(&v, inc_hz(330), p, 128);
    CHECK(!memcmp(first, buf, sizeof(first)));
}

static void test_rom(void)
{
    int16_t p[M1_PARAMS];
    m1_voice v;
    int w, nt = 0, ok = 1;
    static int16_t a[8192];

    if (!m1_have_rom()) {
        printf("(built-in waves only: SoundFont checks skipped)\n");
        return;
    }
    CHECK(m1_nwaves() == 100 && m1_rom_bytes() <= 1500000);
    CHECK(find("PIANO", 0) == 0 && find("DWGS SINE", 0) == 99);
    for (w = 0; w < m1_nwaves(); w++) {         /* "NT" sounds play at one pitch on every key */
        params(p, w);
        p[M1_CUTOFF] = 20000;
        render(&v, m1_inc_of_key(48), p, 8192);
        memcpy(a, buf, sizeof(a));
        render(&v, m1_inc_of_key(72), p, 8192);
        if (!memcmp(a, buf, sizeof(a)))
            nt++;
        else
            ok &= strstr(m1_wave_name(w), " NT") == 0;
    }
    CHECK(nt == 14 && ok);                      /* 12 "NT" + BASSTHUMB1 / 2 */
    {                                           /* single cycles in tune at C4, C7, C8 (+-3 cents) */
        static const char *const one[] = { "SAW WAVE", "SQUAREWAVE", "DWGS SINE", "DWGS TRI" };
        static const int key[] = { 60, 96, 108 };
        unsigned int i, j;

        for (i = 0; i < sizeof(one) / sizeof(one[0]); i++)
            for (j = 0; j < 3; j++) {
                int x = find(one[i], 0);
                double c;

                CHECK(x >= 0);
                if (x < 0)
                    continue;
                params(p, x);
                render(&v, m1_inc_of_key(key[j]), p, SR);
                c = 1200 * log2(hz_of(SR / 5, SR - SR / 5) / (440 * pow(2, (key[j] - 69) / 12.0)));
                CHECK(fabs(c) < 3);
            }
    }
    w = find("DWGS SINE", 0);                   /* single cycle at its pitch */
    params(p, w);
    render(&v, inc_hz(440), p, SR);
    CHECK(amp(440, 4096, 16384) > 20 * amp(880, 4096, 16384));
}

int main(void)
{
    test_decoder();
    test_keys();
    test_pitch();
    test_all_waves();
    test_filter_levels();
    test_restart();
    test_monotonic();
    test_rom();
    printf("%s: %d checks, %d failed (%s, %u bytes)\n", fails ? "FAIL" : "OK", checks, fails,
           m1_have_rom() ? "SoundFont data" : "built-in waves", m1_rom_bytes());
    return fails != 0;
}
