/* Engine host (eh_host.h, README.md): owns drwave's render and note_on vtable slots and
 * hands every voice of an SDK-engine sound to that engine; everything else is TE's drwave.
 *
 * eh_te_render   drwave render slot: tag -> registered engine (flags: fresh note, new block),
 *                else TE's drwave render, untouched
 * eh_te_note_on  drwave note_on slot: TE's note_on, then mark the voice for a fresh note */
#include <op1/te246.h>
#include "eh_host.h"

/* drwave engine: vtable 0x0190fb2c, 6 voices of 0x24 bytes from 0xffb00000 (ctor 0x0197ada8,
 * engine factory 0x01a1b7d4 case 2) */
#define EH_TE_DRWAVE_RENDER  ((void (*)(void *, int16_t *, int16_t *))0x0197c870)
#define EH_TE_DRWAVE_NOTE_ON ((void (*)(void *, int, int))0x0197a826)
#define EH_TE_VOICE_BASE     0xffb00000u
#define EH_TE_DRWAVE_SIZE    0x24
/* per-sample phase increments of the voice being rendered (int32[128]; see features/lofi/README.md) */
#define EH_TE_VOICE_INC      ((const uint32_t *)0xff801950)

static eh_render_fn engines[EH_TAGS];
static volatile uint8_t fresh[EH_VOICES];       /* note_on seen, render starts the note */
static int last_voice = EH_VOICES;              /* new audio block when the index does not rise */

void eh_register(int tag, eh_render_fn render)
{
    if (tag > 0 && tag < EH_TAGS)
        engines[tag] = render;
}

int eh_tag(const volatile int16_t *k)
{
    int t = (k[4] >> 12) & 7;

    if (t == 0 || t >= EH_TAGS || k[4] < 0)
        return 0;
    return ((k[5] >> 12) & 0xf) == t && ((k[6] >> 12) & 0xf) == t && ((k[7] >> 12) & 0xf) == t
           && k[5] >= 0 && k[6] >= 0 && k[7] >= 0 ? t : 0;
}

static int voice_index(void *self)
{
    uint32_t a = (uint32_t)self - EH_TE_VOICE_BASE, vi = a / EH_TE_DRWAVE_SIZE;

    return a % EH_TE_DRWAVE_SIZE == 0 && vi < EH_VOICES ? (int)vi : -1;
}

void eh_te_render(void *self, int16_t *out, int16_t *knobs)
{
    int vi = voice_index(self), t = eh_tag(knobs), flags = 0;

    if (!t || vi < 0 || !engines[t]) {
        EH_TE_DRWAVE_RENDER(self, out, knobs);  /* stock drwave */
        return;
    }
    if (vi <= last_voice)                       /* TE renders voices in order once per block */
        flags |= EH_BLOCK;
    last_voice = vi;
    if (fresh[vi]) {
        fresh[vi] = 0;
        flags |= EH_FRESH;
    }
    engines[t](vi, out, knobs, EH_TE_VOICE_INC, flags);
}

void eh_te_note_on(void *self, int a1, int a2)
{
    int vi = voice_index(self);

    EH_TE_DRWAVE_NOTE_ON(self, a1, a2);
    if (vi >= 0)
        fresh[vi] = 1;
}
