/* my_tape: independent overlay on the tape screen (feature.json, README.md). */
#include <op1/te246.h>
#include <op1/gfx.h>

static volatile int on;                         /* static: only mt_* symbols are global */
static volatile unsigned ticks;                 /* audio blocks since boot, for own timing */

/* on_key filter (KeyHandler thread): nonzero = consumed, TE never sees the key */
int mt_key(int key, int down)
{
    if (key != TE_KEY_INSERT || !*TE_SHIFT_HELD)
        return 0;
    if (down)
        on = !on;
    return 1;
}

/* on_knob filter: nonzero = consumed while the overlay is open */
int mt_knob(int knob, int16_t *diff)
{
    if (!on)
        return 0;
    (void)knob;
    (void)diff;
    return 1;
}

/* seq_tick: once per 64-frame audio block */
void mt_tick(void)
{
    ticks++;
}

/* frame (GUI thread): draw over TE's frame */
void mt_frame(uint16_t *buf)
{
    if (!on)
        return;
    gfx_begin(buf);
    gfx_text(2, 2, "MY_TAPE", GFX_WHITE, 2);
    gfx_text(2, 14, "OVERLAY", GFX_WHITE, 1);
}
