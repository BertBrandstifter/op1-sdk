/* TE R246 addresses only the FXPACK feature uses (FX hosting on phone). Mapped from the R246
 * disassembly, TE's Ranges init run in the simulator and an FX sweep in the simulator; see
 * README.md. */
#ifndef FP_TE246_H
#define FP_TE246_H

#include <stdint.h>

/* phone effect (fx id 3): mono process(this, int16 buf[64], int16 params[8]) in place, stereo
 * process(this, l[64], r[64], params[8]); params = per-block copy (stored values + LFO). Ranges
 * (table 0x01de5c50 + 3 * 0x30): p1 204..20480, p2 3072..17408, p3 1536..16896, p4 0..32767,
 * p5..p8 0..32767. Phone's output does not depend on p5..p8 (sim sweep, mono + stereo). One mono
 * FX object exists at a time (0xffb00600): synth or drum, whichever TE_PLAYMODE plays. */
#define FP_TE_PHONE_ID          3
#define FP_TE_PHONE_MONO_SLOT   0x01911308
#define FP_TE_PHONE_MONO        ((void (*)(void *, int16_t *, int16_t *))0x01a1bb74)
#define FP_TE_PHONE_STEREO_SLOT 0x01911328
#define FP_TE_PHONE_STEREO      ((void (*)(void *, int16_t *, int16_t *, int16_t *))0x01a1c25c)

/* stored FX params (8 int16 per sound), FX type per sound (int32), FX on (byte) */
#define FP_TE_SYNTH_FX          ((volatile int16_t *)0xff801444)    /* + slot * 8 */
#define FP_TE_DRUM_FX           ((volatile int16_t *)0xff8015c4)    /* + slot * 8 */
#define FP_TE_MASTER_FX         ((volatile int16_t *)0xff8016c4)
#define FP_TE_SYNTH_FX_TYPE     ((volatile int32_t *)0xff801734)    /* [8] */
#define FP_TE_SYNTH_FX_ON       ((volatile uint8_t *)0x01aaac20)    /* [8] */
#define FP_TE_PATCH_SET_FX      ((void (*)(void *patch, int16_t *p8))0x019ff664)   /* JSON + save */

#endif
