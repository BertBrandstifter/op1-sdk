/* M1 SoundFont-data bench: m1_core.c with build/m1_data.inc (made by gen_m1.py --sf2), alone at
 * a low SDRAM address. Measures cycles per voice block over keys and sounds; returns failed-check
 * bits (0 = pass). Only built when the data exists (run_m1test.py). */
#include "../m1_core.h"

#define SR      88200

volatile uint32_t worst_one, worst_two, worst_heavy;   /* read by run_m1test.py */
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
    static m1_voice v;
    int16_t p[M1_PARAMS];
    uint32_t worst = 0, t0, t;
    int key, i, k;

    for (key = 36; key + bpitch <= 108; key += 12) {
        for (i = 0; i < M1_PARAMS; i++)
            p[i] = 0;
        p[M1_WAVE_A] = zk(a, m1_nwaves());
        p[M1_WAVE_B] = zk(b + 1, m1_nwaves() + 1);
        p[M1_MIX] = 16384;
        p[M1_CUTOFF] = filter ? 16000 : 32767;
        p[M1_BPITCH] = zk(24 + bpitch, 49);
        if (filter) {
            p[M1_RES] = 20000;
            p[M1_FENV] = 30000;
            p[M1_FDECAY] = 20000;
            p[M1_KEYTRK] = 16000;
            p[M1_ADECAY] = 20 * 520;
            p[M1_BFADE] = 20 * 520;
        }
        for (k = 0; k < 128; k++)
            inc[k] = m1_inc_of_key(key);
        m1_note_on(&v, inc[0], p);
        for (k = 0; k < 40; k++)
            m1_render(&v, out, 128, inc, p);
        t0 = cycles();
        m1_render(&v, out, 128, inc, p);
        t = cycles() - t0;
        worst = t > worst ? t : worst;
    }
    return worst;
}

int main(void)
{
    static const int8_t waves[] = { 0, 3, 4, 6, 31, 42, 91 };  /* piano, clav, harpsichord, organ 2,
                                                                 * strings, lore, saw */
    unsigned int i, j;

    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");  /* CCEN */
    if (!m1_have_rom() || m1_nwaves() != 100)
        return 1;
    for (i = 0; i < sizeof(waves); i++) {
        uint32_t c = cost(waves[i], -1, 0, 0);

        worst_one = c > worst_one ? c : worst_one;
        for (j = 0; j < sizeof(waves); j++) {
            c = cost(waves[i], waves[j], 12, 0);
            worst_two = c > worst_two ? c : worst_two;
        }
        c = cost(waves[i], 31, 24, 1);
        worst_heavy = c > worst_heavy ? c : worst_heavy;
    }
    if (worst_heavy >= 30000)
        fails |= 2;
    return fails;
}
