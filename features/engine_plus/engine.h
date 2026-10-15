/* Engine table shared by engine_plus.c (generic layer) and hidden.c (engine-specific parameters). */
#ifndef EP_ENGINE_H
#define EP_ENGINE_H

#include <stdint.h>

#define EP_HIDDEN_STATE 4                       /* per-voice scratch words for hidden.c */

typedef void (*render_fn)(void *self, int16_t *out, int16_t *knobs);
typedef void (*note_fn)(void *self, int a1, int a2);
/* p = knob slots 5..8 (0 = stock), h = per-voice scratch (zeroed when a voice is first seen),
 * kc = the knob copy TE's render gets (before) / got (after); 0 in on_note */
typedef void (*hidden_fn)(void *self, const int16_t *p, int32_t *h, int16_t *kc);

/* One engine. With before/after: its own hidden parameters around TE's render (hidden.c);
 * without: the generic stages (ep_dsp.c) process the output. note_on/on_note: optional wrapper
 * of the voice's note_on (needs a "pointers" redirect of vptr + 0x1c to ep_note_on). */
typedef struct engine {
    uint32_t vptr;
    render_fn render;
    int type;
    const char *names[4];
    hidden_fn before, after;
    note_fn note_on;
    hidden_fn on_note;
} engine;

#endif
