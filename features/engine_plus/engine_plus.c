/* Engine layer: four extra per-voice parameters for the synth engines that ignore knob slots 5-8
 * (DRIVE, FILTER, RESO, CRUSH, see ep_dsp.c), edited by clicking a knob on the synth screen.
 *
 * ep_render   replaces the voice render function in the engines' vtables ("pointers"): runs
 *             TE's render, then ep_process on the voice's 128 samples with knob slots 5-8.
 * ep_key      on_key: knob click (keys 59-62) on the synth engine screen flips that knob to its
 *             extra parameter and back; both down and up are consumed.
 * ep_knob     on_knob: a flipped knob edits slot 5-8 of the current sound and saves it the way
 *             TE does (TE_PATCH_SET_KNOBS). dsynth/sampler: TE's own slot 5-8 handling (id 4-7).
 * ep_frame    overlay: label + bar in a light knob colour for each flipped knob.
 * Values live in the preset ("knobs" 5-8), so they are saved and loaded with it; on stock
 * firmware the same presets load and the extra values are ignored. */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "ep_dsp.h"

#define KEY_KNOB_PUSH   59                      /* blue 59, green 60, white 61, orange 62 */
#define STEP_SCALE      10485                   /* same feel as TE's dsynth: ~100 clicks full range */
#define MAX_VOICES      16

#include "engine.h"

#define GENERIC { "DRIVE", "FILTER", "RESO", "CRUSH" }, 0, 0, 0, 0
extern const engine ep_hidden[];                /* hidden.c: engines with their own parameters */
static const engine layer[] = {
    { 0x0190ff84, (render_fn)0x019a23d4, TE_INSTR_FM, GENERIC },
    { 0x01911a34, (render_fn)0x01a51f7a, TE_INSTR_DIGITAL, GENERIC },
    { 0x019118c4, (render_fn)0x01a4f098, TE_INSTR_STRING, GENERIC },
    { 0x0190f67c, (render_fn)0x01960288, TE_INSTR_CLUSTER, GENERIC },
    { 0x0191113c, (render_fn)0x01a0bc5c, TE_INSTR_PHASE, GENERIC },
    { 0x019102a4, (render_fn)0x019b48d8, TE_INSTR_DNA, GENERIC },
    { 0x0191185c, (render_fn)0x01a49bf4, TE_INSTR_VOLTAGE, GENERIC },
    { 0x019103cc, (render_fn)0x019b99d4, TE_INSTR_ITER, GENERIC },
};
#define LAYERS (sizeof(layer) / sizeof(layer[0]))

/* the engine description to use: an ep_hidden entry for this vptr wins over the generic one */
static const engine *find(uint32_t vptr, int type)
{
    const engine *e;
    unsigned int i;

    for (e = ep_hidden; e->vptr; e++)
        if (vptr ? e->vptr == vptr : e->type == type)
            return e;
    for (i = 0; i < LAYERS; i++)
        if (vptr ? layer[i].vptr == vptr : layer[i].type == type)
            return &layer[i];
    return 0;
}
static const char *const raw_names[4] = { "P5", "P6", "P7", "P8" };
static const char *const dash[4] = { "--", "--", "--", "--" };

static struct { void *self; ep_voice v; int32_t h[EP_HIDDEN_STATE]; } voices[MAX_VOICES];
volatile uint8_t ep_page[4];                    /* knob flipped to its extra parameter (bench reads) */
static volatile uint8_t consumed;

static int has_layer(int type)
{
    return find(0, type) != 0;
}

static int voice_index(void *self)
{
    int i, free = -1, j;

    for (i = 0; i < MAX_VOICES; i++) {
        if (voices[i].self == self)
            return i;
        if (!voices[i].self && free < 0)
            free = i;
    }
    if (free < 0)
        return -1;
    voices[free].self = self;
    ep_reset(&voices[free].v);
    for (j = 0; j < EP_HIDDEN_STATE; j++)
        voices[free].h[j] = 0;
    return free;
}

void ep_render(void *self, int16_t *out, int16_t *knobs)
{
    const engine *e = find(*(uint32_t *)self, 0);
    int16_t kc[8];
    int on, i, vi;

    if (!e)
        return;                                 /* unknown object: never happens via our vtables */
    on = (knobs[4] | knobs[5] | knobs[6] | knobs[7]) != 0;
    if (!on) {
        e->render(self, out, knobs);            /* off: exactly stock */
        return;
    }
    vi = voice_index(self);
    if (e->before || e->after) {
        /* engine's own hidden parameters: may adjust the voice and a copy of the knobs */
        for (i = 0; i < 8; i++)
            kc[i] = knobs[i];
        if (e->before && vi >= 0)
            e->before(self, knobs + 4, voices[vi].h, kc);
        e->render(self, out, kc);
        if (e->after && vi >= 0)
            e->after(self, knobs + 4, voices[vi].h, kc);
        return;
    }
    e->render(self, out, knobs);
    if (vi >= 0)
        ep_process(&voices[vi].v, out, TE_VOICE_SAMPLES, knobs + 4);
}

