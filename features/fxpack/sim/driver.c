/* FXPACK bench (build/apps/op1plus): phone process wrappers against stub TE functions, the shared
 * FX-name points, page-1 TYPE, page-1/2 value edits + TE's save, and the cost per 64-sample block of
 * every effect in CPU cycles. Returns failed-check bits (0 = pass). */
#include <op1/te246.h>

extern void fp_te_mono(void *self, int16_t *buf, int16_t *params);
extern void fp_te_stereo(void *self, int16_t *l, int16_t *r, int16_t *params);
extern void compose_fx_name(void *out, void *db, const char *sql);
extern void compose_master_fx_name(void *out, void *db, const char *sql);
extern void compose_fx_patch(void *patch);
extern void compose_fx_current_name(void *node, void *out);
extern void compose_master_fx_current_name(void *out);
extern void hook_key(int key, int down);
extern void hook_knob(int knob, int16_t *diff);
extern volatile uint8_t fp_page;

#define SYNTH_FX    ((volatile int16_t *)0xff801444)
#define DRUM_FX     ((volatile int16_t *)0xff8015c4)
#define MASTER_FX   ((volatile int16_t *)0xff8016c4)
#define FX_TYPE     ((volatile int32_t *)0xff801734)
#define FX_ON       ((volatile uint8_t *)0x01aaac20)

static int phone_monos, phone_stereos, saves, master_saves, keys_seen, knobs_seen;
static int16_t buf[64], l[64], r[64];
static uint32_t phone[64];                      /* fake phone object (never looked into) */
volatile uint32_t cycles, cycles_st, each[7];   /* read by run_fptest.py */
volatile int fails;

void phone_mono(void *self, int16_t *b, int16_t *p) { (void)self; (void)b; (void)p; phone_monos++; }
void phone_stereo(void *self, int16_t *a, int16_t *c, int16_t *p) { (void)self; (void)a; (void)c; (void)p; phone_stereos++; }
void log_save(void *patch, int16_t *p) { (void)patch; (void)p; saves++; }
void log_master(void *state) { (void)state; master_saves++; }
void log_key(int key, int down) { (void)key; (void)down; keys_seen++; }
void log_knob(int knob, int16_t *diff) { (void)knob; (void)diff; knobs_seen++; }

static uint32_t now(void)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

static uint32_t seed = 1;

static void noise(int16_t *b)
{
    int i;

    for (i = 0; i < 64; i++) {
        seed = seed * 1664525u + 1013904223u;
        b[i] = (int16_t)(((int32_t)(seed >> 16) - 32768) / 4);
    }
}

static int differs(const int16_t *a, const int16_t *b)
{
    int i, n = 0;

    for (i = 0; i < 64; i++)
        n += a[i] != b[i];
    return n;
}

