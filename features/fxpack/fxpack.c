/* FXPACK (spec: README.md): seven small effects (PHASER FLANGER FOLD FUZZ DJ FILTER TRANSIENT
 * TEXTURE) hosted on TE's phone effect, one row "fx mix" in TE's FX list; page 1's blue knob
 * picks the effect.
 *
 * An FX is FXPACK when its stored fx_params 5-8 all have the form 0x6000 | v (24576..32767): param
 * 5's v selects the effect (TYPE), params 6-8 are its page-2 values. Phone allows 0..32767 there
 * and ignores them, so FXPACK sounds pass TE's patch validator (boot included) and load on stock
 * firmware as plain phone. Page 1 = phone's four knobs in phone's ranges, rescaled to 0..32767.
 *
 * fp_te_mono / fp_te_stereo  phone process slots: FXPACK when the stored params carry the marker
 *                (synth or drum FX: mono, master FX: one state per channel), otherwise TE's phone
 * fp_te_fx_fetched / fp_te_master_fetched  FX list: the picked row's type "fx mix" becomes
 *                "phone" (its default_params, added by content_sql, carry PHASER's defaults)
 * fp_key / fp_knob  synth FX screen of an FXPACK sound: knob push flips page 1 (blue = TYPE, then
 *                phone's knobs 1-3) / page 2 (phone's knob 4 = MIX, then the three page-2 values);
 *                every knob edited here, saved through TE
 * fp_frame       FXPACK page over phone's on the synth FX screen */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "fxpack_core.h"
#include "fp_te246.h"

#define KEY_KNOB_PUSH   59                      /* blue 59 .. orange 62 */
#define STEP_SCALE      10485                   /* ~100 clicks full range */
#define MARK            0x6000
#define MARK_MASK       0xe000

enum { INST_MONO = 0, INST_L, INST_R, INSTS };

static fp_state inst[INSTS];
static uint8_t on[INSTS];
static int8_t mono_mode;

volatile uint32_t fp_n_mono, fp_n_stereo, fp_n_stereo_marked;   /* call counters (fxdiag) */
volatile uint8_t fp_page;                       /* synth FX screen: 0 page 1, 1 page 2 (bench) */
static volatile uint8_t consumed;

/* phone's knob ranges -> 0..32767: (v - lo) * k >> 15, k = 32767 * 32768 / (hi - lo) */
static const int16_t lo[4] = { 204, 3072, 1536, 0 }, hi[4] = { 20480, 17408, 16896, 32767 };
static const int32_t k_norm[4] = { 52954, 74896, 69903, 32768 };

/* knob (blue .. orange) -> param per page: page 1 TYPE + phone's knobs 1-3, page 2 phone's
 * knob 4 (MIX) + the page-2 values */
static const uint8_t slot_of[2][4] = { { 4, 0, 1, 2 }, { 3, 5, 6, 7 } };

#define LIST_NAME       "fx mix"                /* the one FX-list row (content_sql) */

/* stored defaults per effect (page 1 in phone's ranges, then TYPE + page 2 with the marker):
 * loaded when TYPE changes, so every effect starts from settings that suit it */
static const int16_t defaults[FP_TYPES][8] = {
    { 7300, 13107, 7680, 32767, 25161, 28671, 27852, 28671 },      /* PHASER */
    { 6286, 11673, 14131, 32767, 26331, 25804, 27033, 28671 },     /* FLANGER */
    { 8314, 10240, 12288, 32767, 27502, 27033, 28671, 24576 },     /* FOLD */
    { 12369, 11673, 12288, 32767, 28672, 27852, 28671, 24576 },    /* FUZZ */
    { 10342, 8806, 1536, 32767, 29843, 24576, 27033, 24576 },      /* DJ FILTER */
    { 15411, 10240, 9216, 32767, 31013, 28671, 27033, 24576 },     /* TRANSIENT */
    { 204, 10240, 10752, 9830, 32184, 27033, 27852, 27033 },       /* TEXTURE */
};

static int marked(const volatile int16_t *f)
{
    return (f[4] & MARK_MASK) == MARK && (f[5] & MARK_MASK) == MARK
           && (f[6] & MARK_MASK) == MARK && (f[7] & MARK_MASK) == MARK;
}

static int16_t norm(int i, int32_t v)
{
    v = ((v - lo[i]) * k_norm[i]) >> 15;
    return (int16_t)(v < 0 ? 0 : v > 32767 ? 32767 : v);
}

