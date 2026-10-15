/* TE R246 addresses only the RESO feature uses (FX hosting on CWO, voice notes). Mapped from the
 * R246 disassembly + TE's Ranges init run in the simulator; see README.md. */
#ifndef RS_TE246_H
#define RS_TE246_H

#include <stdint.h>

/* CWO effect (fx id 7): mono process(this, int16 buf[64], int16 params[8]) in place, stereo
 * process(this, l[64], r[64], params[8]); params = per-block copy (stored values + LFO). Its 8
 * fx_params all have range 0..32767 (Ranges table 0x01de5c50), and no stock FX process reads
 * params 4..7. Mono object at 0xffb00600 (synth pos 0 / drum pos 1 share it), pos at +0xe4. */
#define RS_TE_CWO_ID            7
#define RS_TE_CWO_MONO_SLOT     0x0190f588
#define RS_TE_CWO_MONO          ((void (*)(void *, int16_t *, int16_t *))0x0194480c)
#define RS_TE_CWO_STEREO_SLOT   0x0190f5a8
#define RS_TE_CWO_STEREO        ((void (*)(void *, int16_t *, int16_t *, int16_t *))0x019450fc)
#define RS_TE_CWO_POS(obj)      (*(volatile int32_t *)((char *)(obj) + 0xe4))

/* stored FX params (8 int16 per sound), FX type per sound (int32), FX on (byte) */
#define RS_TE_SYNTH_FX          ((volatile int16_t *)0xff801444)    /* + slot * 8 */
#define RS_TE_DRUM_FX           ((volatile int16_t *)0xff8015c4)    /* + slot * 8 */
#define RS_TE_MASTER_FX         ((volatile int16_t *)0xff8016c4)
#define RS_TE_SYNTH_FX_TYPE     ((volatile int32_t *)0xff801734)    /* [8] */
#define RS_TE_SYNTH_FX_ON       ((volatile uint8_t *)0x01aaac20)    /* [8] */
#define RS_TE_PATCH_SET_FX      ((void (*)(void *patch, int16_t *p8))0x019ff664)   /* JSON + save */

/* synth voices (6): sounding flag, target phase increment at 88.2 kHz (set by VoiceCtl note-on
 * 0x01a84414; 2^32 = one cycle) */
#define RS_TE_VOICES            6
#define RS_TE_VOICE_ON          ((volatile uint8_t *)0xff80105c)
#define RS_TE_VOICE_INC         ((volatile uint32_t *)0xff8012d4)

/* FX list: picking a row reads its type name with 0x0196fbd0(te_string *out, db, sql) at these
 * call sites ("SELECT type FROM fx_types WHERE id=%i"), then stores it as "fx_type" */
#define RS_TE_FX_FETCH_SITE     0x019ff3f4      /* synth / drum FX (patch set fx type 0x019ff3b0) */
#define RS_TE_MASTER_FETCH_SITE 0x019da9b0      /* master FX (mixer) */

#endif
