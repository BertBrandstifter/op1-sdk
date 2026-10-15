/* SHIMMER bench (build/apps/op1plus): spring process wrappers against stub TE functions, the
 * shared FX-name point (SHIMMER + RESO renames), page-2 pushes and knob edits + TE's save, and
 * the cost per 64-sample FX block in CPU cycles. Returns failed-check bits (0 = pass). */
#include <op1/te246.h>

extern void sh_te_mono(void *self, int16_t *buf, int16_t *params);
extern void sh_te_stereo(void *self, int16_t *l, int16_t *r, int16_t *params);
extern void compose_fx_name(void *out, void *db, const char *sql);
extern void compose_master_fx_name(void *out, void *db, const char *sql);
extern void hook_key(int key, int down);
extern void hook_knob(int knob, int16_t *diff);
extern volatile uint8_t sh_page;

#define SYNTH_FX    ((volatile int16_t *)0xff801444)
#define DRUM_FX     ((volatile int16_t *)0xff8015c4)
#define MASTER_FX   ((volatile int16_t *)0xff8016c4)
#define FX_TYPE     ((volatile int32_t *)0xff801734)
#define FX_ON       ((volatile uint8_t *)0x01aaac20)

static int spring_monos, spring_stereos, saves, keys_seen, knobs_seen;
static int16_t buf[64], l[64], r[64];
static uint32_t spring[64];                     /* fake spring object (never looked into) */
volatile uint32_t cycles, cycles_st;            /* read by run_shtest.py */
volatile int fails;

void spring_mono(void *self, int16_t *b, int16_t *p) { (void)self; (void)b; (void)p; spring_monos++; }
void spring_stereo(void *self, int16_t *a, int16_t *c, int16_t *p) { (void)self; (void)a; (void)c; (void)p; spring_stereos++; }
void log_save(void *patch, int16_t *p) { (void)patch; (void)p; saves++; }
void log_key(int key, int down) { (void)key; (void)down; keys_seen++; }
void log_knob(int knob, int16_t *diff) { (void)knob; (void)diff; knobs_seen++; }

static uint32_t now(void)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

static uint32_t seed = 1;

/* one block of noise (on) or silence */
static void noise(int16_t *b, int on)
{
    int i;

    for (i = 0; i < 64; i++) {
        seed = seed * 1664525u + 1013904223u;
        b[i] = on ? (int16_t)((int32_t)(seed >> 16) - 32768) / 4 : 0;
    }
}

static int peak(const int16_t *b)
{
    int i, m = 0;

    for (i = 0; i < 64; i++)
        m = b[i] > m ? b[i] : -b[i] > m ? -b[i] : m;
    return m;
}

/* spring ranges: DECAY 1344..16448, TONE 7744..16448, SHIMMER 0..16384, MIX 0..32767 */
static void mark(volatile int16_t *f, int mix)
{
    f[0] = 14000; f[1] = 14000; f[2] = 10000; f[3] = (int16_t)mix;
    f[4] = 0x4000; f[5] = 0x4000 | 5000; f[6] = 0x4000 | 2000; f[7] = 0x4000 | 2000;
}

static void copy(int16_t *te, const volatile int16_t *f)
{
    int i;

    for (i = 0; i < 8; i++)
        te[i] = f[i];
}

static void push(int key)
{
    hook_key(key, 1);
    hook_key(key, 0);
}

static void set_name(te_string *s, const char *n)
{
    int i;

    for (i = 0; i < 15; i++)
        s->bx.sso[i] = 0;
    for (i = 0; n[i]; i++)
        s->bx.sso[i] = n[i];
    s->size = (uint32_t)i;
    s->cap = 15;
}