/* an FXPACK sound of effect t: page 1 mid-range in phone's ranges, full MIX/LEVEL, page 2 mid */
static void mark(volatile int16_t *f, int t)
{
    f[0] = 14000; f[1] = 14000; f[2] = 12000; f[3] = 32767;
    f[4] = (int16_t)(0x6000 | ((t * 4682 + 2341) >> 2));
    f[5] = 0x6000 | 6000; f[6] = 0x6000 | 4000; f[7] = 0x6000 | 4000;
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
    int16_t te[8], ref[64];
    int i, k, t, ok;
    uint32_t t0, c;

    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");  /* CCEN */
    *TE_SYNTH_SLOT = 1;
    *TE_DRUM_SLOT = 2;
    *TE_PLAYMODE = 0;
    *TE_PLAYMODE_TARGET = 0;

    /* 0: plain phone (no marker) -> TE's phone, block untouched */
    noise(buf);
    k = buf[10];
    copy(te, SYNTH_FX + 8);
    fp_te_mono(phone, buf, te);
    CHECK(0, phone_monos == 1 && buf[10] == k);

    /* 1: every effect runs instead of phone and changes the block (TEXTURE adds its bed) */
    for (t = 0, ok = 1; t < 7; t++) {
        mark(SYNTH_FX + 8, t);
        copy(te, SYNTH_FX + 8);
        for (k = 0; k < 40; k++) {
            noise(buf);
            for (i = 0; i < 64; i++)
                ref[i] = buf[i];
            fp_te_mono(phone, buf, te);
        }
        ok &= differs(buf, ref) > 8;
    }
    CHECK(1, ok && phone_monos == 1);

    /* 2: drum play mode reads the drum sound's params; while TE switches -> TE's phone */
    *TE_PLAYMODE = 1;
    *TE_PLAYMODE_TARGET = 1;
    SYNTH_FX[8 + 4] = 0;
    mark(DRUM_FX + 16, 3);
    copy(te, DRUM_FX + 16);
    noise(buf);
    fp_te_mono(phone, buf, te);
    k = phone_monos;
    *TE_PLAYMODE_TARGET = 0;
    fp_te_mono(phone, buf, te);
    CHECK(2, k == 1 && phone_monos == 2);
    *TE_PLAYMODE = 0;
    mark(SYNTH_FX + 8, 0);

    /* 3: master FX: both channels processed (PHASER with SPREAD: they differ); no marker -> TE */
    mark(MASTER_FX, 0);
    MASTER_FX[7] = 0x6000 | 8191;               /* SPREAD full */
    copy(te, MASTER_FX);
    for (k = 0; k < 40; k++) {
        noise(l);
        for (i = 0; i < 64; i++)
            r[i] = ref[i] = l[i];
        fp_te_stereo(phone, l, r, te);
    }
    CHECK(3, phone_stereos == 0 && differs(l, ref) > 8 && differs(r, ref) > 8 && differs(l, r) > 8);
    MASTER_FX[4] = 0;
    fp_te_stereo(phone, l, r, te);
    CHECK(4, phone_stereos == 1);

    /* 5: shared FX-name points: "fx mix" -> "phone"; shimmer / reso still renamed; others not */
    {
        static te_string a, b;

        ok = 1;
        set_name(&a, "fx mix");
        compose_fx_name(&a, 0, 0);
        set_name(&b, "fx mix");
        compose_master_fx_name(&b, 0, 0);
        ok &= is(&a, "phone") && is(&b, "phone");
        set_name(&b, "phaser");                 /* v0.1 row names are gone */
        compose_fx_name(&b, 0, 0);
        ok &= is(&b, "phaser");
        set_name(&a, "shimmer");
        compose_fx_name(&a, 0, 0);
        set_name(&b, "fuzzy");
        compose_fx_name(&b, 0, 0);
        ok &= is(&a, "spring") && is(&b, "fuzzy");
        set_name(&a, "phone");
        compose_fx_name(&a, 0, 0);
        ok &= is(&a, "phone");
        CHECK(5, ok);
    }

    /* 6: synth FX screen of an FXPACK sound, page 1: blue = TYPE one effect per click, loading
     * that effect's defaults (marker kept), saved once per change; clamps at PHASER */
    *TE_EDIT_MODE = 0;
    *TE_SYNTH_SUBSCREEN = 2;
    FX_TYPE[1] = 3;
    FX_ON[1] = 1;
    {
        int16_t d = 1;

        hook_knob(0, &d);                       /* PHASER -> FLANGER: its defaults */
        k = (SYNTH_FX[8 + 4] & 0x1fff) << 2;
        ok = SYNTH_FX[8 + 0] == 6286 && SYNTH_FX[8 + 2] == 14131 && SYNTH_FX[8 + 7] == 28671;
        hook_knob(0, &d);                       /* -> FOLD */
        d = -1;
        for (i = 0; i < 10; i++)
            hook_knob(0, &d);                   /* back to PHASER, then clamps (no saves) */
    }
    CHECK(6, ok && fp_page == 0 && keys_seen == 0 && saves == 4 && knobs_seen == 0 && k * 7 >> 15 == 1
             && (SYNTH_FX[8 + 4] & 0xe000) == 0x6000 && (((SYNTH_FX[8 + 4] & 0x1fff) << 2) * 7 >> 15) == 0
             && SYNTH_FX[8 + 0] == 7300);
    /* 7: push -> page 2: green = page-2 value 1; blue = MIX (phone's knob 4) in phone's range */
    push(59);
    {
        int16_t d = 32767;

        for (i = 0; i < 6; i++)
            hook_knob(1, &d);
        d = -8192;
        hook_knob(0, &d);
    }
    CHECK(7, fp_page == 1 && SYNTH_FX[8 + 5] >= 24576 && SYNTH_FX[8 + 5] <= 32767
             && (SYNTH_FX[8 + 5] & 0x1fff) > 6000 && SYNTH_FX[8 + 3] < 32767 && SYNTH_FX[8 + 3] > 28000
             && (SYNTH_FX[8 + 4] & 0xe000) == 0x6000 && knobs_seen == 0);
    /* 8: push -> page 1: green = phone's knob 1 (RATE) in its range 204..20480, clamped; shift
     * turns consumed; TE sees no knob */
    push(59);
    {
        int16_t d = 4096;

        k = SYNTH_FX[8 + 0];
        hook_knob(1, &d);
        ok = SYNTH_FX[8 + 0] > k;
        d = 32767;
        for (i = 0; i < 10; i++)
            hook_knob(1, &d);
        ok &= SYNTH_FX[8 + 0] == 20480 && SYNTH_FX[8 + 4] == 25161;   /* TYPE untouched */
        d = -32767;
        for (i = 0; i < 10; i++)
            hook_knob(3, &d);                   /* orange = knob 3 (FEEDBACK), floor 1536 */
        ok &= SYNTH_FX[8 + 2] == 1536;
    }
    CHECK(8, ok && fp_page == 0 && knobs_seen == 0);

    /* 9: engine screen and a spring sound's FX screen are not FXPACK's */
    *TE_SYNTH_SUBSCREEN = 0;
    push(60);
    CHECK(9, fp_page == 0);
    *TE_SYNTH_SUBSCREEN = 2;
    FX_TYPE[1] = 4;
    push(60);
    CHECK(10, fp_page == 0);
    FX_TYPE[1] = 3;

    /* 13: mixer master FX page (edit mode 3, page FX, phone on, marked): push -> page 2, knobs edit
     * the master params and mark the master dirty; other mixer pages / trick edits not claimed */
    {
        int16_t d = 4096;
        int16_t before;

        *TE_SYNTH_SUBSCREEN = 0;
        mark(MASTER_FX, 1);
        *TE_EDIT_MODE = TE_EDIT_MODE_MIXER;
        *TE_MIXER_PAGE = 0;
        *TE_MASTER_FX_TYPE = 3;
        *TE_MASTER_FX_ON = 1;
        TE_MASTER_TRICK[0] = TE_MASTER_TRICK[1] = 0;
        k = keys_seen;
        push(60);                               /* levels page: TE's */
        ok = keys_seen == k + 2 && fp_page == 0;
        *TE_MIXER_PAGE = TE_MIXER_PAGE_FX;
        push(60);
        ok &= fp_page == 1 && keys_seen == k + 2;
        before = MASTER_FX[6];
        hook_knob(2, &d);                       /* white: page-2 value 2 (FLANGER DAMP) */
        ok &= MASTER_FX[6] > before && (MASTER_FX[6] & 0xe000) == 0x6000 && master_saves == 1;
        TE_MASTER_TRICK[0] = 1;                 /* trick edit: TE's */
        push(61);
        ok &= fp_page == 1 && keys_seen == k + 4;
        TE_MASTER_TRICK[0] = 0;
        push(61);                               /* back to page 1 */
        ok &= fp_page == 0;
        d = 1;
        hook_knob(0, &d);                       /* page 1 blue: TYPE -> FOLD with its defaults */
        ok &= ((((MASTER_FX[4] & 0x1fff) << 2) * 7) >> 15) == 2 && MASTER_FX[0] == 8314 && master_saves == 2;
        CHECK(13, ok);
        *TE_EDIT_MODE = 0;
        *TE_SYNTH_SUBSCREEN = 2;
    }

    /* 12: FX browsers' current row: a marked sound answers with its own row name (synth, drum,
     * master; FXPACK by TYPE, SHIMMER, RESO); unmarked sounds and other names stay */
    {
        static te_string a;
        volatile int16_t *sf = SYNTH_FX + 8 * 5, *df = DRUM_FX + 8 * 6;

        ok = 1;
        mark(sf, 3);                            /* synth sound 5: FXPACK FUZZ on phone */
        compose_fx_patch((void *)(0x01ddd7e8 + 5 * 0x38));
        set_name(&a, "phone");
        compose_fx_current_name(0, &a);
        ok &= is(&a, "fx mix");
        sf[4] = 0;                              /* unmarked: stays */
        compose_fx_patch((void *)(0x01ddd7e8 + 5 * 0x38));
        set_name(&a, "phone");
        compose_fx_current_name(0, &a);
        ok &= is(&a, "phone");
        df[0] = 9000; df[1] = 9000; df[2] = 9000; df[3] = 9000;     /* drum sound 6: SHIMMER */
        df[4] = 0x4000; df[5] = 0x4000; df[6] = 0x4000; df[7] = 0x4000;
        compose_fx_patch((void *)(0x01ddd7e8 + 14 * 0x38));
        set_name(&a, "spring");
        compose_fx_current_name(0, &a);
        ok &= is(&a, "shimmer");
        df[4] = 0x2000; df[5] = 0x2000; df[6] = 0x2000; df[7] = 0x2000;   /* now RESO on cwo */
        compose_fx_patch((void *)(0x01ddd7e8 + 14 * 0x38));
        set_name(&a, "cwo");
        compose_fx_current_name(0, &a);
        ok &= is(&a, "reso");
        set_name(&a, "spring");                 /* RESO marker but spring: not ours */
        compose_fx_patch((void *)(0x01ddd7e8 + 14 * 0x38));
        compose_fx_current_name(0, &a);
        ok &= is(&a, "spring");
        set_name(&a, "cwo");                    /* no patch seen: nothing */
        compose_fx_current_name(0, &a);
        ok &= is(&a, "cwo");
        mark(MASTER_FX, 6);                     /* master: FXPACK TEXTURE */
        set_name(&a, "phone");
        compose_master_fx_current_name(&a);
        ok &= is(&a, "fx mix");
        CHECK(12, ok);
    }

    /* 11: cost: worst of the seven effects (page 2 full: 8 phaser stages, 24 dB, ...), mono and
     * one stereo block */
    for (t = 0; t < 7; t++) {
        mark(SYNTH_FX + 8, t);
        for (i = 5; i < 8; i++)
            SYNTH_FX[8 + i] = 0x6000 | 8191;
        SYNTH_FX[8 + 2] = 16000;
        copy(te, SYNTH_FX + 8);
        noise(buf);
        fp_te_mono(phone, buf, te);             /* starts */
        noise(buf);
        t0 = now();
        fp_te_mono(phone, buf, te);
        c = now() - t0;
        each[t] = c;
        cycles = c > cycles ? c : cycles;
        mark(MASTER_FX, t);
        for (i = 5; i < 8; i++)
            MASTER_FX[i] = 0x6000 | 8191;
        copy(te, MASTER_FX);
        noise(l);
        noise(r);
        fp_te_stereo(phone, l, r, te);
        noise(l);
        noise(r);
        t0 = now();
        fp_te_stereo(phone, l, r, te);
        c = now() - t0;
        cycles_st = c > cycles_st ? c : cycles_st;
    }
    CHECK(11, cycles < 25000 && cycles_st < 50000);     /* < ~17 M cycles/s mono (RESO: 22k) */
    return fails;
}
