/* SHIMMER effect (spec: README.md): a reverb whose tail rises an interval on every pass,
 * hosted on TE's spring effect and picked as "shimmer" in TE's FX list.
 *
 * An FX is SHIMMER when its stored fx_params 5-8 all have the form 0x4000 | v (16384..24575): v
 * (13 bit) is SHIMMER's page-2 value. Spring allows 0..32767 there and ignores them, so a SHIMMER
 * sound passes TE's patch validator (boot included) and loads on stock firmware as plain spring.
 * Page 1 = spring's four knobs in spring's ranges, rescaled to 0..32767.
 *
 * sh_te_mono / sh_te_stereo  spring process slots: SHIMMER when the stored params carry the
 *                marker (synth or drum FX: mono, master FX: stereo), otherwise TE's spring
 * sh_te_fx_fetched / sh_te_master_fetched  FX list: the picked row's type "shimmer" becomes
 *                "spring" (its default_params, added by content_sql, carry the defaults + marker)
 * sh_key / sh_knob  synth FX screen of a SHIMMER sound: knob push flips page 1 (TE's knobs) /
 *                page 2 (INTERVAL SIZE MOD LOWCUT, saved through SH_TE_PATCH_SET_FX)
 * sh_frame       SHIMMER page over spring's on the synth FX screen */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "shimmer_core.h"
#include "sh_te246.h"

#define KEY_KNOB_PUSH   59                      /* blue 59 .. orange 62 */
#define STEP_SCALE      10485                   /* ~100 clicks full range */
#define MARK            0x4000
#define MARK_MASK       0xe000

enum { INST_MONO = 0, INST_MASTER, INSTS };

static struct {
    sh_state s;
    uint8_t on;                                 /* ran last block (else reset on start) */
    int8_t mode;                                /* mono: TE_PLAYMODE it ran for */
} inst[INSTS];

volatile uint32_t sh_n_mono, sh_n_stereo, sh_n_stereo_marked;   /* call counters (fxdiag) */
volatile uint8_t sh_page;                       /* synth FX screen: 0 page 1, 1 page 2 (bench) */
static volatile uint8_t consumed;

/* spring's page-1 ranges -> 0..32767: (v - lo) * k >> 15, k = 32767 * 32768 / (hi - lo) */
static const int16_t lo[4] = { 1344, 7744, 0, 0 };
static const int32_t k_norm[4] = { 71087, 123362, 65534, 32768 };

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

/* page 1 from TE's per-block params (stored + LFO, spring's ranges), page 2 from the marker */
static void params(int16_t *p, const int16_t *te, const volatile int16_t *stored)
{
    int i;

    for (i = 0; i < 4; i++) {
        p[i] = norm(i, te[i]);
        p[4 + i] = (int16_t)((stored[4 + i] & 0x1fff) << 2);
    }
}

void sh_te_mono(void *self, int16_t *buf, int16_t *te)
{
    int32_t mode = *TE_PLAYMODE;
    const volatile int16_t *stored = 0;
    int16_t p[SH_PARAMS];

    sh_n_mono++;
    if (mode == *TE_PLAYMODE_TARGET) {          /* not while TE switches synth <-> drum */
        if (mode == 0)
            stored = SH_TE_SYNTH_FX + (*TE_SYNTH_SLOT & 7) * 8;
        else if (mode == 1)
            stored = SH_TE_DRUM_FX + (*TE_DRUM_SLOT & 7) * 8;
    }
    if (!stored || !marked(stored)) {
        inst[INST_MONO].on = 0;
        SH_TE_SPRING_MONO(self, buf, te);       /* stock spring */
        return;
    }
    if (!inst[INST_MONO].on || inst[INST_MONO].mode != mode) {
        sh_reset(&inst[INST_MONO].s);
        inst[INST_MONO].on = 1;
        inst[INST_MONO].mode = (int8_t)mode;
    }
    params(p, te, stored);
    sh_process(&inst[INST_MONO].s, buf, 64, p);
}

void sh_te_stereo(void *self, int16_t *l, int16_t *r, int16_t *te)
{
    int16_t p[SH_PARAMS];

    sh_n_stereo++;
    if (!marked(SH_TE_MASTER_FX)) {
        inst[INST_MASTER].on = 0;
        SH_TE_SPRING_STEREO(self, l, r, te);    /* stock spring */
        return;
    }
    sh_n_stereo_marked++;
    if (!inst[INST_MASTER].on) {
        sh_reset(&inst[INST_MASTER].s);
        inst[INST_MASTER].on = 1;
    }
    params(p, te, SH_TE_MASTER_FX);
    sh_process_stereo(&inst[INST_MASTER].s, l, r, 64, p);
}