static int is(const te_string *s, const char *n)
{
    int i;

    for (i = 0; n[i]; i++)
        if (s->bx.sso[i] != n[i])
            return 0;
    return s->size == (uint32_t)i && s->bx.sso[i] == 0;
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
    *TE_PLAYMODE = 0;                           /* synth */
    *TE_PLAYMODE_TARGET = 0;

    /* 0: plain spring (no marker) -> TE's spring process, block untouched */
    noise(buf, 1);
    k = buf[10];
    copy(te, SYNTH_FX + 8);
    sh_te_mono(spring, buf, te);
    CHECK(0, spring_monos == 1 && buf[10] == k);

    /* 1: SHIMMER as synth FX: 0.2 s of noise, then silence: a tail 0.5 s later, TE's spring
     * never runs */
    mark(SYNTH_FX + 8, 32767);
    copy(te, SYNTH_FX + 8);
    for (k = 0; k < 138; k++) {
        noise(buf, 1);
        sh_te_mono(spring, buf, te);
    }
    for (k = 0, pk = 0; k < 345; k++) {
        noise(buf, 0);
        sh_te_mono(spring, buf, te);
        if (k > 340)
            pk = peak(buf) > pk ? peak(buf) : pk;
    }
    CHECK(1, spring_monos == 1 && pk > 300);

    /* 2: drum play mode reads the drum sound's params; while TE switches -> TE's spring */
    *TE_PLAYMODE = 1;
    *TE_PLAYMODE_TARGET = 1;
    SYNTH_FX[8 + 4] = 0;                        /* synth sound: no marker */
    mark(DRUM_FX + 16, 32767);
    copy(te, DRUM_FX + 16);
    noise(buf, 1);
    sh_te_mono(spring, buf, te);
    k = spring_monos;
    *TE_PLAYMODE_TARGET = 0;
    sh_te_mono(spring, buf, te);
    CHECK(2, k == 1 && spring_monos == 2);
    *TE_PLAYMODE = 0;
    mark(SYNTH_FX + 8, 32767);

    /* 3: master FX (stereo): both channels get a tail; MIX 0 leaves them dry; no marker -> TE */
    mark(MASTER_FX, 32767);
    copy(te, MASTER_FX);
    for (k = 0; k < 200; k++) {
        noise(l, k < 50);
        noise(r, k < 50);
        sh_te_stereo(spring, l, r, te);
    }
    CHECK(3, spring_stereos == 0 && peak(l) > 100 && peak(r) > 100);
    te[3] = 0;
    noise(l, 1);
    k = l[5];
    sh_te_stereo(spring, l, r, te);
    CHECK(4, l[5] == k && spring_stereos == 0);
    MASTER_FX[4] = 0;
    sh_te_stereo(spring, l, r, te);
    CHECK(5, spring_stereos == 1);

    /* 6: shared FX-name points: "shimmer" -> "spring", "reso" -> "cwo", others untouched */
    {
        static te_string a, b, c, e;

        set_name(&a, "shimmer");
        set_name(&b, "reso");
        set_name(&c, "shimmers");
        set_name(&e, "shimmer");
        compose_fx_name(&a, 0, 0);
        compose_fx_name(&b, 0, 0);
        compose_fx_name(&c, 0, 0);
        compose_master_fx_name(&e, 0, 0);
        CHECK(6, is(&a, "spring") && is(&b, "cwo") && is(&c, "shimmers") && is(&e, "spring"));
    }

    /* 7: synth FX screen of a SHIMMER sound: push -> page 2 (TE never sees it), knob -> slot 5 up,
     * marker kept, value in TE's range, saved; page 1 knobs stay TE's */
    *TE_EDIT_MODE = 0;
    *TE_SYNTH_SUBSCREEN = 2;
    FX_TYPE[1] = 4;
    FX_ON[1] = 1;
    push(59);
    {
        int16_t d = 1024;

        hook_knob(0, &d);
    }
    CHECK(7, sh_page == 1 && keys_seen == 0 && saves == 1 && knobs_seen == 0
             && (SYNTH_FX[8 + 4] & 0xe000) == 0x4000 && (SYNTH_FX[8 + 4] & 0x1fff) > 0);
    {
        int16_t d = 32767;

        for (i = 0; i < 6; i++)
            hook_knob(1, &d);                   /* clamp: stays 16384..24575 */
    }
    CHECK(8, SYNTH_FX[8 + 5] >= 16384 && SYNTH_FX[8 + 5] <= 24575);
    push(59);                                   /* page 1: TE's knobs */
    {
        int16_t d = 100;

        hook_knob(0, &d);
    }
    CHECK(9, sh_page == 0 && knobs_seen == 1);

    /* 10: engine screen: pushes are not SHIMMER's; a CWO sound's FX screen neither */
    *TE_SYNTH_SUBSCREEN = 0;
    push(60);
    CHECK(10, sh_page == 0);
    *TE_SYNTH_SUBSCREEN = 2;
    FX_TYPE[1] = 7;
    push(60);
    CHECK(11, sh_page == 0);
    FX_TYPE[1] = 4;

    /* 12: cost of one SHIMMER block, everything on (synth FX mono, master stereo) */
    SYNTH_FX[8 + 6] = 0x4000 | 8191;            /* MOD full */
    copy(te, SYNTH_FX + 8);
    noise(buf, 1);
    sh_te_mono(spring, buf, te);
    noise(buf, 1);
    t0 = now();
    sh_te_mono(spring, buf, te);
    cycles = now() - t0;
    mark(MASTER_FX, 32767);
    copy(te, MASTER_FX);
    noise(l, 1);
    noise(r, 1);
    sh_te_stereo(spring, l, r, te);             /* starts (reset) */
    noise(l, 1);
    noise(r, 1);
    t0 = now();
    sh_te_stereo(spring, l, r, te);
    cycles_st = now() - t0;
    CHECK(12, cycles < 45000 && cycles_st < 50000);    /* ~25-30 M cycles/s at 689 blocks/s */
    return fails;
}
