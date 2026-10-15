/* DRUM ROLL voice post-process (spec: README.md): after TE rendered a drum voice (128 samples
 * at 88.2 kHz) its sample's filter (Simper trapezoidal SVF, as TRIO), the hit's gain and the length
 * gate. Pure C, host-tested by test_dr_dsp.c. */
#ifndef DR_DSP_H
#define DR_DSP_H

#include <stdint.h>

#define DR_VOICE_RATE   88200
#define DR_FADE         256             /* gate fade-out, voice samples (2.9 ms) */
#define DR_UNITY        32768           /* gain Q15 */

enum { DR_F_OFF, DR_F_LP, DR_F_BP, DR_F_HP, DR_FILTERS };

extern const char *const dr_filter_name[DR_FILTERS];

typedef struct dr_coef {
    int32_t a1, a2, a3, kh;             /* Q31; kh = damping / 2 */
    uint8_t type;
} dr_coef;

typedef struct dr_voice {
    int32_t ic1, ic2;                   /* SVF integrators (samples << 12) */
    int32_t gain;                       /* Q15, DR_UNITY = untouched */
    int32_t gate;                       /* voice samples until the fade, -1 = no gate */
    int32_t fade;                       /* fade samples left once the gate has run out */
    uint8_t key;
} dr_voice;

/* cut, res 0..127 (20 Hz .. 20 kHz exponential; damping 2 .. 0.05) */
void dr_coef_set(dr_coef *c, int type, int cut, int res);
/* a new note on the voice: states cleared; gate < 0 = none */
void dr_voice_start(dr_voice *v, int key, int32_t gain, int32_t gate);
/* in place; c may be 0 (no filter) */
void dr_process(dr_voice *v, int16_t *out, int n, const dr_coef *c);

#endif
