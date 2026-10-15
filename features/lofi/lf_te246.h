/* TE R246 addresses only the LOFI feature uses (voice render contract, pulse engine). Mapped from
 * the R246 disassembly; see README.md. */
#ifndef LF_TE246_H
#define LF_TE246_H

#include <stdint.h>

/* per-sample phase increments of the voice being rendered (int32[128], 2^32 = one cycle at the
 * 88.2 kHz voice rate), filled by TE's voice loop before each render: glide, bend, octave,
 * tune and pitch LFO applied. Every stock engine reads its pitch from here. */
#define LF_TE_VOICE_INC     ((const uint32_t *)0xff801950)

/* pulse engine: vtable 0x019111cc, voices 0x60 bytes each from 0xffb00000 (render lives in
 * encrypted L1: redirect the slot, call the original) */
#define LF_TE_PULSE_RENDER_SLOT 0x019111e0
#define LF_TE_PULSE_RENDER  ((void (*)(void *, int16_t *, int16_t *))0xffa04b88)
#define LF_TE_PULSE_NOTE_ON_SLOT 0x019111e8
#define LF_TE_PULSE_NOTE_ON ((void (*)(void *, int, int))0x01a0dee4)
/* TE's synth knob range check (ReadInterface): range_check(type, slot, int16 *value) -> 1 if
 * ranges[type][slot].min <= *value <= .max (int16 min/max, 8 bytes per slot, 8 slots per type).
 * The patch validator 0x01a037a8 calls it for each of the 8 knobs on every patch apply, boot
 * included, and returns error 15 on the first failure -> fatal "patch.cpp:390 Invalid synth
 * patch". Replaced by lf_te_range_check (entry hook) so pulse slots 5-8 may hold LOFI values. */
#define LF_TE_RANGE_CHECK   0x01a1deb0
#define LF_TE_KNOB_RANGES   ((const volatile int16_t *)0x01de5890)
/* engine browser: picking a row runs "SELECT type FROM synth_types WHERE id=%i" into a
 * std::string (call site 0x01940374 -> 0x0196fbd0(te_string *out, db, sql)), then copies it into
 * the patch JSON "type" and loads the row's default_params as knobs */
#define LF_TE_TYPE_FETCH_SITE 0x01940374
#define LF_TE_VOICE_BASE    0xffb00000u
#define LF_TE_PULSE_SIZE    0x60

#endif
