/* PSS sample-data bench: pss_core.c with build/pss_data.inc (made by gen_pss11.py) or another data
 * set (-DPSS_DATA_INC, run_awmtest.py: a synthetic GEW7 ROM), alone at
 * a low SDRAM address. Measures cycles per voice block over keys and sounds; returns failed-check
 * bits (0 = pass). Only built when the data exists (run_psstest.py). */
#include "../pss_core.h"

#define SR      88200

volatile uint32_t worst_one, worst_two, worst_heavy;   /* read by run_psstest.py */
volatile int fails;
static int16_t out[128];
static uint32_t inc[128];

static uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

static int16_t zk(int z, int n) { return (int16_t)(((2 * z + 1) * 32768L) / (2 * n)); }

/* cycles of one steady block (after 40 blocks), the worst over keys 36 .. C8 (both oscillators'
 * sounding keys <= 108, where the octave-down copies reach) */
static uint32_t cost(int a, int b, int bpitch, int filter)
{
    static pss_voice v;
    int16_t p[PSS_PARAMS];
    uint32_t worst = 0, t0, t;
    int key, i, k;

    for (key = 36; key + bpitch <= 108; key += 12) {
        for (i = 0; i < PSS_PARAMS; i++)
            p[i] = 0;
        p[PSS_WAVE_A] = zk(a, pss_nwaves());
        p[PSS_WAVE_B] = zk(b + 1, pss_nwaves() + 1);
        p[PSS_MIX] = 16384;
        p[PSS_CUTOFF] = filter ? 16000 : 32767;
        p[PSS_BPITCH] = zk(24 + bpitch, 49);
        if (filter) {
            p[PSS_RES] = 20000;
            p[PSS_FENV] = 30000;
            p[PSS_FDECAY] = 20000;
            p[PSS_KEYTRK] = 16000;
            p[PSS_ADECAY] = 20 * 520;
            p[PSS_BFADE] = 20 * 520;
        }
        for (k = 0; k < 128; k++)
            inc[k] = pss_inc_of_key(key);
        pss_note_on(&v, inc[0], p);
        for (k = 0; k < 40; k++)
            pss_render(&v, out, 128, inc, p);
        t0 = cycles();
        pss_render(&v, out, 128, inc, p);
        t = cycles() - t0;
        worst = t > worst ? t : worst;
    }
    return worst;
}

int main(void)
{
    int waves[7], nw = pss_nwaves(), i, j;

    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");  /* CCEN */
    if (!pss_have_rom() || nw < 1)
        return 1;
    for (i = 0; i < 7; i++)                     /* 7 sounds spread over the set */
        waves[i] = i * (nw - 1) / 6;
    for (i = 0; i < 7; i++) {
        uint32_t c = cost(waves[i], -1, 0, 0);

        worst_one = c > worst_one ? c : worst_one;
        for (j = 0; j < 7; j++) {
            c = cost(waves[i], waves[j], 12, 0);
            worst_two = c > worst_two ? c : worst_two;
        }
        c = cost(waves[i], waves[6], 24, 1);
        worst_heavy = c > worst_heavy ? c : worst_heavy;
    }
    if (worst_heavy >= 30000)
        fails |= 2;
    return fails;
}
