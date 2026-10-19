/* M1 bench (build/apps/op1plus, SDK_BENCH: the built-in waves): engine host + M1 glue against stub
 * TE functions and TE's per-sample pitch buffer. Returns failed-check bits (0 = pass). */
#include <op1/te246.h>

extern void wh_te_render(void *self, int16_t *out, int16_t *knobs);    /* the engine host's */
extern void wh_te_note_on(void *self, int a1, int a2);
extern void compose_init(void);
extern void engine_name(te_string *out, void *db, const char *sql);    /* composer's chain */
extern void hook_key(int key, int down);
extern void hook_knob(int knob, int16_t *diff);
extern void hook_frame(uint16_t *buf);
extern char FB[];
extern volatile uint8_t m1_page;
extern volatile int8_t m1_bank;

#define INC         ((volatile uint32_t *)0xff801950)
#define VOICE(i)    ((void *)(0xffb00000u + (i) * 0x24))
#define P2(t, v)    ((int16_t)(((t) << 12) | (v)))

static int drwave_renders, drwave_notes, saves, fxsaves, keys_seen, knobs_seen;
static int16_t out[128], first[128];
volatile uint32_t cycles_plain, cycles_two, cycles_heavy;     /* read by run_m1test.py */
volatile int fails;

void drwave_render(void *self, int16_t *o, int16_t *k)
{
    int i;
    (void)self; (void)k;
    drwave_renders++;
    for (i = 0; i < 128; i++)
        o[i] = 1234;
}
void drwave_note_on(void *self, int a1, int a2) { (void)self; (void)a1; (void)a2; drwave_notes++; }
void log_save(void *patch, int16_t *k) { (void)patch; (void)k; saves++; }
void log_fxsave(void *patch, int16_t *f) { (void)patch; (void)f; fxsaves++; }
void log_key(int key, int down) { (void)key; (void)down; keys_seen++; }
void log_knob(int knob, int16_t *diff) { (void)knob; (void)diff; knobs_seen++; }

static uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

static void pitch(uint32_t inc)
{
    int i;
    for (i = 0; i < 128; i++)
        INC[i] = inc;
}

static void push(int key, int shift)
{
    *TE_SHIFT_HELD = (uint8_t)shift;
    hook_key(key, 1);
    hook_key(key, 0);
    *TE_SHIFT_HELD = 0;
}

static void turn(int knob, int16_t d) { hook_knob(knob, &d); }

static void name(te_string *s, const char *n)
{
    int i;

    for (i = 0; n[i]; i++)
        s->bx.sso[i] = n[i];
    s->bx.sso[i] = 0;
    s->size = (uint32_t)i;
    s->cap = 15;
}

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)
#define K(i)        TE_SYNTH_KNOBS[2 * 8 + (i)]  /* sound 3 */
#define F(hi, lo)   P2(3, (hi) << 6 | (lo))
#define Z1(z, n)    ((int16_t)(((2 * (z) + 1) * 32768 / (2 * (n))) >> 1))