/* FX list row "shimmer" -> "spring" right after TE reads its type name (TE knows no FX
 * "shimmer": fatal) */
static void rename(te_string *out)
{
    static const char from[] = "shimmer", to[] = "spring";
    char *d = (char *)te_str_data(out);
    unsigned int i;

    if (out->size != sizeof(from) - 1)
        return;
    for (i = 0; i < sizeof(from) - 1; i++)
        if (d[i] != from[i])
            return;
    for (i = 0; i < sizeof(to); i++)
        d[i] = to[i];
    out->size = sizeof(to) - 1;
    sh_page = 0;
}

void sh_te_fx_fetched(void *out, void *db, const char *sql)     { (void)db; (void)sql; rename(out); }
void sh_te_master_fetched(void *out, void *db, const char *sql) { (void)db; (void)sql; rename(out); }

/* ------------------------------------------------------------------ FX browser: current row */

/* TE's FX browsers find the current row from the stored type name ("spring"); for a SHIMMER sound
 * that row would be spring's, so the cursor jumps there and a confirm loads spring's defaults (our
 * marker gone). Answer with our own row name instead. */
static void *sh_cur_patch;

static const volatile int16_t *sh_fx_of_patch(void *patch)
{
    uint32_t a = (uint32_t)patch, s;

    if (a >= 0x01ddd7e8u && a < 0x01ddd7e8u + 16 * 0x38 && (a - 0x01ddd7e8u) % 0x38 == 0) {
        s = (a - 0x01ddd7e8u) / 0x38;           /* synth patches 0..7, then drum patches 0..7 */
        return s < 8 ? SH_TE_SYNTH_FX + s * 8 : SH_TE_DRUM_FX + (s - 8) * 8;
    }
    return 0;
}

static void sh_name_current(te_string *out, const char *name)
{
    static const char host[] = "spring";
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

void sh_te_fx_patch(void *patch) { sh_cur_patch = patch; }

void sh_te_fx_current(void *node, void *out)
{
    const volatile int16_t *f = sh_fx_of_patch(sh_cur_patch);

    (void)node;
    if (f && marked(f))
        sh_name_current(out, "shimmer");
    sh_cur_patch = 0;
}

void sh_te_master_current(void *out)
{
    if (marked(SH_TE_MASTER_FX))
        sh_name_current(out, "shimmer");
}

/* ------------------------------------------------------------------ KeyHandler thread */

/* where the effect's page is drawn and edited: the synth FX screen of the current sound
 * (*slot = sound), the drum FX screen (*slot = 8 + drum sound) or the mixer's master FX page (*slot = -1); 0 when neither shows a SHIMMER */
static volatile int16_t *synth_fx_shimmer(int *slot)
{
    volatile int16_t *f;

    *slot = *TE_SYNTH_SLOT & 7;
    if (*TE_EDIT_MODE == 0 && *TE_SYNTH_SUBSCREEN == 2 && SH_TE_SYNTH_FX_TYPE[*slot] == SH_TE_SPRING_ID && SH_TE_SYNTH_FX_ON[*slot]) {
        f = SH_TE_SYNTH_FX + *slot * 8;
        return marked(f) ? f : 0;
    }
    *slot = *TE_DRUM_SLOT & 7;                  /* drum FX screen: slot 8..15 */
    if (*TE_EDIT_MODE == TE_EDIT_MODE_DRUM && *TE_DRUM_SUBSCREEN == 2 && TE_DRUM_FX_TYPE[*slot] == SH_TE_SPRING_ID
        && TE_DRUM_FX_ON[*slot]) {
        f = TE_DRUM_FX_PARAMS + *slot * 8;
        *slot += 8;
        return marked(f) ? f : 0;
    }
    if (*TE_EDIT_MODE == TE_EDIT_MODE_MIXER && *TE_MIXER_PAGE == TE_MIXER_PAGE_FX
        && *TE_MASTER_FX_TYPE == SH_TE_SPRING_ID && *TE_MASTER_FX_ON && !TE_MASTER_TRICK[0] && !TE_MASTER_TRICK[1]
        && marked(TE_MASTER_FX_PARAMS)) {
        *slot = -1;
        return TE_MASTER_FX_PARAMS;
    }
    return 0;
}

static void sh_save(volatile int16_t *f, int slot)
{
    if (slot >= 8)
        SH_TE_PATCH_SET_FX(TE_DRUM_PATCH(slot - 8), (int16_t *)f);
    else if (slot >= 0)
        SH_TE_PATCH_SET_FX(TE_SYNTH_PATCH(slot), (int16_t *)f);
    else
        TE_MASTER_CHANGED(TE_MASTER_STATE);
}

int sh_key(int key, int down)
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
    if (*TE_SHIFT_HELD || !synth_fx_shimmer(&slot))
        return 0;
    sh_page ^= 1;
    consumed |= (uint8_t)(1 << b);
    return 1;
}

