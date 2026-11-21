/* py_tev: TE's own synth engines (fm, digital, string, cluster, pulse, sampler, phase, dna, dsynth,
 * voltage, iter) on PLAY's SYNTH tracks. Static RE 2026-11-20 (README "TE's engines"):
 *
 * TE builds the 6 voices of the current sound's engine with its factory (TE_ENGINE_NEW,
 * producer_factory.cpp) as small C++ objects at 0xffb00000 + vi * size in the L1 scratchpad;
 * engine-wide state (fm's operator setup, ...) sits right after them, below the FX objects at
 * 0xffb00600. Measured extents (sim, TE's own factory): fm 0x1a4, digital 0xc0, string 0xbd,
 * cluster 0x287, pulse 0x260, sampler 0x60, phase 0xa8, dna 0xc0, dsynth 0x1b0, voltage 0x135,
 * iter 0xd5 bytes. A voice is driven through its vtable ({int32 this-delta, fn} slots):
 * note_on +0x1c (this, knobs), render +0x14 (this, int16 out[128], knobs: 128 samples at
 * 88.2 kHz, the pitch increments from 0xff801950), release +0x2c (this, knobs). Their code reads
 * nothing per voice outside the object but TE's per-voice buffers that the caller fills
 * (increments 0xff801950, dsynth: the envelope 0xff801d50), read-only tables, shared scratch and
 * the dither LCG; the sampler's voice holds the slot's sample object at +4.
 *
 * So PLAY keeps each instance's copy of that region (PY_TEV_REGION bytes) and swaps it in for the
 * track's block (py_tev_enter / py_tev_leave, audio interrupt, after TE's own voices rendered):
 * TE's voices are saved and put back, the objects run at the addresses they were built for.
 * Fresh voices come from a prototype per engine type, built in the main loop with interrupts
 * masked (py_tev_main: TE's region saved, TE_ENGINE_NEW, copied, restored). */
#ifndef PY_TEV_H
#define PY_TEV_H

#include <stdint.h>

#define PY_TEV_REGION   0x300                   /* bytes of scratch swapped (largest: cluster 0x287) */
#define PY_TEV_INST     7                       /* = EH_INST: instances 1..6 are PLAY's */
#define PY_TEV_PROTOS   8                       /* at most 8 different engines in the 8 sound slots */

/* the engine type can play on a SYNTH track through TE's voices (not drwave: those are the SDK's) */
int py_tev_type_ok(int type);

/* GUI: can sound slot `slot` (a TE engine) play? 0 = yes, PY_TEV_NOT = not an engine PLAY plays,
 * PY_TEV_NO_SAMPLE = a sampler sound without a sample */
#define PY_TEV_NOT          1
#define PY_TEV_NO_SAMPLE    2
int py_tev_why(int slot);

/* main loop: a prototype for every TE engine type a synth sound slot holds (built once per type) */
void py_tev_main(void);

/* audio: instance inst plays sound slot `slot`, a TE engine. 0 = not playable now (no prototype
 * yet, or a sampler sound without a sample); 1 = ready; 2 = ready with fresh voices (the engine
 * changed: the caller restarts its notes) */
int py_tev_bind(int inst, int slot);

/* audio: the instance's voices into TE's scratch (TE's own saved) and back; every call below
 * between the two */
void py_tev_enter(int inst);
void py_tev_leave(int inst);

/* audio: voice v (0..5) of the instance renders 128 samples into out. flags & PY_SYN_FRESH: its
 * note_on first. env: the voice's envelope of this block (128, dsynth reads it), or 0 */
int py_tev_render(int inst, int v, int16_t *out, const int16_t *knobs, const uint32_t *inc,
                  const int16_t *env, int flags);

/* audio: the note-off of voice v (TE's release: strings damp, the sampler's loop ends, ...) */
void py_tev_key_up(int inst, int v, const int16_t *knobs);

#endif
