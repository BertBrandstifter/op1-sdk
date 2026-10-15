/* Engine-layer bench (build/apps/op1plus): ep_render around a fake fm voice, knob click paging,
 * knob edits of slots 5-8 + TE's save call, dsynth routed to TE's controller (id 4..7), and the
 * DSP cost per 128-sample voice block in CPU cycles. Returns failed-check bits (0 = pass). */
#include <op1/te246.h>

extern void ep_render(void *self, int16_t *out, int16_t *knobs);
extern void hook_key(int key, int down);
extern void hook_knob(int knob, int16_t *diff);
extern volatile uint8_t ep_page[4];

static int saves, keys_seen, knobs_seen, ctl_calls, ctl_id;
static int16_t *saved_knobs;
static int16_t out[128], ref[128];
volatile uint32_t cycles_off, cycles_on;        /* read by run_eptest.py */
static int fails;

void fake_render(void *self, int16_t *o, int16_t *k)
{
    int i;
    (void)self; (void)k;
    for (i = 0; i < 128; i++)
        o[i] = (int16_t)((i * 509) - 32000);    /* loud saw */
}
void log_save(void *patch, int16_t *k) { (void)patch; saves++; saved_knobs = k; }
static int16_t fm_S[30], fm_seen[30];
void fm_render(void *self, int16_t *o, int16_t *k)       /* records the ratio table it renders with */
{
    int i;
    (void)self; (void)o; (void)k;
    for (i = 0; i < 30; i++)
        fm_seen[i] = fm_S[i];
}
void log_key(int key, int down) { (void)key; (void)down; keys_seen++; }
void log_knob(int knob, int16_t *diff) { (void)knob; (void)diff; knobs_seen++; }
static void ctl_knob(void *self, int id, int16_t *diff) { (void)self; (void)diff; ctl_calls++; ctl_id = id; }

static uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)

int main(void)
{
    static uint32_t voice[16] = { 0x019118c4 };          /* fake string voice (generic stages) */
    static uint32_t vt[16];
    static uint32_t ctl[2];
    int16_t knobs[8] = { 0 }, up = 1024;
    uint32_t t0;
    int i, same;

    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");  /* CCEN */

    /* 0: layer off -> output is exactly TE's render */
    fake_render(0, ref, 0);
    t0 = cycles();
    ep_render(voice, out, knobs);
    cycles_off = cycles() - t0;
    same = 1;
    for (i = 0; i < 128; i++)
        same &= out[i] == ref[i];
    CHECK(0, same);

    /* 1: all four stages on: output changes, stays in range; cost measured */
    knobs[4] = 20000; knobs[5] = 16000; knobs[6] = 20000; knobs[7] = 8000;
    ep_render(voice, out, knobs);               /* warm-up (state allocation) */
    t0 = cycles();
    ep_render(voice, out, knobs);
    cycles_on = cycles() - t0;
    same = 1;
    for (i = 0; i < 128; i++)
        same &= out[i] == ref[i];
    CHECK(1, !same);

    /* synth engine screen, sound 2 = string */
    *TE_EDIT_MODE = 0;
    *(volatile int32_t *)0x01b4ee88 = 0;
    *TE_SYNTH_SLOT = 2;
    TE_SYNTH_TYPE[2] = TE_INSTR_STRING;

    /* 2: click the green knob (60): flipped, consumed (down and up) */
    hook_key(60, 1);
    hook_key(60, 0);
    CHECK(2, ep_page[1] == 1 && keys_seen == 0);
    /* 3: green knob turns now edit slot 6 of sound 2 and save like TE */
    hook_knob(1, &up);
    hook_knob(1, &up);
    CHECK(3, TE_SYNTH_KNOBS[2 * 8 + 5] == 2 * ((1024 * 10485) >> 15) && saves == 2
             && saved_knobs == (int16_t *)TE_SYNTH_KNOBS + 16 && knobs_seen == 0);
    /* 4: blue knob is not flipped: TE's */
    hook_knob(0, &up);
    CHECK(4, knobs_seen == 1);
    /* 5: dsynth: flipped green goes to TE's controller as id 5 (= shift + green) */
    TE_SYNTH_TYPE[2] = TE_INSTR_DSYNTH;
    vt[0x28 / 4] = 0;                          /* this-delta 0 (int16 at +0x28) */
    vt[0x2c / 4] = (uint32_t)ctl_knob;
    ctl[1] = (uint32_t)vt;
    *TE_CONTROLLER = ctl;
    hook_knob(1, &up);
    CHECK(5, ctl_calls == 1 && ctl_id == 5 && saves == 2);
    /* 6: other screen: knob clicks and turns belong to TE */
    *TE_EDIT_MODE = 4;
    hook_key(61, 1);
    hook_key(61, 0);
    hook_knob(1, &up);
    CHECK(6, keys_seen == 2 && knobs_seen == 2 && ep_page[2] == 0);
    /* 7: click green again on the synth screen: back to normal */
    *TE_EDIT_MODE = 0;
    hook_key(60, 1);
    hook_key(60, 0);
    hook_knob(1, &up);
    CHECK(7, ep_page[1] == 0 && knobs_seen == 3);

    /* 8/9: fm hidden parameters. Pristine table at 0x01d05880, live table S via 0xff906f6c.
     * RATIO4 at max scales table A (words 6..13) by 4 (saturating) during the render only. */
    {
        static uint32_t fmv[16] = { 0x0190ff84 };
        volatile int16_t *stock = (volatile int16_t *)0x01d05880;
        int16_t k8[8] = { 0, 0, 0, 0, 32767, 0, 0, 0 };
        int ok = 1;

        for (i = 0; i < 30; i++)
            stock[i] = fm_S[i] = (int16_t)(1000 + i);
        *(int16_t *volatile *)0xff906f6c = fm_S;
        ep_render(fmv, out, k8);
        for (i = 0; i < 30; i++)
            ok &= fm_seen[i] == (i >= 6 && i < 14 ? 4 * (1000 + i) : 1000 + i) && fm_S[i] == 1000 + i;
        CHECK(8, ok);
        k8[4] = 0;
        fm_S[7] = 1;                            /* off: render sees the table untouched */
        ep_render(fmv, out, k8);
        CHECK(9, fm_seen[7] == 1);
    }
    return fails;
}