/* note-on wrapper for engines whose hidden parameters act once per note (pointer redirect of the
 * voice vtable's note_on slot); p = the current sound's knob slots 5..8 */
void ep_note_on(void *self, int a1, int a2)
{
    const engine *e = find(*(uint32_t *)self, 0);
    int vi, slot;
    const int16_t *p;

    if (!e || !e->note_on)
        return;
    e->note_on(self, a1, a2);
    if (!e->on_note)
        return;
    slot = *TE_SYNTH_SLOT & 7;
    p = (const int16_t *)TE_SYNTH_KNOBS + slot * 8 + 4;
    vi = voice_index(self);
    if (vi >= 0 && (p[0] | p[1] | p[2] | p[3]))
        e->on_note(self, p, voices[vi].h, 0);
}

static int on_engine_screen(void)
{
    return *TE_EDIT_MODE == 0 && *TE_SYNTH_SUBSCREEN == 0;
}

int ep_key(int key, int down)
{
    int k = key - KEY_KNOB_PUSH;

    if (k < 0 || k > 3)
        return 0;
    if (down && !*TE_SHIFT_HELD && on_engine_screen()) {   /* shift + push: not ours */
        ep_page[k] ^= 1;
        consumed |= (uint8_t)(1 << k);
        return 1;
    }
    if (!down && (consumed & (1 << k))) {
        consumed &= (uint8_t)~(1 << k);
        return 1;
    }
    return 0;
}

int ep_knob(int knob, int16_t *diff)
{
    int slot, type, v;
    int16_t *p;

    if ((unsigned int)knob > 3 || !ep_page[knob] || *TE_SHIFT_HELD || !on_engine_screen())
        return 0;
    slot = *TE_SYNTH_SLOT & 7;
    type = TE_SYNTH_TYPE[slot];
    if (has_layer(type)) {
        p = (int16_t *)TE_SYNTH_KNOBS + slot * 8;
        v = p[4 + knob] + ((*diff * STEP_SCALE) >> 15);
        p[4 + knob] = (int16_t)(v < 0 ? 0 : v > 32767 ? 32767 : v);
        TE_PATCH_SET_KNOBS(TE_SYNTH_PATCH(slot), p);
        return 1;
    }
    if (type == TE_INSTR_DSYNTH || type == TE_INSTR_SAMPLER) {
        /* TE's own extra parameters: what shift + knob does there (controller id 4..7) */
        char *ctl = *(char *volatile *)TE_CONTROLLER;
        char *vt = *(char **)(ctl + 4);
        int16_t d = *diff;

        ((void (*)(void *, int, int16_t *))*(uint32_t *)(vt + 0x2c))(ctl + *(int16_t *)(vt + 0x28),
                                                                      knob + 4, &d);
        return 1;
    }
    return 0;
}

void ep_frame(uint16_t *buf)
{
    static const uint16_t light[4] = {
        GFX_RGB(0xbf, 0xd8, 0xff), GFX_RGB(0x80, 0xff, 0xaf),     /* lightblue, lightgreen */
        GFX_RGB(0xae, 0xb1, 0xdc), GFX_RGB(0xff, 0xb3, 0xb3),     /* purplegrey, lightorange */
    };
    int k, slot, type, layered, raw, x;
    int16_t val;
    const engine *e;

    if (!(ep_page[0] | ep_page[1] | ep_page[2] | ep_page[3]) || !on_engine_screen())
        return;
    slot = *TE_SYNTH_SLOT & 7;
    type = TE_SYNTH_TYPE[slot];
    e = find(0, type);
    layered = e != 0;
    raw = type == TE_INSTR_DSYNTH || type == TE_INSTR_SAMPLER;
    gfx_begin(buf);
    for (k = 0; k < 4; k++) {
        if (!ep_page[k])
            continue;
        x = 4 + k * 80;
        gfx_rect(x - 2, 141, 76, 15, GFX_BLACK);
        gfx_text(x, 142, layered ? e->names[k] : raw ? raw_names[k] : dash[k], light[k], 1);
        val = TE_SYNTH_KNOBS[slot * 8 + 4 + k];
        if (layered || raw) {
            gfx_rect(x, 149, 70, 5, GFX_RGB(0x35, 0x32, 0x38));     /* offgrey track */
            gfx_rect(x, 149, (val < 0 ? 0 : val) * 70 / 32767, 5, light[k]);
        }
    }
}