int main(void)
{
    static const int16_t stock[8] = { 20924, 4313, 8063, 0, 32767, 0, 0, 0 };
    /* SAW (built-in wave 0), B off, filter open, B PITCH unison; page 3 all 0 */
    static const int16_t saw[8] = { Z1(0, 8), Z1(0, 9), 0, 16383, F(24, 0), F(0, 0), F(0, 0), F(0, 0) };
    int16_t knobs[8];
    int i, n, low, ok;
    uint32_t t0;

    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");  /* CCEN */
    compose_init();
    *TE_EDIT_MODE = 0;
    *TE_SYNTH_SUBSCREEN = 0;
    *TE_SYNTH_SLOT = 2;
    TE_SYNTH_TYPE[2] = TE_INSTR_DRWAVE;

    /* 0: plain drwave stays TE's */
    for (i = 0; i < 8; i++)
        knobs[i] = stock[i];
    wh_te_render(VOICE(0), out, knobs);
    CHECK(0, drwave_renders == 1 && out[0] == 1234);

    /* 1: tag 3 = M1: SAW at 441 Hz from TE's pitch buffer (rising zero crossings); restart */
    for (i = 0; i < 8; i++)
        K(i) = knobs[i] = saw[i];
    pitch((uint32_t)(441.0 / 88200 * 4294967296.0));
    wh_te_note_on(VOICE(1), 0, 0);
    drwave_renders = 0;
    for (n = 0, low = 1, i = 0; i < 689; i++) {
        int j;
        wh_te_render(VOICE(1), out, knobs);
        if (i == 0)
            for (j = 0; j < 128; j++)
                first[j] = out[j];
        for (j = 0; j < 128; j++) {
            if (low && out[j] > 4000) { n++; low = 0; }
            else if (out[j] < -4000) low = 1;
        }
    }
    CHECK(1, drwave_renders == 0 && n >= 439 && n <= 443);
    wh_te_note_on(VOICE(1), 0, 0);
    wh_te_render(VOICE(1), out, knobs);
    for (i = 0, ok = 1; i < 128; i++)
        ok &= out[i] == first[i];
    CHECK(1, ok);

    /* 2: knob push cycles 4 pages */
    i = keys_seen;
    push(59, 0);
    CHECK(2, m1_page == 1 && keys_seen == i);
    push(59, 0);
    push(59, 0);
    CHECK(2, m1_page == 3);
    push(59, 0);
    CHECK(2, m1_page == 0);

    /* 3: page 1 WAVE A / B one multisound per detent (B: off .. 8), CUTOFF clamps at 16383;
     * page 2 B PITCH one semitone per detent, max +24 (48), the low field (page 3) untouched,
     * tag kept; page 3 RES steps */
    turn(0, 1024);
    CHECK(3, K(0) == Z1(1, 8));
    for (n = 0; n < 20; n++)
        turn(0, 1024);
    CHECK(3, K(0) == Z1(7, 8));
    turn(0, -1024);
    turn(1, 1024);
    CHECK(3, K(0) == Z1(6, 8) && K(1) == Z1(1, 9));
    turn(1, -1024);
    CHECK(3, K(1) == Z1(0, 9));
    for (n = 0; n < 60; n++)
        turn(3, 32767);
    CHECK(3, K(3) == 16383 && knobs_seen == 0);
    push(59, 0);
    turn(0, 1024);
    CHECK(3, K(4) == F(25, 0));
    for (n = 0; n < 40; n++)
        turn(0, 1024);
    CHECK(3, K(4) == F(48, 0));
    push(59, 0);
    turn(0, 1024);
    CHECK(3, K(4) == F(48, 1) && (K(4) >> 12) == 3);
    turn(3, 1024);                              /* B FADE */
    CHECK(3, K(7) == F(0, 1));
    push(59, 0);
    push(59, 0);

    /* 4: shift + blue = bank, 8 entries (built-in waves) in drwave's ranges with tag 3; after the
     * last comes P-- (the sound as it was before browsing: its knobs back), then P01 again */
    {
        static const int16_t hi[8] = { 24575, 16383, 16383, 32767, 32767, 32767, 32767, 32767 };
        int16_t before[8];
        int e, k;

        for (k = 0; k < 8; k++)
            before[k] = K(k);
        *TE_SHIFT_HELD = 1;
        turn(0, 1024);
        ok = m1_bank == 0;
        for (e = 0; e < 8; e++) {
            for (k = 0; k < 8; k++)
                ok &= K(k) >= 0 && K(k) <= hi[k];
            for (k = 4; k < 8; k++)
                ok &= (K(k) >> 12) == 3;
            turn(0, 1024);
        }
        ok &= m1_bank == -1;                    /* P-- */
        for (k = 0; k < 8; k++)
            ok &= K(k) == before[k];
        turn(0, -1024);
        ok &= m1_bank == 7;                     /* back: the last entry */
        turn(0, 1024);
        ok &= m1_bank == -1;
        *TE_SHIFT_HELD = 0;
        CHECK(4, ok && knobs_seen == 0);
    }

    /* 5: page 4 = FX knobs */
    {
        volatile int16_t *rt = (volatile int16_t *)0x01de5c50 + TE_FX_SPRING * 24;
        volatile int16_t *fx = (volatile int16_t *)0xff801444 + 2 * 8;

        rt[0] = 1344; rt[1] = 16448;
        fx[0] = 8000;
        ((volatile int32_t *)0xff801734)[2] = TE_FX_SPRING;
        ((volatile uint8_t *)0x01aaac20)[2] = 1;
        m1_page = 3;
        for (n = 0; n < 200; n++)
            turn(0, 32767);
        CHECK(5, fx[0] == 16448 && fxsaves == 200);
        m1_page = 0;
    }

    /* 6: engine list: "station 1" -> "drwave", "trio" too (the chain), "station" / "station 2"
     * untouched */
    {
        static te_string a, b, c, e;

        name(&a, "station 1"); name(&b, "trio"); name(&c, "station"); name(&e, "station 2");
        engine_name(&a, 0, 0);
        engine_name(&b, 0, 0);
        engine_name(&c, 0, 0);
        engine_name(&e, 0, 0);
        CHECK(6, a.size == 6 && a.bx.sso[0] == 'd' && a.bx.sso[6] == 0 && b.size == 6 && c.size == 7
                 && c.bx.sso[0] == 's' && e.size == 9 && e.bx.sso[0] == 's');
    }

    /* 7: cost per voice block: one wave, filter open; two waves + filter; + resonance, filter env,
     * key track, A decay, B fade */
    for (i = 0; i < 8; i++)
        knobs[i] = saw[i];
    wh_te_note_on(VOICE(1), 0, 0);
    wh_te_render(VOICE(1), out, knobs);
    t0 = cycles();
    wh_te_render(VOICE(1), out, knobs);
    cycles_plain = cycles() - t0;
    knobs[1] = Z1(5, 9); knobs[2] = 8000; knobs[3] = 9000; knobs[4] = F(31, 0); knobs[5] = F(20, 0);
    wh_te_note_on(VOICE(1), 0, 0);
    wh_te_render(VOICE(1), out, knobs);
    t0 = cycles();
    wh_te_render(VOICE(1), out, knobs);
    cycles_two = cycles() - t0;
    knobs[4] = F(31, 40); knobs[5] = F(20, 40); knobs[6] = F(50, 30); knobs[7] = F(30, 30);
    wh_te_note_on(VOICE(1), 0, 0);
    t0 = cycles();
    wh_te_render(VOICE(1), out, knobs);
    cycles_heavy = cycles() - t0;
    CHECK(7, cycles_heavy < 30000);

    /* 8: the engine screen draws (bank entry 2, page 1) */
    *TE_SHIFT_HELD = 1;
    turn(0, 1024);
    turn(0, 1024);
    *TE_SHIFT_HELD = 0;
    m1_page = 0;
    hook_frame((uint16_t *)FB);
    CHECK(8, m1_bank == 1);

    /* 9: second screen (run_m1test.py: m1_screen_fx): a layered bank sound (SINE PAD: B a fifth up,
     * B fade) on page 4, into a second buffer */
    *TE_SHIFT_HELD = 1;
    for (n = 0; n < 3; n++)
        turn(0, 1024);
    *TE_SHIFT_HELD = 0;
    m1_page = 3;
    hook_frame((uint16_t *)(FB + 0x40000));
    m1_page = 0;
    CHECK(9, m1_bank == 4);

    /* 10: keyboard picture: a held key (id 41: note 4 = A, TE octave +1 -> A4) lights A4 even long
     * after the start, though its voice starts 2 semitones low (glide), and goes dark on release while
     * TE still renders the voice */
    {
        volatile uint16_t *fb = (volatile uint16_t *)FB;
        const uint16_t green = (uint16_t)(((0x3d >> 3) << 11) | ((0xdc >> 2) << 5) | (0x97 >> 3));
        int lit_held, lit_after;

        hook_key(41, 1);
        pitch((uint32_t)(392.0 / 88200 * 4294967296.0));
        wh_te_note_on(VOICE(2), 0, 0);
        for (n = 0; n < 300; n++)               /* > FLASH blocks */
            wh_te_render(VOICE(2), out, knobs);
        hook_frame((uint16_t *)FB);
        lit_held = fb[(319 - 188) * 256 + 72] == green;   /* A4: white key 19, lower part */
        hook_key(41, 0);
        for (n = 0; n < 10; n++)
            wh_te_render(VOICE(2), out, knobs);
        hook_frame((uint16_t *)FB);
        lit_after = fb[(319 - 188) * 256 + 72] == green;
        CHECK(10, lit_held && !lit_after);
    }
    return fails;
}
