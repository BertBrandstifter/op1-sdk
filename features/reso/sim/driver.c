/* RESO bench (build/apps/op1plus): CWO process wrappers against stub TE functions and TE's voice
 * tables, the FX list rename, page-2 pushes and knob edits + TE's save, and the cost per 64-sample
 * FX block in CPU cycles. Returns failed-check bits (0 = pass). */
#include <op1/te246.h>

extern void rs_te_mono(void *self, int16_t *buf, int16_t *params);
extern void rs_te_stereo(void *self, int16_t *l, int16_t *r, int16_t *params);
extern void rs_te_fx_fetched(te_string *out, void *db, const char *sql);
extern void hook_key(int key, int down);
extern void hook_knob(int knob, int16_t *diff);
extern volatile uint8_t rs_page;

#define SYNTH_FX    ((volatile int16_t *)0xff801444)
#define DRUM_FX     ((volatile int16_t *)0xff8015c4)
#define MASTER_FX   ((volatile int16_t *)0xff8016c4)
#define FX_TYPE     ((volatile int32_t *)0xff801734)
#define FX_ON       ((volatile uint8_t *)0x01aaac20)
#define VOICE_ON    ((volatile uint8_t *)0xff80105c)
#define VOICE_INC   ((volatile uint32_t *)0xff8012d4)

static int cwo_monos, cwo_stereos, saves, keys_seen, knobs_seen;
static int16_t buf[64], l[64], r[64];
static uint32_t cwo[64];                        /* fake CWO object: pos at +0xe4 */
volatile uint32_t cycles, cycles_tune;          /* read by run_rstest.py */
volatile int fails;

void cwo_mono(void *self, int16_t *b, int16_t *p) { (void)self; (void)b; (void)p; cwo_monos++; }
void cwo_stereo(void *self, int16_t *a, int16_t *c, int16_t *p) { (void)self; (void)a; (void)c; (void)p; cwo_stereos++; }
static void *last_patch;
void log_save(void *patch, int16_t *p) { (void)p; last_patch = patch; saves++; }
void log_key(int key, int down) { (void)key; (void)down; keys_seen++; }
void log_knob(int knob, int16_t *diff) { (void)knob; (void)diff; knobs_seen++; }

static uint32_t now(void)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

static uint32_t ph;

/* one block of a 220 Hz triangle (on) or silence */
static void tri(int16_t *b, int on)
{
    int i;

    for (i = 0; i < 64; i++) {
        ph += 21426141u;                        /* 220 Hz at 44.1 kHz */
        b[i] = on ? (int16_t)((((ph < 0x80000000u ? ph : ~ph) >> 15) - 32768) / 4) : 0;
    }
}

static int peak(const int16_t *b)
{
    int i, m = 0;

    for (i = 0; i < 64; i++)
        m = b[i] > m ? b[i] : -b[i] > m ? -b[i] : m;
    return m;
}

static void mark(volatile int16_t *f, int mix)
{
    f[0] = 24000; f[1] = 30000; f[2] = 0; f[3] = (int16_t)mix;
    f[4] = 0x2000; f[5] = 0x2000 | 4096; f[6] = 0x2000; f[7] = 0x2000;
}

static void push(int key)
{
    hook_key(key, 1);
    hook_key(key, 0);
}

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)

