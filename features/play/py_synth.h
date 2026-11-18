/* py_synth: PLAY's synth voices for SYNTH tracks (new_pattern_tape.md 2.2), host-tested.
 *
 * A SYNTH track runs its own instance of an SDK engine (features/enginehost: instance 1..5) on
 * one of the 8 synth sound slots. Per block (64 output frames) and voice: the engine renders 128
 * samples at 88.2 kHz from the slot's stored knobs and PLAY's pitch increments; TE's own envelope
 * code (dsp/Adsr.cpp, called through the api on PLAY's own 24-byte objects) shapes it with the
 * slot's envelope and the note's velocity. The track's voices are summed with the track's gain
 * into a 88.2 kHz bus that the caller decimates once for all tracks (TE's 63-tap halfband,
 * py_syn_decimate; analysis/notes/re_voice.md).
 *
 * Cost savers: at most `cap` voices per track (1 / 2 / 4; the oldest is stolen); a releasing voice
 * stops once its envelope is below -60 dB; ECO: the engine renders every other block, 128 samples
 * at twice the pitch step = two output blocks at 44.1 kHz, no decimation (half the engine cost;
 * the engine's own time constants run at half speed, TE's envelope keeps its speed: two envelope
 * blocks, every other sample; an engine that knows PY_SYN_HALF, as ACID 3, keeps its own times too).
 * ECO tracks render on alternate blocks (`par`: the caller spreads its tracks over both), so their
 * cost does not pile up in one block. With `boost` (the caller's CPU budget) the render carries
 * PY_SYN_BOOST: an engine that knows it (ACID 3) may run at the full rate where its sound needs
 * it. Notes start at the next block (ECO: the next block of the
 * track's parity), new notes render first (a mono engine's new note then needs one render, not
 * two); the note-off comes after the note's hold, then the envelope's release. TE's synth FX and LFO are not
 * applied (one instance each, TE's). */
#ifndef PY_SYNTH_H
#define PY_SYNTH_H

#include <stdint.h>

#define PY_SPOLY        4                       /* voice slots per SYNTH track */
#define PY_SYN_FRESH    0x01                    /* = EH_FRESH: first block of a new note */
#define PY_SYN_BLOCK    0x02                    /* = EH_BLOCK: first voice of the instance */
#define PY_SYN_HALF     0x04                    /* = EH_HALF: ECO, the samples are at 44.1 kHz */
#define PY_SYN_BOOST    0x08                    /* = EH_BOOST: ECO, the engine may do twice the work */
#define PY_INC_A4       21426141u               /* phase increment of MIDI 69 at 88.2 kHz */
#define PY_SYN_CAP_DEFAULT 2                    /* voices of a track whose setting is 0 */
#define PY_ENV_FLOOR    33                      /* -60 dB of full scale: a released voice ends */

typedef struct py_syn_api {
    /* eh_render_inst: 0 = no engine (silence) */
    int (*render)(int inst, int v, int16_t *out, const int16_t *knobs, const uint32_t *inc, int flags);
    void (*env_start)(void *obj);
    void (*env_release)(void *obj, int slot);
    int (*env_render)(void *obj, int16_t *env, const int16_t *adsr, const int16_t *vel);  /* 1 = done */
} py_syn_api;

typedef struct py_sv {
    uint32_t env[6];                            /* TE's Adsr object (24 bytes, word-aligned) */
    uint8_t on, started, released, pad;
    int16_t vel;                                /* velocity << 8 */
    uint32_t inc;                               /* phase increment per 88.2 kHz sample */
    uint32_t gate;                              /* output samples until the note-off */
    uint32_t age;
} py_sv;

typedef struct py_syn {
    py_sv v[PY_SPOLY];
    uint32_t ages;
    uint8_t cap;                                /* voices at most (1..PY_SPOLY; 0 = PY_SPOLY) */
    uint8_t eco;                                /* 1: the engine at 44.1 kHz */
    uint8_t phase;                              /* ECO: 1 = the second half of eco_buf is due */
    uint8_t par;                                /* ECO: renders on blocks of this parity (0 / 1) */
    uint8_t boost;                              /* ECO: the caller's CPU budget allows PY_SYN_BOOST */
    int32_t eco_buf[128];                       /* ECO: two output blocks, gain applied */
} py_syn;

/* pitch increment of a MIDI note (clamped 0..127) */
uint32_t py_syn_inc(int key);

/* a note: MIDI key, velocity 1..127, held `hold` output samples; a free voice among the first `cap`,
 * else the oldest of them */
void py_syn_note(py_syn *s, int key, int vel7, uint32_t hold);

/* one block (number `blk`) of track s: voices rendered by instance `inst` with the slot's knobs and
 * envelope (`slot` for the release), summed with `gain` (Q15). Normal: added into bus88[0..127]
 * (88.2 kHz, the caller decimates); ECO: into out44[0..63]. Returns the block's peak after the gain. */
uint32_t py_syn_block(py_syn *s, const py_syn_api *api, int inst, const int16_t *knobs,
                      const int16_t *adsr, int slot, int32_t gain, int32_t *bus88, int32_t *out44,
                      uint32_t blk);

int py_syn_active(const py_syn *s);

/* nothing sounds but release tails (or nothing at all): no voice holds a note */
int py_syn_tail(const py_syn *s);

/* TE's halfband: 128 samples in (88.2 kHz) -> 64 out (44.1 kHz), hist = 62 samples of state */
void py_syn_decimate(int32_t *hist, const int32_t *in, int32_t *out);

void py_syn_reset(py_syn *s);                   /* every voice off, ECO phase back */

#endif
