/* TE R246 addresses only the SHIMMER feature uses (FX hosting on spring). Mapped from the R246
 * disassembly, TE's Ranges init run in the simulator and an FX sweep in the simulator; see
 * README.md. */
#ifndef SH_TE246_H
#define SH_TE246_H

#include <stdint.h>

/* spring effect (fx id 4): mono process(this, int16 buf[64], int16 params[8]) in place, stereo
 * process(this, l[64], r[64], params[8]); params = per-block copy (stored values + LFO). Ranges
 * (table 0x01de5c50 + 4 * 0x30): p1 1344..16448, p2 7744..16448, p3 0..16384, p4 0..32767,
 * p5..p8 0..32767. Spring's output does not depend on p5..p8 (sim sweep, mono + stereo). One mono
 * FX object exists at a time (0xffb00600): synth or drum, whichever TE_PLAYMODE plays. */
#define SH_TE_SPRING_ID         4
#define SH_TE_SPRING_MONO_SLOT  0x019113d0
#define SH_TE_SPRING_MONO       ((void (*)(void *, int16_t *, int16_t *))0x01a326e8)
#define SH_TE_SPRING_STEREO_SLOT 0x019113f0
#define SH_TE_SPRING_STEREO     ((void (*)(void *, int16_t *, int16_t *, int16_t *))0x01a32ffc)

/* stored FX params (8 int16 per sound), FX type per sound (int32), FX on (byte) */
#define SH_TE_SYNTH_FX          ((volatile int16_t *)0xff801444)    /* + slot * 8 */
#define SH_TE_DRUM_FX           ((volatile int16_t *)0xff8015c4)    /* + slot * 8 */
#define SH_TE_MASTER_FX         ((volatile int16_t *)0xff8016c4)
#define SH_TE_SYNTH_FX_TYPE     ((volatile int32_t *)0xff801734)    /* [8] */
#define SH_TE_SYNTH_FX_ON       ((volatile uint8_t *)0x01aaac20)    /* [8] */
#define SH_TE_PATCH_SET_FX      ((void (*)(void *patch, int16_t *p8))0x019ff664)   /* JSON + save */

#endif