/* 0..32767 -> phone's range (rounded) */
static int16_t denorm(int i, int32_t v)
{
    return (int16_t)(lo[i] + (v * (hi[i] - lo[i]) + 16383) / 32767);
}

/* page 1 from TE's per-block params (stored + LFO, phone's ranges), TYPE + page 2 from the marker */
static int params(int16_t *p, const int16_t *te, const volatile int16_t *stored)
{
    int i;

    for (i = 0; i < 4; i++) {
        p[i] = norm(i, te[i]);
        p[4 + i] = (int16_t)((stored[4 + i] & 0x1fff) << 2);
    }
    return fp_type_of(p[4]);
}

static void run(int which, int ch, int16_t *buf, const int16_t *p, int type)
{
    if (!on[which] || inst[which].type != type) {
        fp_reset(&inst[which], type, ch);
        on[which] = 1;
    }
    fp_process(&inst[which], buf, 64, p);
}

void fp_te_mono(void *self, int16_t *buf, int16_t *te)
{
    int32_t mode = *TE_PLAYMODE;
    const volatile int16_t *stored = 0;
    int16_t p[8];
    int type;

    fp_n_mono++;
    if (mode == *TE_PLAYMODE_TARGET) {          /* not while TE switches synth <-> drum */
        if (mode == 0)
            stored = FP_TE_SYNTH_FX + (*TE_SYNTH_SLOT & 7) * 8;
        else if (mode == 1)
            stored = FP_TE_DRUM_FX + (*TE_DRUM_SLOT & 7) * 8;
    }
    if (!stored || !marked(stored)) {
        on[INST_MONO] = 0;
        FP_TE_PHONE_MONO(self, buf, te);        /* stock phone */
        return;
    }
    if (mono_mode != mode)
        on[INST_MONO] = 0;
    mono_mode = (int8_t)mode;
    type = params(p, te, stored);
    run(INST_MONO, 0, buf, p, type);
}

void fp_te_stereo(void *self, int16_t *l, int16_t *r, int16_t *te)
{
    int16_t p[8];
    int type;

    fp_n_stereo++;
    if (!marked(FP_TE_MASTER_FX)) {
        on[INST_L] = on[INST_R] = 0;
        FP_TE_PHONE_STEREO(self, l, r, te);     /* stock phone */
        return;
    }
    fp_n_stereo_marked++;
    type = params(p, te, FP_TE_MASTER_FX);
    run(INST_L, 0, l, p, type);
    run(INST_R, 1, r, p, type);
}

/* FX list row "fx mix" -> "phone" right after TE reads the type name (TE knows no such FX: fatal) */
static void rename(te_string *out)
{
    static const char n[] = LIST_NAME;
    char *d = (char *)te_str_data(out);
    unsigned int i;

    if (out->size != sizeof(n) - 1)
        return;
    for (i = 0; i < sizeof(n) - 1; i++)
        if (d[i] != n[i])
            return;
    d[0] = 'p'; d[1] = 'h'; d[2] = 'o'; d[3] = 'n'; d[4] = 'e'; d[5] = 0;
    out->size = 5;
    fp_page = 0;
}

void fp_te_fx_fetched(void *out, void *db, const char *sql)     { (void)db; (void)sql; rename(out); }
void fp_te_master_fetched(void *out, void *db, const char *sql) { (void)db; (void)sql; rename(out); }

/* ------------------------------------------------------------------ FX browser: current row */

/* TE's FX browsers find the current row from the stored type name ("phone"); for a FXPACK sound
 * that row would be phone's, so the cursor jumps there and a confirm loads phone's defaults (our
 * marker gone). Answer with our own row name instead. */
static void *fp_cur_patch;

static const volatile int16_t *fp_fx_of_patch(void *patch)
{
    uint32_t a = (uint32_t)patch, s;

    if (a >= 0x01ddd7e8u && a < 0x01ddd7e8u + 16 * 0x38 && (a - 0x01ddd7e8u) % 0x38 == 0) {
        s = (a - 0x01ddd7e8u) / 0x38;           /* synth patches 0..7, then drum patches 0..7 */
        return s < 8 ? FP_TE_SYNTH_FX + s * 8 : FP_TE_DRUM_FX + (s - 8) * 8;
    }
    return 0;
}

