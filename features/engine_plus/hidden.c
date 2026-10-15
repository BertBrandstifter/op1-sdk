/* Engine-specific hidden parameters (knob slots 5-8). Each entry replaces the generic stages for
 * its engine. Slot value 0 leaves the engine exactly as stock (ep_render does not call these
 * while all four slots are 0, and every parameter below is a no-op at 0 on its own).
 * Voice layouts and the effect of each override were mapped from the R246 disassembly and
 * checked by running TE's render in the simulator (0 = bit-exact stock, on = changed spectrum). */
#include <op1/te246.h>
#include "engine.h"

#define W16(self, off)  (*(volatile int16_t *)((char *)(self) + (off)))
#define W32(self, off)  (*(volatile int32_t *)((char *)(self) + (off)))

static int16_t sat16(int32_t x)
{
    return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

/* ------------------------------------------------------------------ voltage
 * voice 0x34: +04/+0c/+14/+1c osc 1-4 phases (osc2 runs in lock-step with osc1), +2c drift added
 * to osc3's and subtracted from osc4's increment (saturating), knobs recomputed every block:
 * k0 xor level, k1 self-feedback (a plain multiplier), k2 lowpass, k3 detune. */
#define V_OSC1      0x04
#define V_OSC2      0x0c
#define V_DRIFT     0x2c
#define SHAPE_SLEW  (1 << 24)                   /* max osc2 offset change per block (no clicks) */

static void voltage_before(void *self, const int16_t *p, int32_t *h, int16_t *kc)
{
    int32_t target = (int32_t)((uint32_t)p[0] << 17), d;

    /* SHAPE: osc2 phase offset against osc1 (moves the sync point), slewed */
    if (target || h[0]) {
        d = target - h[0];
        h[0] += d > SHAPE_SLEW ? SHAPE_SLEW : d < -SHAPE_SLEW ? -SHAPE_SLEW : d;
        W32(self, V_OSC2) = W32(self, V_OSC1) + h[0];
    }
    /* SPREAD: fixed detune, osc3 up / osc4 down by up to ~20 Hz (undone after render) */
    h[1] = (int32_t)p[1] * 60;
    W32(self, V_DRIFT) += h[1];
    /* DRIFT: TE's release drift, applied while the note is held (accumulates, note_on resets) */
    W32(self, V_DRIFT) += p[2] >> 3;
    /* FBPOL: self-feedback from +k1 through 0 to -k1 */
    if (p[3])
        kc[1] = (int16_t)(((int32_t)kc[1] * (32767 - 2 * (int32_t)p[3])) >> 15);
}

static void voltage_after(void *self, const int16_t *p, int32_t *h, int16_t *kc)
{
    (void)p; (void)kc;
    W32(self, V_DRIFT) -= h[1];
}

/* ------------------------------------------------------------------ iter
 * voice 0x24: +1c c = chain gain (note_on: k0^2, then * d0^2 per block), +1e e1 = level/brightness
 * envelope (note_on: 0x7fff, then * d1 per block). Chain buffer 0x01dd4e18[128] is read sample by
 * sample before render overwrites it, so scaling it right before this voice's render feeds only
 * this voice. Knobs must never be negative here (they index tables). */
#define I_CHAIN     0x1c
#define I_ENV       0x1e
#define I_CHAINBUF  ((volatile int16_t *)0x01dd4e18)

static void iter_before(void *self, const int16_t *p, int32_t *h, int16_t *kc)
{
    int32_t g, i;

    (void)kc;
    h[0] = W16(self, I_ENV);                    /* SUST: remember before TE's decay step */
    h[1] = W16(self, I_CHAIN);                  /* ITER: same for the chain gain */
    if (p[3]) {                                 /* FEED: loop gain x1 .. x4 */
        g = 4096 + (int32_t)p[3] * 3 * 4096 / 32767;   /* Q12 */
        for (i = 0; i < TE_VOICE_SAMPLES; i++)
            I_CHAINBUF[i] = sat16((I_CHAINBUF[i] * g) >> 12);
    }
}

static void iter_after(void *self, const int16_t *p, int32_t *h, int16_t *kc)
{
    int32_t e = h[0], c = h[1];

    (void)kc;
    /* SUST / ITER: keep (32767 - p) / 32768 of TE's per-block decay step: max = frozen */
    if (p[1])
        W16(self, I_ENV) = (int16_t)(e + (((W16(self, I_ENV) - e) * (32767 - (int32_t)p[1])) >> 15));
    if (p[2])
        W16(self, I_CHAIN) = (int16_t)(c + (((W16(self, I_CHAIN) - c) * (32767 - (int32_t)p[2])) >> 15));
}

static void iter_note(void *self, const int16_t *p, int32_t *h, int16_t *kc)
{
    (void)h; (void)kc;
    /* POLAR: chain feedback and FM direction from +c through 0 to -c */
    if (p[0])
        W16(self, I_CHAIN) = (int16_t)((W16(self, I_CHAIN) * (32767 - 2 * (int32_t)p[0])) >> 15);
}

/* ------------------------------------------------------------------ fm
 * Shared table block S (pointer at 0xff906f6c, right after the 6 voices): int16 rates[6] (LFO),
 * then ratio tables A[8] (op4), B[8] (op3), C[8] (op2), value / 8192 = frequency ratio; the green
 * knob only selects entries. TE copies S from the pristine block at 0x01d05880 when the engine is
 * created. S is shared by all voices: rebuild it from the pristine copy before each voice block
 * and restore it afterwards, so slots at 0 always mean stock. */
#define FM_S        (*(int16_t *volatile *)0xff906f6c)
#define FM_STOCK    ((const volatile int16_t *)0x01d05880)
#define FM_WORDS    30

static int16_t scale(int16_t v, int32_t p, int32_t times)    /* v * (1 + (times - 1) p / 32767) */
{
    int32_t r = v + (int32_t)(((int64_t)v * (times - 1) * p) / 32767);
    return sat16(r);
}

static void fm_before(void *self, const int16_t *p, int32_t *h, int16_t *kc)
{
    int16_t *S = FM_S;
    int i;

    (void)self; (void)h; (void)kc;
    for (i = 0; i < FM_WORDS; i++)
        S[i] = FM_STOCK[i];
    for (i = 0; i < 8; i++) {
        if (p[0]) S[6 + i] = scale(FM_STOCK[6 + i], p[0], 4);       /* RATIO4: table A */
        if (p[1]) S[14 + i] = scale(FM_STOCK[14 + i], p[1], 4);     /* RATIO3: table B */
        if (p[2]) S[22 + i] = scale(FM_STOCK[22 + i], p[2], 4);     /* RATIO2: table C */
    }
    if (p[3])
        for (i = 0; i < 6; i++)
            S[i] = scale(FM_STOCK[i], p[3], 64);                    /* WOBRT: LFO rates */
}

static void fm_after(void *self, const int16_t *p, int32_t *h, int16_t *kc)
{
    int16_t *S = FM_S;
    int i;

    (void)self; (void)p; (void)h; (void)kc;
    for (i = 0; i < FM_WORDS; i++)
        S[i] = FM_STOCK[i];
}

/* ------------------------------------------------------------------ digital
 * voice 0x20: +04 osc A phase, +0c osc B phase (hard-synced to A while k2 > 0), +14 osc C phase
 * (B's frequency, never synced, free-running). B/C ratio: 2^((k1 >> 12) - 3) + k2-detune. */
#define D_A         0x04
#define D_B         0x0c
#define D_C         0x14

static void digital_before(void *self, const int16_t *p, int32_t *h, int16_t *kc)
{
    int32_t k;

    (void)h;
    if (p[0]) {                                 /* SUBOCT: B/C down up to 8 octaves */
        k = kc[1] - 4096 * (((int32_t)p[0] * 8 + 16383) / 32767);
        kc[1] = (int16_t)(k < -32768 ? -32768 : k);
    }
    if (p[1]) {                                 /* FREE: no sync, B/C detuned down to -6.25 % */
        k = kc[2] + p[1];
        kc[2] = (int16_t)-(k > 32767 ? 32767 : k);
    }
    if (p[2])                                   /* XDRIFT: C beats against B (~0..10 Hz) */
        W32(self, D_C) += (int32_t)p[2] * 3800;
    if (p[3] && W32(self, D_A) == (int32_t)0x80000000 && W32(self, D_B) == (int32_t)0x80000000)
        W32(self, D_C) = (int32_t)(0x80000000u + ((uint32_t)p[3] << 17));   /* XPHASE: new note */
}

const engine ep_hidden[] = {
    { 0x0190ff84, (render_fn)0x019a23d4, TE_INSTR_FM, { "RATIO4", "RATIO3", "RATIO2", "WOBRT" },
      fm_before, fm_after, 0, 0 },
    { 0x01911a34, (render_fn)0x01a51f7a, TE_INSTR_DIGITAL, { "SUBOCT", "FREE", "XDRIFT", "XPHASE" },
      digital_before, 0, 0, 0 },
    { 0x0191185c, (render_fn)0x01a49bf4, TE_INSTR_VOLTAGE, { "SHAPE", "SPREAD", "DRIFT", "FBPOL" },
      voltage_before, voltage_after, 0, 0 },
    { 0x019103cc, (render_fn)0x019b99d4, TE_INSTR_ITER, { "POLAR", "SUST", "ITER", "FEED" },
      iter_before, iter_after, (note_fn)0x019b9b8a, iter_note },
    { 0, 0, 0, { 0, 0, 0, 0 }, 0, 0, 0, 0 },    /* end */
};
