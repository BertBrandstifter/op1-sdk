/* LOFI bench (build/apps/op1plus): pulse voice render/note_on wrappers against stub TE functions
 * and TE's per-sample pitch buffer, the on/off gesture, page pushes, knob edits + TE's save, and
 * the cost per 128-sample voice block in CPU cycles. Returns failed-check bits (0 = pass). */
#include <op1/te246.h>

extern void lf_te_render(void *self, int16_t *out, int16_t *knobs);
extern void lf_te_note_on(void *self, int a1, int a2);
extern int lf_te_range_check(int type, int slot, const int16_t *v);
extern void lf_te_type_fetched(te_string *out, void *db, const char *sql);
extern void hook_key(int key, int down);
extern void hook_knob(int knob, int16_t *diff);
extern void hook_frame(uint16_t *buf);
extern char FB[];
extern volatile uint8_t lf_page, ep_page[4];
extern volatile int8_t lf_bank;

#define INC         ((volatile uint32_t *)0xff801950)
#define VOICE(i)    ((void *)(0xffb00000u + (i) * 0x60))

static int pulse_renders, pulse_notes, saves, keys_seen, knobs_seen;
static int16_t out[128], first[128];
volatile uint32_t cycles_pulse, cycles_heavy;   /* read by run_lofitest.py */
volatile int fails;                             /* read by run_lofitest.py */

void pulse_render(void *self, int16_t *o, int16_t *k)
{
    int i;
    (void)self; (void)k;
    pulse_renders++;
    for (i = 0; i < 128; i++)
        o[i] = 1234;
}
void pulse_note_on(void *self, int a1, int a2) { (void)self; (void)a1; (void)a2; pulse_notes++; }
void log_save(void *patch, int16_t *k) { (void)patch; (void)k; saves++; }
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

static void turn(int knob, int16_t d)
{
    hook_knob(knob, &d);
}

/* what picking LOFI in TE's lists stores (engine-list defaults: PULSE 50 %, page 2 off) */
static void lofi_on(volatile int16_t *k)
{
    int i;
    for (i = 0; i < 8; i++)
        k[i] = (int16_t)(i < 4 ? 0 : ~0);
}

/* a plain pulse sound (TE's pulse defaults) */
static void pulse_sound(volatile int16_t *k)
{
    static const int16_t d[8] = { 20000, 16000, 4000, 3000, 0, 0, 0, 0 };
    int i;
    for (i = 0; i < 8; i++)
        k[i] = d[i];
}

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)
#define K(i)        TE_SYNTH_KNOBS[2 * 8 + (i)]  /* sound 3 */

