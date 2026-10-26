/* MALLET voice: modal synthesis of struck bars, pans and bells (marimba, vibraphone, xylophone,
 * glockenspiel, steelpan, handpan, kalimba, tubular bells). Pure C (no TE dependencies): runs on
 * the OP-1 and in the host unit tests (test_mallet_core.c). Spec: README.md.
 *
 * A mallet pulse (half-sine-like, its length a share of the note's period: soft = long = only the
 * low modes, hard = short + a click = all of them) excites up to ML_MODES resonators tuned to the
 * instrument's mode ratios, each with its own decay. Renders TE's voice rate, ML_SR = 88.2 kHz
 * (128 samples per 64-frame audio block), computed at half that rate and interpolated. Pitch comes
 * in as TE's per-sample phase increments (2^32 = one cycle of the note; glide, bend, LFO and tune
 * applied). Parameters are 8 values 0..32767. Output is the raw note (TE's envelope, velocity and
 * volume shape it afterwards). Per-sample work is 32-bit integer maths only, per-block set-up may
 * use 64-bit. */
#ifndef MALLET_CORE_H
#define MALLET_CORE_H

#include <stdint.h>

enum {
    ML_INSTR = 0,   /* instrument: ML_INSTRS values */
    ML_MALLET,      /* soft (yarn) .. hard (plastic, with a click) */
    ML_DECAY,       /* x 1/4 .. x 4 around the instrument's own (centre = as is) */
    ML_STRIKE,      /* centre of the bar (round, few overtones) .. edge (bright) */
    ML_MOTOR,       /* vibraphone motor: 0 off, then 1.2 .. 8 Hz tremolo on the fundamental */
    ML_TUBE,        /* resonator tube: longer, louder fundamental */
    ML_DETUNE,      /* overtones -4 % .. +4 % (centre = the instrument's tuning) */
    ML_TONE,        /* dark .. open */
    ML_PARAMS
};

enum { ML_MARIMBA, ML_VIBES, ML_XYLO, ML_GLOCK, ML_STEELPAN, ML_HANDPAN, ML_KALIMBA, ML_TUBULAR,
       ML_INSTRS };

#define ML_MODES    6
#define ML_SR       88200

typedef struct ml_voice {
    int32_t s1[ML_MODES], s2[ML_MODES];         /* resonator states (coupled form), 8 bits finer */
    uint32_t ex_ph, ex_step;                    /* mallet pulse: phase (2^31 = its end), step */
    int32_t ex_amp;                             /* pulse height, Q16 per 1.0 of area */
    int32_t click;                              /* click share of the pulse, Q15 */
    int32_t lp;                                 /* TONE low-pass state */
    int32_t pitch_lt;                           /* decay shift from the note's pitch, Q16 octaves */
    uint32_t rnd;
    int16_t last;                               /* previous half-rate sample (interpolation) */
    uint8_t strike;                             /* a new strike waits for its first block */
    uint8_t nm;                                 /* modes set up (cache below valid when > 0) */
    uint32_t key_f;                             /* pitch and knobs the set-up was made for */
    int16_t key_p[ML_PARAMS];
    int32_t key_lt;
    int32_t e[ML_MODES], g[ML_MODES], r8[ML_MODES]; /* per mode: coefficient, strike gain, decay / 16 */
} ml_voice;

/* a new strike: ringing modes are damped to a quarter (a restruck bar), the pulse starts with the
 * next block (its length needs the note's pitch) */
void ml_note_on(ml_voice *v);

/* render n samples (n even, <= 128); inc[i] = phase increment of the note at sample i; motor =
 * gain of the fundamental this block (Q15, from ml_motor) */
void ml_render(ml_voice *v, int16_t *out, int n, const uint32_t *inc, const int16_t *p, int32_t motor);

/* vibraphone motor, shared by all voices: advance by one audio block (n samples at ML_SR) and
 * return the fundamental's gain for it (Q15; 32768 when MOTOR is 0) */
int32_t ml_motor(uint32_t *phase, const int16_t *p, int n);

/* knob value -> instrument index, and names for the UI */
int ml_instr(int16_t knob);
const char *ml_instr_name(int i);
/* the instrument's mode count and the mode ratios (Q12, 4096 = the note) for the screen */
int ml_modes(int i);
int ml_ratio(int i, int k);

#endif