static void fp_name_current(te_string *out, const char *name)
{
    static const char host[] = "phone";
    char *d = (char *)te_str_data(out);
    unsigned int i, n;

    if (out->size != sizeof(host) - 1)
        return;
    for (i = 0; i < sizeof(host) - 1; i++)
        if (d[i] != host[i])
            return;
    for (n = 0; name[n]; n++)
        ;
    if (n > out->cap)
        return;
    for (i = 0; i <= n; i++)
        d[i] = name[i];
    out->size = n;
}

void fp_te_fx_patch(void *patch) { fp_cur_patch = patch; }

void fp_te_fx_current(void *node, void *out)
{
    const volatile int16_t *f = fp_fx_of_patch(fp_cur_patch);

    (void)node;
    if (f && marked(f))
        fp_name_current(out, LIST_NAME);
    fp_cur_patch = 0;
}

void fp_te_master_current(void *out)
{
    if (marked(FP_TE_MASTER_FX))
        fp_name_current(out, LIST_NAME);
}

/* ------------------------------------------------------------------ KeyHandler thread */

/* where the effect's page is drawn and edited: the synth FX screen of the current sound
 * (*slot = sound), the drum FX screen (*slot = 8 + drum sound) or the mixer's master FX page (*slot = -1); 0 when neither shows a FXPACK */
static volatile int16_t *synth_fx_pack(int *slot)
{
    volatile int16_t *f;

    *slot = *TE_SYNTH_SLOT & 7;
    if (*TE_EDIT_MODE == 0 && *TE_SYNTH_SUBSCREEN == 2 && FP_TE_SYNTH_FX_TYPE[*slot] == FP_TE_PHONE_ID && FP_TE_SYNTH_FX_ON[*slot]) {
        f = FP_TE_SYNTH_FX + *slot * 8;
        return marked(f) ? f : 0;
    }
    *slot = *TE_DRUM_SLOT & 7;                  /* drum FX screen: slot 8..15 */
    if (*TE_EDIT_MODE == TE_EDIT_MODE_DRUM && *TE_DRUM_SUBSCREEN == 2 && TE_DRUM_FX_TYPE[*slot] == FP_TE_PHONE_ID
        && TE_DRUM_FX_ON[*slot]) {
        f = TE_DRUM_FX_PARAMS + *slot * 8;
        *slot += 8;
        return marked(f) ? f : 0;
    }
    if (*TE_EDIT_MODE == TE_EDIT_MODE_MIXER && *TE_MIXER_PAGE == TE_MIXER_PAGE_FX
        && *TE_MASTER_FX_TYPE == FP_TE_PHONE_ID && *TE_MASTER_FX_ON && !TE_MASTER_TRICK[0] && !TE_MASTER_TRICK[1]
        && marked(TE_MASTER_FX_PARAMS)) {
        *slot = -1;
        return TE_MASTER_FX_PARAMS;
    }
    return 0;
}

static void fp_save(volatile int16_t *f, int slot)
{
    if (slot >= 8)
        FP_TE_PATCH_SET_FX(TE_DRUM_PATCH(slot - 8), (int16_t *)f);
    else if (slot >= 0)
        FP_TE_PATCH_SET_FX(TE_SYNTH_PATCH(slot), (int16_t *)f);
    else
        TE_MASTER_CHANGED(TE_MASTER_STATE);
}

int fp_key(int key, int down)
{
    int b = key - KEY_KNOB_PUSH, slot;

    if (b < 0 || b > 3)
        return 0;
    if (!down) {
        if (consumed & (1 << b)) {
            consumed &= (uint8_t)~(1 << b);
            return 1;
        }
        return 0;
    }
    if (*TE_SHIFT_HELD || !synth_fx_pack(&slot))
        return 0;
    fp_page ^= 1;
    consumed |= (uint8_t)(1 << b);
    return 1;
}

int fp_knob(int knob, int16_t *diff)
{
    int slot, v, t, p;
    volatile int16_t *f;

    if ((unsigned int)knob > 3 || !(f = synth_fx_pack(&slot)))
        return 0;
    if (*TE_SHIFT_HELD)
        return 1;
    p = slot_of[fp_page & 1][knob];
    if (p < 4) {                                /* phone's knob: edited in its own range */
        t = (*diff * STEP_SCALE) >> 15;
        if (t) {
            v = norm(p, f[p]) + t;
            f[p] = denorm(p, v < 0 ? 0 : v > 32767 ? 32767 : v);
            fp_save(f, slot);
        }
        return 1;
    }
    v = (f[p] & 0x1fff) << 2;
    if (p == 4) {                               /* TYPE: one effect per click, with its defaults */
        t = fp_type_of((int16_t)v) + (*diff > 0 ? 1 : *diff < 0 ? -1 : 0);
        t = t < 0 ? 0 : t >= FP_TYPES ? FP_TYPES - 1 : t;
        if (t != fp_type_of((int16_t)v)) {
            for (v = 0; v < 8; v++)
                f[v] = defaults[t][v];
            fp_save(f, slot);
        }
        return 1;
    } else {
        v += (*diff * STEP_SCALE) >> 15;
    }
    v = v < 0 ? 0 : v > 32767 ? 32767 : v;
    f[p] = (int16_t)(MARK | (v >> 2));          /* always 24576..32767: in TE's range */
    fp_save(f, slot);
    return 1;
}

