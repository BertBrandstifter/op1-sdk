/* FXDIAG (temporary debug overlay): why does a master FX of ours not sound? Draws three lines at
 * the bottom of every screen:
 *   EM <edit mode>  MFX <stored master fx_params 1-8, hex>
 *   ST <master FX object> <its vtable> <vtable +0x14 = process TE calls>
 *   RS/SH/FP <stereo calls> <stereo calls with our marker> <mono calls> (low 16 bits, hex)
 * Counters are optional (weak): a feature missing from the image shows "--". */
#include <op1/te246.h>
#include <op1/gfx.h>

extern volatile uint32_t rs_n_mono __attribute__((weak)), rs_n_stereo __attribute__((weak)),
    rs_n_stereo_marked __attribute__((weak));
extern volatile uint32_t sh_n_mono __attribute__((weak)), sh_n_stereo __attribute__((weak)),
    sh_n_stereo_marked __attribute__((weak));
extern volatile uint32_t fp_n_mono __attribute__((weak)), fp_n_stereo __attribute__((weak)),
    fp_n_stereo_marked __attribute__((weak));

#define MASTER_FX   ((volatile int16_t *)0xff8016c4)
#define STEREO_OBJ  ((void *volatile *)0x01aaad68)      /* TE's master FX object pointer */

static int counters(int x, int y, const char *name, volatile uint32_t *st, volatile uint32_t *mk,
                    volatile uint32_t *mo, uint16_t c)
{
    x = gfx_text(x, y, name, c, 1) + 4;
    if (!st)
        return gfx_text(x, y, "--", c, 1) + 8;
    x = gfx_hex(x, y, *st & 0xffff, 4, c, 1) + 4;
    x = gfx_hex(x, y, *mk & 0xffff, 4, c, 1) + 4;
    return gfx_hex(x, y, *mo & 0xffff, 4, c, 1) + 10;
}

void fd_frame(uint16_t *buf)
{
    uint32_t obj = (uint32_t)*STEREO_OBJ, vt = 0, slot = 0;
    int i, x, y = gfx_h - 22;

    gfx_begin(buf);
    y = gfx_h - 22;
    if (obj >= 0xffb00000u && obj < 0xffb01000u) {
        vt = *(volatile uint32_t *)obj;
        if (vt >= 0x01000000u && vt < 0x02000000u)
            slot = *(volatile uint32_t *)(vt + 0x14);
    }
    gfx_rect(0, y - 1, gfx_w, 22, GFX_BLACK);
    x = gfx_text(2, y, "EM", GFX_WHITE, 1) + 4;
    x = gfx_dec(x, y, *TE_EDIT_MODE, GFX_WHITE, 1) + 8;
    x = gfx_text(x, y, "MFX", GFX_WHITE, 1) + 4;
    for (i = 0; i < 8; i++)
        x = gfx_hex(x, y, (uint16_t)MASTER_FX[i], 4, i < 4 ? GFX_WHITE : GFX_RGB(0xff, 0x6a, 0x3d), 1) + 4;
    y += 7;
    x = gfx_text(2, y, "ST", GFX_WHITE, 1) + 4;
    x = gfx_hex(x, y, obj, 8, GFX_WHITE, 1) + 4;
    x = gfx_hex(x, y, vt, 8, GFX_WHITE, 1) + 4;
    gfx_hex(x, y, slot, 8, GFX_RGB(0x00, 0xed, 0x95), 1);
    y += 7;
    x = counters(2, y, "RS", &rs_n_stereo, &rs_n_stereo_marked, &rs_n_mono, GFX_RGB(0x69, 0x8e, 0xff));
    x = counters(x, y, "SH", &sh_n_stereo, &sh_n_stereo_marked, &sh_n_mono, GFX_RGB(0x00, 0xed, 0x95));
    counters(x, y, "FP", &fp_n_stereo, &fp_n_stereo_marked, &fp_n_mono, GFX_RGB(0xff, 0x6a, 0x3d));
}