int sh_knob(int knob, int16_t *diff)
{
    int slot, v;
    volatile int16_t *f;

    if ((unsigned int)knob > 3 || !sh_page || !(f = synth_fx_shimmer(&slot)))
        return 0;                               /* page 1: TE's own spring knobs */
    if (*TE_SHIFT_HELD)
        return 1;
    v = ((f[4 + knob] & 0x1fff) << 2) + ((*diff * STEP_SCALE) >> 15);
    v = v < 0 ? 0 : v > 32767 ? 32767 : v;
    f[4 + knob] = (int16_t)(MARK | (v >> 2));   /* always 16384..24575: in TE's range */
    sh_save(f, slot);
    return 1;
}

/* ------------------------------------------------------------------ GUI thread */

static const char *const intervals[SH_INTERVALS] = { "+12 OCT", "+7 FIFTH", "+19", "+24 2 OCT", "-12 DOWN" };
static const char *const names[SH_PARAMS] = {
    "DECAY", "TONE", "SHIMMER", "MIX", "INTERVAL", "SIZE", "MOD", "LOWCUT",
};

void sh_frame(uint16_t *buf)
{
    static const uint16_t col[4] = {
        GFX_RGB(0x69, 0x8e, 0xff), GFX_RGB(0x00, 0xed, 0x95),
        GFX_RGB(0xe6, 0xe6, 0xe6), GFX_RGB(0xff, 0x6a, 0x3d),
    };
    static const uint16_t dim = GFX_RGB(0x5a, 0x58, 0x66), track = GFX_RGB(0x35, 0x32, 0x38);
    static uint32_t t;
    int16_t v[SH_PARAMS];
    volatile int16_t *f;
    int slot, i, x, pg = sh_page, lvl, shown, speed;

    if (!(f = synth_fx_shimmer(&slot)))
        return;
    for (i = 0; i < 4; i++) {
        v[i] = norm(i, f[i]);
        v[4 + i] = (int16_t)((f[4 + i] & 0x1fff) << 2);
    }
    gfx_begin(buf);
    gfx_rect(0, 0, gfx_w, gfx_h, GFX_BLACK);
    gfx_text(4, 4, "SHIMMER", col[1], 2);
    gfx_text(100, 4, intervals[sh_interval(v[SH_INTERVAL])], GFX_WHITE, 2);
    gfx_text(272, 4, pg ? "P2" : "P1", GFX_WHITE, 2);

    /* rising sparks: as many as the tail is loud, rising as fast as SHIMMER, brighter higher up
     * (down interval: they fall) */
    t++;
    i = slot < 0 ? INST_MASTER : INST_MONO;
    lvl = inst[i].on ? inst[i].s.level : 0;
    shown = lvl >> 8;                           /* 0..~128 */
    if (shown > 48)
        shown = 48;
    speed = 1 + (v[SH_SHIMMER] >> 12);          /* 1..8 px per frame */
    for (i = 0; i < shown; i++) {
        uint32_t h = (uint32_t)i * 2654435761u;
        int sx = 8 + (int)((h >> 8) % 304), y = (int)((t * (uint32_t)speed + (h >> 20)) % 84);
        uint16_t c = y > 56 ? GFX_WHITE : y > 28 ? col[1] : col[0];

        if (sh_interval(v[SH_INTERVAL]) == SH_INTERVALS - 1)
            y = 83 - y;
        gfx_rect(sx, 108 - y, 2, 2, c);
    }

    for (i = 0; i < 4; i++) {
        int p = pg * 4 + i, o = (1 - pg) * 4 + i;

        x = 4 + i * 80;
        gfx_text(x, 118, names[p], col[i], 2);
        gfx_rect(x, 132, 72, 6, track);
        gfx_rect(x, 132, v[p] * 72 / 32767, 6, col[i]);
        gfx_text(x, 144, names[o], dim, 1);
        gfx_rect(x + 48, 146, 24, 3, track);
        gfx_rect(x + 48, 146, v[o] * 24 / 32767, 3, dim);
    }
}
