/* cpuinfo bench (recipes/op1cpu.json): the audio callback wrap, the per-window numbers, the SDK
 * engines' share (an ACID voice rendered inside the callback), a re-set callback re-wrapped, and
 * the panel drawn. Returns failed-check bits (0 = pass). */
#include <op1/te246.h>

extern void compose_init(void);
extern void hook_key(int key, int down);
extern void hook_frame(uint16_t *buf);
extern void wh_te_render(void *self, int16_t *out, int16_t *knobs);
extern void wh_te_note_on(void *self, int a1, int a2);
extern void ac_bank_knobs(int e, int16_t *k);
extern void ci_main(void);
extern volatile uint32_t ci_avg_w, ci_avg_p, ci_avg_e, ci_windows, ci_rewraps;
extern volatile uint8_t ci_ccen_was;
extern char FB[];

#define INC         ((volatile uint32_t *)0xff801950)
#define VOICE(i)    ((void *)(0xffb00000u + (i) * 0x24))
#define WIN         172

static int16_t out[128], knobs[8];
static int32_t tx[128], rx[128];
static int calls_a, calls_b;
volatile int fails;
volatile uint32_t sink;

/* stand-ins the shared stubs forward to (te_stubs.S of the ACID bench) */
void drwave_render(void *self, int16_t *o, int16_t *k) { (void)self; (void)o; (void)k; }
void drwave_note_on(void *self, int a1, int a2) { (void)self; (void)a1; (void)a2; }
void log_save(void *patch, int16_t *k) { (void)patch; (void)k; }
void log_fxsave(void *patch, int16_t *f) { (void)patch; (void)f; }
void log_key(int key, int down) { (void)key; (void)down; }
void log_knob(int knob, int16_t *diff) { (void)knob; (void)diff; }

static void burn(int n)
{
    int i;

    for (i = 0; i < n; i++)
        sink += (uint32_t)i;
}

/* TE's audio callback: some mixing work and one ACID voice */
static void te_cb_a(int32_t *t, const int32_t *r)
{
    (void)t; (void)r;
    calls_a++;
    burn(2000);
    wh_te_render(VOICE(0), out, knobs);
}

static void te_cb_b(int32_t *t, const int32_t *r) { (void)t; (void)r; calls_b++; burn(500); }

static void blocks(int n)
{
    int i;

    for (i = 0; i < n; i++) {
        (*TE_AUDIO_BLOCK_CB)(tx, rx);
        burn(6000);                             /* the rest of the block: other threads, idle */
    }
}

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)

int main(void)
{
    int i;

    compose_init();
    *TE_EDIT_MODE = 2;                          /* tape: no engine screen draws */
    ac_bank_knobs(0, knobs);
    for (i = 0; i < 128; i++)
        INC[i] = 21426141u;
    wh_te_note_on(VOICE(0), 0, 0);              /* a sounding ACID voice */
    *TE_AUDIO_BLOCK_CB = te_cb_a;

    /* 0: nothing before the panel opens */
    ci_main();
    blocks(4);
    CHECK(0, *TE_AUDIO_BLOCK_CB == te_cb_a && calls_a == 4 && ci_ccen_was == 0);

    /* 1: shift + speech: wrapped at the next main tick, TE's callback still runs every block */
    *TE_SHIFT_HELD = 1;
    hook_key(TE_KEY_SPEECH, 1);
    hook_key(TE_KEY_SPEECH, 0);
    *TE_SHIFT_HELD = 0;
    ci_main();
    CHECK(1, *TE_AUDIO_BLOCK_CB != te_cb_a);
    blocks(2 * WIN + 2);
    CHECK(1, calls_a == 4 + 2 * WIN + 2);

    /* 2: the cycle counter was off in the sim: set; two windows; work < period; engines < work */
    CHECK(2, ci_ccen_was == 2 && ci_windows == 2);
    CHECK(3, ci_avg_w > 0 && ci_avg_p > ci_avg_w && ci_avg_e > 0 && ci_avg_e < ci_avg_w);

    /* 4: TE re-sets its callback: wrapped again, the new one runs */
    *TE_AUDIO_BLOCK_CB = te_cb_b;
    ci_main();
    blocks(WIN + 1);
    CHECK(4, *TE_AUDIO_BLOCK_CB != te_cb_b && calls_b == WIN + 1 && ci_rewraps == 1 && ci_windows == 3);

    /* the panel (a window of te_cb_a again for the numbers) */
    *TE_AUDIO_BLOCK_CB = te_cb_a;
    ci_main();
    blocks(WIN);
    hook_frame((uint16_t *)FB);
    return fails;
}