/* ------------------------------------------------------------------ GUI thread */

/* scope colour from the block's peak (0..32767): blue (quiet) -> green -> orange (full scale) */
static uint16_t peak_colour(int32_t peak)
{
    static const uint8_t stop[3][3] = { { 0x69, 0x8e, 0xff }, { 0x00, 0xed, 0x95 }, { 0xff, 0x6a, 0x3d } };
    int32_t t = peak > 32767 ? 32767 : peak, i = t < 16384 ? 0 : 1;
    int32_t f = (t - i * 16384) >> 6;          /* 0..255 within the half */
    int32_t c[3], j;

    for (j = 0; j < 3; j++)
        c[j] = stop[i][j] + (((stop[i + 1][j] - stop[i][j]) * f) >> 8);
    return GFX_RGB(c[0], c[1], c[2]);
}

void fp_frame(uint16_t *buf)
{
    static const uint16_t col[4] = {
        GFX_RGB(0x69, 0x8e, 0xff), GFX_RGB(0x00, 0xed, 0x95),
        GFX_RGB(0xe6, 0xe6, 0xe6), GFX_RGB(0xff, 0x6a, 0x3d),
    };
    static const uint16_t dim = GFX_RGB(0x5a, 0x58, 0x66), track = GFX_RGB(0x35, 0x32, 0x38);
    int16_t v[8];
    volatile int16_t *f;
    int slot, i, x, pg = fp_page, type, who;

    if (!(f = synth_fx_pack(&slot)))
        return;
    for (i = 0; i < 4; i++) {
        v[i] = norm(i, f[i]);
        v[4 + i] = (int16_t)((f[4 + i] & 0x1fff) << 2);
    }
    type = fp_type_of(v[4]);
    who = slot < 0 ? INST_L : INST_MONO;
    gfx_begin(buf);
    gfx_rect(0, 0, gfx_w, gfx_h, GFX_BLACK);
    gfx_text(4, 4, fp_name(type), col[1], 2);
    gfx_text(272, 4, pg ? "P2" : "P1", GFX_WHITE, 2);

    /* scope: the last block the effect put out, one colour from its peak */
    gfx_rect(4, 66, 312, 1, track);
    {
        int live = on[who] && inst[who].type == type;
        int32_t peak = 0, a;
        uint16_t c;

        for (i = 0; live && i < 64; i++) {
            a = inst[who].scope[i];
            a = a < 0 ? -a : a;
            peak = a > peak ? a : peak;
        }
        c = peak_colour(peak);
        for (i = 0; i < 64; i++) {
            int y = live ? inst[who].scope[i] / 1024 : 0, h;

            h = y < 0 ? -y : y;
            gfx_rect(4 + i * 5, y < 0 ? 66 : 66 - h, 4, h ? h : 1, c);
        }
    }

    for (i = 0; i < 4; i++) {
        int p = slot_of[pg & 1][i], o = slot_of[(pg & 1) ^ 1][i];
        const char *c = fp_choice(type, p, v[p]), *co = fp_choice(type, o, v[o]);

        x = 4 + i * 80;
        gfx_text(x, 118, fp_param_name(type, p), col[i], 2);
        if (c) {
            gfx_text(x, 132, c, GFX_WHITE, 1);
        } else {
            gfx_rect(x, 132, 72, 6, track);
            gfx_rect(x, 132, v[p] * 72 / 32767, 6, col[i]);
        }
        gfx_text(x, 144, fp_param_name(type, o), dim, 1);
        if (co) {
            gfx_text(x + 40, 144, co, dim, 1);
        } else {
            gfx_rect(x + 48, 146, 24, 3, track);
            gfx_rect(x + 48, 146, v[o] * 24 / 32767, 3, dim);
        }
    }
}