int main(void)
{
    int16_t te[8];
    int i, k, pk;
    uint32_t t0;

    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");  /* CCEN */
    *TE_SYNTH_SLOT = 1;
    *TE_DRUM_SLOT = 2;
    cwo[0xe4 / 4] = 0;                          /* synth FX */

    /* 0: plain CWO (no marker) -> TE's CWO process, block untouched */
    tri(buf, 1);
    k = buf[10];
    for (i = 0; i < 8; i++)
        te[i] = SYNTH_FX[8 + i];
    rs_te_mono(cwo, buf, te);
    CHECK(0, cwo_monos == 1 && buf[10] == k);

    /* 1: RESO as synth FX: a 220 Hz note into strings tuned from the voices (220 Hz playing),
     * then the voice stops: the strings ring on, TE's CWO never runs */
    mark(SYNTH_FX + 8, 32767);
    for (i = 0; i < 8; i++)
        te[i] = SYNTH_FX[8 + i];
    VOICE_ON[0] = 1;
    VOICE_INC[0] = 10713070u;                   /* 220 Hz at 88.2 kHz */
    for (k = 0; k < 345; k++) {                 /* 0.5 s */
        tri(buf, 1);
        rs_te_mono(cwo, buf, te);
    }
    VOICE_ON[0] = 0;
    for (k = 0, pk = 0; k < 689; k++) {         /* 1 s later */
        tri(buf, 0);
        rs_te_mono(cwo, buf, te);
        if (k > 680)
            pk = peak(buf) > pk ? peak(buf) : pk;
    }
    CHECK(1, cwo_monos == 1 && pk > 1000);

    /* 2: drum FX (pos 1) reads the drum sound's params; pos -1 -> TE */
    cwo[0xe4 / 4] = 1;
    mark(DRUM_FX + 16, 32767);
    tri(buf, 1);
    rs_te_mono(cwo, buf, te);
    k = cwo_monos;
    cwo[0xe4 / 4] = (uint32_t)-1;
    rs_te_mono(cwo, buf, te);
    CHECK(2, k == 1 && cwo_monos == 2);
    cwo[0xe4 / 4] = 0;

    /* 3: master FX (stereo): both channels get the strings; MIX 0 leaves them dry */
    mark(MASTER_FX, 32767);
    for (i = 0; i < 8; i++)
        te[i] = MASTER_FX[i];
    VOICE_ON[0] = 1;
    for (k = 0; k < 100; k++) {
        tri(l, 1);
        tri(r, 1);
        rs_te_stereo(cwo, l, r, te);
    }
    CHECK(3, cwo_stereos == 0 && peak(l) > 100 && peak(r) > 100);
    te[3] = 0;
    tri(l, 1);
    k = l[5];
    rs_te_stereo(cwo, l, r, te);
    CHECK(4, l[5] == k && cwo_stereos == 0);
    MASTER_FX[4] = 0;                           /* no marker: TE's stereo CWO */
    rs_te_stereo(cwo, l, r, te);
    CHECK(5, cwo_stereos == 1);

    /* 6: FX list: "reso" -> "cwo", others untouched */
    {
        static te_string a, b;
        static const char na[] = "reso", nb[] = "resox";

        for (i = 0; i < 6; i++) {
            a.bx.sso[i] = na[i < 5 ? i : 4];
            b.bx.sso[i] = nb[i];
        }
        a.size = 4; b.size = 5;
        a.cap = b.cap = 15;
        rs_te_fx_fetched(&a, 0, 0);
        rs_te_fx_fetched(&b, 0, 0);
        CHECK(6, a.size == 3 && a.bx.sso[0] == 'c' && a.bx.sso[2] == 'o' && a.bx.sso[3] == 0
                 && b.size == 5 && b.bx.sso[0] == 'r');
    }

    /* 7: synth FX screen of a RESO sound: push -> page 2 (TE never sees it), knob -> slot 5 up,
     * marker kept, value in TE's range, saved; page 1 knobs stay TE's */
    *TE_EDIT_MODE = 0;
    *TE_SYNTH_SUBSCREEN = 2;
    FX_TYPE[1] = 7;
    FX_ON[1] = 1;
    push(59);
    {
        int16_t d = 1024;

        hook_knob(0, &d);
    }
    CHECK(7, rs_page == 1 && keys_seen == 0 && saves == 1 && knobs_seen == 0
             && (SYNTH_FX[8 + 4] & 0xe000) == 0x2000 && (SYNTH_FX[8 + 4] & 0x1fff) > 0);
    {
        int16_t d = 32767;

        for (i = 0; i < 6; i++)
            hook_knob(1, &d);                   /* clamp: stays 8192..16383 */
    }
    CHECK(8, SYNTH_FX[8 + 5] >= 8192 && SYNTH_FX[8 + 5] <= 16383);
    push(59);                                   /* page 1: TE's knobs */
    {
        int16_t d = 100;

        hook_knob(0, &d);
    }
    CHECK(9, rs_page == 0 && knobs_seen == 1);

    /* 10: engine screen: pushes are not RESO's */
    *TE_SYNTH_SUBSCREEN = 0;
    i = keys_seen;
    push(60);
    CHECK(10, rs_page == 0);
    *TE_SYNTH_SUBSCREEN = 2;

    /* 12: drum FX screen (edit mode 1, drum sub-screen 2) of a RESO drum sound: same page, saved into
     * the drum patch */
    {
        volatile int16_t *drum = (volatile int16_t *)0xff8015c4 + 2 * 8;
        int16_t d = 1024;

        for (i = 0; i < 8; i++)
            drum[i] = SYNTH_FX[8 + i];
        *TE_SYNTH_SUBSCREEN = 0;
        *TE_EDIT_MODE = 1;
        *(volatile int32_t *)0x01aab9f4 = 2;
        *TE_DRUM_SLOT = 2;
        ((volatile int32_t *)0xff801754)[2] = 7;
        ((volatile uint8_t *)0x01aaac30)[2] = 1;
        i = saves;
        push(59);
        hook_knob(0, &d);
        CHECK(12, rs_page == 1 && saves == i + 1 && last_patch == (void *)(0x01ddd9a8 + 2 * 0x38)
                  && (drum[4] & 0xe000) == 0x2000 && drum[4] != SYNTH_FX[8 + 4] && keys_seen == 0);
        push(59);
        *(volatile int32_t *)0x01aab9f4 = 0;
        *TE_EDIT_MODE = 0;
        *TE_SYNTH_SUBSCREEN = 2;
    }

    /* 11: cost of one RESO block (synth FX, everything on) */
    SYNTH_FX[8 + 7] = 0x2000 | 4000;            /* drive */
    for (i = 0; i < 8; i++)
        te[i] = SYNTH_FX[8 + i];
    tri(buf, 1);
    t0 = now();
    rs_te_mono(cwo, buf, te);                   /* retunes (OCTAVE changed above) */
    cycles_tune = now() - t0;
    tri(buf, 1);
    t0 = now();
    rs_te_mono(cwo, buf, te);                   /* steady block */
    cycles = now() - t0;
    CHECK(11, cycles < 30000);                  /* ~15 M cycles/s at 689 blocks/s (~3-4 % CPU) */
    return fails;
}
