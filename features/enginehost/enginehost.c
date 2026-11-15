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
static uint8_t block_due;                       /* the block's first rendered voice gets EH_BLOCK */

/* TE's envelope of voice vi (dsp/Adsr.cpp, analysis/notes/re_voice.md): 24-byte objects at
 * 0xff8000d4 + 24 vi, +0 state (4 / 5 release, 8 kill), +8 level Q30. TE renders the envelope
 * before the engine and keeps calling the engine until the envelope is done: with a long RELEASE
 * (TE's release knob low: over 29 s) a voice that is long inaudible still costs a whole render */
#define EH_TE_ADSR(vi)      ((const volatile int32_t *)(0xff8000d4u + 24u * (uint32_t)(vi)))
#define EH_TE_QUIET         (1 << 20)           /* level Q30: -60 dB */

static int te_voice_quiet(int vi)
{
    const volatile int32_t *a = EH_TE_ADSR(vi);
    int32_t st = a[0];

    return (st == 4 || st == 5 || st == 8) && a[2] < EH_TE_QUIET;
}
volatile uint8_t eh_prof;                       /* set by cpuinfo: count the engines' cycles */
volatile uint32_t eh_cycles;                    /* CYCLES spent in SDK engine renders (wraps) */

static uint8_t multi[EH_TAGS];                  /* the engine takes vi 0..EH_SLOTS-1 */

void eh_register(int tag, eh_render_fn render)
{
    if (tag > 0 && tag < EH_TAGS)
        engines[tag] = render;
}

void eh_register_multi(int tag, eh_render_fn render)
{
    if (tag > 0 && tag < EH_TAGS) {
        engines[tag] = render;
        multi[tag] = 1;
    }
}

int eh_multi_ok(const volatile int16_t *knobs)
{
    int t = eh_tag(knobs);

    return t && multi[t] && engines[t];
}

int eh_render_inst(int inst, int v, int16_t *out, const int16_t *knobs, const uint32_t *inc, int flags)
{
    int t = eh_tag(knobs);

    if (!t || !multi[t] || !engines[t] || inst < 1 || inst >= EH_INST || v < 0 || v >= EH_VOICES)
        return 0;
    if (eh_prof) {
        uint32_t c0, c1;

        __asm__ volatile("%0 = CYCLES;" : "=d"(c0));
        engines[t](inst * EH_VOICES + v, out, knobs, inc, flags);
        __asm__ volatile("%0 = CYCLES;" : "=d"(c1));
        eh_cycles += c1 - c0;
        return 1;
    }
    engines[t](inst * EH_VOICES + v, out, knobs, inc, flags);
    return 1;
}

int eh_tag(const volatile int16_t *k)
{
    int t = (k[4] >> 12) & 7, t8 = (k[7] >> 12) & 7;

    if (t == 0 || k[4] < 0 || k[5] < 0 || k[6] < 0 || k[7] < 0
        || ((k[5] >> 12) & 7) != t || ((k[6] >> 12) & 7) != t)
        return 0;
    if (t8 == t)
        return t;                               /* tags 1..7: all four slots alike */
    return t == 7 && t8 >= 1 && t8 <= EH_TAGS - 8 ? 7 + t8 : 0;     /* extended: 7 7 7 x */
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
        block_due = 1;
    last_voice = vi;
    if (!fresh[vi] && te_voice_quiet(vi)) {     /* released below -60 dB: silence, no render */
        int i;

        for (i = 0; i < 128; i++)
            out[i] = 0;
        return;
    }
    if (block_due) {                            /* the first voice rendered in this block */
        block_due = 0;
        flags |= EH_BLOCK;
    }
    if (fresh[vi]) {
        fresh[vi] = 0;
        flags |= EH_FRESH;
    }
    if (eh_prof) {
        uint32_t c0, c1;

        __asm__ volatile("%0 = CYCLES;" : "=d"(c0));
        engines[t](vi, out, knobs, EH_TE_VOICE_INC, flags);
        __asm__ volatile("%0 = CYCLES;" : "=d"(c1));
        eh_cycles += c1 - c0;
        return;
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
