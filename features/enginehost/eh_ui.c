/* Engine host UI helpers (eh_host.h): the "FX page" every hosted engine shows as its page 3 -
 * the sound's FX knobs 1-4 (what TE's FX screen edits), with the FX's own knob names, edited
 * inside TE's range for that FX type and saved through TE's FX save. */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "eh_host.h"

#define STEP_SCALE      10485                   /* ~100 clicks full range */
/* the sound's FX: stored params (8 int16 per sound), type (TE_FX_*), on (byte), save (JSON + save
 * queue); FX ranges {int16 min, max, ?} per knob at 0x01de5c50 + type * 0x30 (TE's Ranges init) */
#define EH_TE_SYNTH_FX       ((volatile int16_t *)0xff801444)    /* + slot * 8 */
#define EH_TE_SYNTH_FX_TYPE  ((volatile int32_t *)0xff801734)    /* [8] */
#define EH_TE_SYNTH_FX_ON    ((volatile uint8_t *)0x01aaac20)    /* [8] */
#define EH_TE_PATCH_SET_FX   ((void (*)(void *patch, int16_t *p8))0x019ff664)
#define EH_TE_FX_RANGES      ((const volatile int16_t *)0x01de5c50)
#define EH_TE_FX_TYPES       8

/* TE's range of FX knob i for FX type t */
static void fx_range(int t, int i, int *lo, int *hi)
{
    const volatile int16_t *r = EH_TE_FX_RANGES + t * 24 + i * 3;

    *lo = r[0];
    *hi = r[1];
}

/* page 3: the sound's FX knob, ~100 clicks over TE's range; FX off: nothing to edit */
int eh_fx_knob(int slot, int knob, int16_t diff)
{
    volatile int16_t *f = EH_TE_SYNTH_FX + slot * 8;
    int t = EH_TE_SYNTH_FX_TYPE[slot], lo, hi, v;

    if ((unsigned int)t >= EH_TE_FX_TYPES || !EH_TE_SYNTH_FX_ON[slot])
        return 1;
    fx_range(t, knob, &lo, &hi);
    v = f[knob] + ((((diff * STEP_SCALE) >> 15) * (hi - lo)) >> 15);
    f[knob] = (int16_t)(v < lo ? lo : v > hi ? hi : v);
    EH_TE_PATCH_SET_FX(TE_SYNTH_PATCH(slot), (int16_t *)f);
    return 1;
}

/* knob names of the sound's FX. TE's from the OP-1 guide's effects reference (delay "solid state
 * delay", grid "three dimensional feedback plate", nitro "dual resonant turbo filter", phone
 * "hacked telephone system", punch "hard hitting low pass filter", spring "mathematic reverb", cwo
 * "pitch shifting delay"); the SDK effects hosted on them (marker in params 5-8) by their own page 1 */
static const char *const te_fx[EH_TE_FX_TYPES][5] = {
    { "DELAY", "SIZE", "SPEED", "FEEDBACK", "MIX" },
    { "GRID", "X SIZE", "Y SIZE", "Z FEEDBK", "MIX" },
    { "PUNCH", "FREQ", "PUNCH", "ROUNDS", "POWER" },
    { "PHONE", "TONE", "PHONIC", "BAUD", "TELEMATIC" },
    { "SPRING", "TONE", "TURNS", "DAMPING", "SEND" },
    { "FILTER", "FX 1", "FX 2", "FX 3", "FX 4" },
    { "NITRO", "FREQ", "FOLLOW", "RES", "FREQ 2" },
    { "CWO", "FREQ", "DELAY", "FEEDBACK", "SIDEBAND" },
};
static const char *const sdk_fx[9][5] = {
    { "RESO", "DECAY", "TONE", "CHORD", "MIX" },
    { "SHIMMER", "DECAY", "TONE", "SHIMMER", "MIX" },
    { "PHASER", "RATE", "DEPTH", "FEEDBACK", "MIX" },       /* FX MIX types, its TYPE order */
    { "FLANGER", "RATE", "DEPTH", "FEEDBACK", "MIX" },
    { "FOLD", "DRIVE", "BIAS", "TONE", "MIX" },
    { "FUZZ", "DRIVE", "TONE", "CHAR", "MIX" },
    { "DJ FILTER", "FILTER", "RESO", "DRIVE", "MIX" },
    { "TRANSIENT", "ATTACK", "SUSTAIN", "OUTPUT", "MIX" },
    { "TEXTURE", "KIND", "LEVEL", "TONE", "BREATH" },
};

/* name + 4 knob names of the sound's FX (t = TE_FX_*, f = its stored params) */
static const char *const *fx_names(const volatile int16_t *f, int t)
{
    int m = f[4] & 0xe000;

    if ((f[5] & 0xe000) == m && (f[6] & 0xe000) == m && (f[7] & 0xe000) == m) {
        if (t == TE_FX_CWO && m == 0x2000)
            return sdk_fx[0];
        if (t == TE_FX_SPRING && m == 0x4000)
            return sdk_fx[1];
        if (t == TE_FX_PHONE && m == 0x6000) {          /* FX MIX: TYPE in param 5 */
            int type = (((f[4] & 0x1fff) << 2) * 7) >> 15;

            return sdk_fx[2 + (type > 6 ? 6 : type)];
        }
    }
    return te_fx[t];
}

void eh_fx_draw(int slot, int y)
{
    static const uint16_t col[4] = {
        GFX_RGB(0x69, 0x8e, 0xff), GFX_RGB(0x00, 0xed, 0x95),   /* blue, green */
        GFX_RGB(0xe6, 0xe6, 0xe6), GFX_RGB(0xff, 0x6a, 0x3d),   /* white, orange */
    };
    static const uint16_t dim = GFX_RGB(0x5a, 0x58, 0x66), track = GFX_RGB(0x35, 0x32, 0x38);
    volatile int16_t *f = EH_TE_SYNTH_FX + slot * 8;
    int t = EH_TE_SYNTH_FX_TYPE[slot], lo, hi, i, x;
    const char *const *names;

    if ((unsigned int)t >= EH_TE_FX_TYPES) {
        gfx_text(4, y, "NO FX", dim, 2);
        return;
    }
    names = fx_names(f, t);
    x = gfx_text(4, y, "FX ", dim, 2);
    gfx_text(x, y, names[0], col[1], 2);
    if (!EH_TE_SYNTH_FX_ON[slot]) {
        gfx_text(4, y + 18, "FX IS OFF: TURN IT ON IN T3", dim, 2);
        return;
    }
    for (i = 0; i < 4; i++) {
        x = 4 + i * 80;
        fx_range(t, i, &lo, &hi);
        gfx_text(x, y + 16, names[1 + i], col[i], 2);
        gfx_rect(x, y + 30, 72, 6, track);
        if (hi > lo)
            gfx_rect(x, y + 30, (f[i] - lo) * 72 / (hi - lo), 6, col[i]);
    }
}