int main(void)
{
    int16_t knobs[8] = { 0 };
    int i, n, low, ok;
    uint32_t t0;

    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");  /* CCEN */
    *TE_EDIT_MODE = 0;
    *TE_SYNTH_SUBSCREEN = 0;
    *TE_SYNTH_SLOT = 2;
    TE_SYNTH_TYPE[2] = TE_INSTR_PULSE;

    /* 0: plain pulse sound -> TE's pulse render, untouched */
    lf_te_render(VOICE(0), out, knobs);
    CHECK(0, pulse_renders == 1 && out[0] == 1234 && out[127] == 1234);

    /* 1: note_on goes to TE's pulse note_on */
    lf_te_note_on(VOICE(1), 0, 0);
    CHECK(1, pulse_notes == 1);

    /* 2: no gesture any more (v0.7): shift + white push on a pulse sound reaches TE, knobs
     * unchanged; LOFI comes from TE's lists (engine row / preset), which store the marker */
    pulse_sound(&K(0));
    push(61, 1);
    CHECK(2, K(4) == 0 && keys_seen == 2 && saves == 0);
    lofi_on(&K(0));

    /* 3: LOFI renders from TE's pitch buffer: 441 Hz at 88.2 kHz -> 441 cycles in 689 blocks */
    for (i = 0; i < 8; i++)
        knobs[i] = K(i);
    pitch((uint32_t)(441.0 / 88200 * 4294967296.0));
    lf_te_note_on(VOICE(1), 0, 0);
    pulse_renders = 0;
    for (n = 0, low = 1, i = 0; i < 689; i++) {
        int j;
        lf_te_render(VOICE(1), out, knobs);
        if (i == 0)
            for (j = 0; j < 128; j++)
                first[j] = out[j];
        for (j = 0; j < 128; j++) {
            if (low && out[j] > 200) { n++; low = 0; }
            else if (out[j] < -200) low = 1;
        }
    }
    CHECK(3, pulse_renders == 0 && n >= 440 && n <= 442);

    /* 4: a new note restarts the voice: same first block */
    lf_te_note_on(VOICE(1), 0, 0);
    lf_te_render(VOICE(1), out, knobs);
    for (i = 0, ok = 1; i < 128; i++)
        ok &= out[i] == first[i];
    CHECK(4, ok && pulse_notes == 3);

    /* 5: knob push on a LOFI sound flips the page; engine_plus does not see it */
    push(59, 0);
    CHECK(5, lf_page == 1 && ep_page[0] == 0 && keys_seen == 2);

    /* 6: page 2, blue knob: slot 5 value up (stored as ~v), saved, TE's knob handler skipped */
    i = saves;
    turn(0, 1024);
    CHECK(6, K(4) < 0 && (int16_t)~K(4) > 0 && saves == i + 1 && knobs_seen == 0);

    /* 7: page 1, white knob: slot 3 (CRUSH) up; clamped at the top */
    push(61, 0);
    for (i = 0; i < 4; i++)
        turn(2, 32767);
    CHECK(7, lf_page == 0 && K(2) == 32766 && knobs_seen == 0);

    /* 8: other screens are TE's: envelope subscreen knob goes to TE */
    *TE_SYNTH_SUBSCREEN = 1;
    turn(0, 100);
    CHECK(8, knobs_seen == 1);
    *TE_SYNTH_SUBSCREEN = 0;

    /* 9: cost: PULSE (defaults) and BYTE + crush + bits + tone + bend + loop + sweep */
    for (i = 0; i < 8; i++)
        knobs[i] = K(i);
    knobs[2] = 0;
    t0 = cycles();
    lf_te_render(VOICE(1), out, knobs);
    cycles_pulse = cycles() - t0;
    knobs[0] = 32000; knobs[1] = 9000; knobs[2] = 3000; knobs[3] = 12000;
    knobs[4] = ~12000; knobs[5] = ~20000; knobs[6] = ~8000; knobs[7] = ~15000;
    t0 = cycles();
    lf_te_render(VOICE(1), out, knobs);
    cycles_heavy = cycles() - t0;
    CHECK(9, cycles_heavy < 40000);

    /* 10: another preset picked (plain pulse): render back to TE's pulse */
    pulse_sound(&K(0));
    for (i = 0; i < 8; i++)
        knobs[i] = K(i);
    pulse_renders = 0;
    lf_te_render(VOICE(1), out, knobs);
    CHECK(10, K(4) == 0 && K(7) == 0 && pulse_renders == 1);

    /* 11: plain pulse again: knob push goes to engine_plus (its paging), turns to TE */
    i = knobs_seen;
    turn(1, 100);
    push(60, 0);
    CHECK(11, knobs_seen == i + 1 && ep_page[1] == 1);
    push(60, 0);

    /* 12: shift + white push on another engine is not LOFI's (reaches TE) */
    TE_SYNTH_TYPE[2] = TE_INSTR_FM;
    i = keys_seen;
    push(61, 1);
    CHECK(12, K(4) == 0 && keys_seen == i + 2);
    /* 13: TE's knob range check (replaced): same answers from TE's table, plus LOFI's page-2
     * values in pulse slots 5-8 (the boot-time patch validator calls it for every knob) */
    {
        volatile int16_t *rt = (volatile int16_t *)0x01de5890;
        int16_t m1 = -1, m2 = -32767, mn = -32768, pos = 100, big = 20000;

        for (i = 0; i < 15 * 32; i += 4) {
            rt[i] = 0;
            rt[i + 1] = 16384;
        }
        CHECK(13, lf_te_range_check(TE_INSTR_PULSE, 4, &m1) && lf_te_range_check(TE_INSTR_PULSE, 7, &m2)
                  && !lf_te_range_check(TE_INSTR_PULSE, 4, &mn)    /* -32768 never stored */
                  && !lf_te_range_check(TE_INSTR_PULSE, 0, &m1)    /* slots 1-4: no negatives */
                  && lf_te_range_check(TE_INSTR_PULSE, 0, &big)    /* LOFI page 1: 0..32767 */
                  && !lf_te_range_check(TE_INSTR_FM, 0, &big)      /* other engines: TE's range */
                  && !lf_te_range_check(TE_INSTR_FM, 4, &m1)       /* other engines: TE's range */
                  && lf_te_range_check(TE_INSTR_PULSE, 4, &pos)
                  && !lf_te_range_check(TE_INSTR_PULSE, 4, &big)   /* slots 5-8 positive: TE's */
                  && !lf_te_range_check(TE_INSTR_PULSE, 8, &pos) && !lf_te_range_check(TE_INSTR_PULSE, -1, &pos));
    }
    /* 14: with TE's real pulse ranges (simulated Ranges init 0x01a1e914): a LOFI sound passes
     * the validator in every slot */
    {
        static const int16_t pr[8][2] = { { 7808, 23168 }, { 512, 16384 }, { 0, 16384 },
                                          { -16384, 16384 }, { 0, 32767 }, { 0, 32767 },
                                          { 0, 32767 }, { 0, 32767 } };
        volatile int16_t *rt = (volatile int16_t *)0x01de5890 + TE_INSTR_PULSE * 32;
        int16_t v;

        for (i = 0; i < 8; i++) {
            rt[i * 4] = pr[i][0];
            rt[i * 4 + 1] = pr[i][1];
        }
        TE_SYNTH_TYPE[2] = TE_INSTR_PULSE;
        lofi_on(&K(0));                         /* LOFI: slot 1 = 0 (below pulse's 7808) */
        for (i = 0, ok = K(0) == 0; i < 8; i++) {
            v = K(i);
            ok &= lf_te_range_check(TE_INSTR_PULSE, i, &v);
        }
        CHECK(14, ok);
    }
    /* 15: engine browser: the picked row's type "lofi" becomes "pulse"; other names untouched */
    {
        static te_string a, b, c;
        static const char na[] = "lofi", nb[] = "pulse", nc[] = "lofix";

        for (i = 0; i < 6; i++) {
            a.bx.sso[i] = na[i < 5 ? i : 4];
            b.bx.sso[i] = nb[i];
            c.bx.sso[i] = nc[i];
        }
        a.size = 4; b.size = c.size = 5;
        a.cap = b.cap = c.cap = 15;

        lf_page = 1;
        lf_te_type_fetched(&a, 0, 0);
        lf_te_type_fetched(&b, 0, 0);
        lf_te_type_fetched(&c, 0, 0);
        CHECK(15, a.size == 5 && a.bx.sso[0] == 'p' && a.bx.sso[4] == 'e' && a.bx.sso[5] == 0 && lf_page == 0
                  && b.size == 5 && b.bx.sso[0] == 'p' && c.size == 5 && c.bx.sso[0] == 'l');
    }
    /* 16: shift + blue turn on a LOFI sound browses the built-in bank (former factory presets):
     * entry 0 = CHIP LEAD exactly, saved; every entry passes the (replaced) range check */
    {
        static const int16_t chip[8] = { 655, 16000, 0, 0, -1, -1, -1, -6001 };
        int e, k;
        int16_t v;

        TE_SYNTH_TYPE[2] = TE_INSTR_PULSE;
        lofi_on(&K(0));
        i = saves;
        *TE_SHIFT_HELD = 1;
        turn(0, 1024);
        for (k = 0, ok = lf_bank == 0 && saves == i + 1; k < 8; k++)
            ok &= K(k) == chip[k];
        for (e = 0; e < 14; e++) {
            for (k = 0; k < 8; k++) {
                v = K(k);
                ok &= lf_te_range_check(TE_INSTR_PULSE, k, &v);
            }
            turn(0, 1024);
        }
        *TE_SHIFT_HELD = 0;
        CHECK(16, ok && lf_bank == 0);
    }
    /* 17: the LOFI screen draws (bank entry TRI BASS, page 1, a note in the scope) - dumped as a
     * screenshot */
    *TE_SHIFT_HELD = 1;
    for (i = 0; i < 3; i++)
        turn(0, 1024);
    *TE_SHIFT_HELD = 0;
    for (i = 0; i < 8; i++)
        knobs[i] = K(i);
    pitch((uint32_t)(88200.0 / 64 / 88200 * 4294967296.0));   /* two periods per block */
    lf_te_note_on(VOICE(1), 0, 0);
    for (i = 0; i < 4; i++)
        lf_te_render(VOICE(1), out, knobs);
    lf_page = 0;
    hook_frame((uint16_t *)FB);
    CHECK(17, lf_bank == 3);
    return fails;
}
