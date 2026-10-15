/* WHEEL voice: tonewheel organ (9 drawbars, percussion, key click, drive, rotary speaker). Pure C
 * (no TE dependencies): runs on the OP-1 and in the host unit tests (test_wheel_core.c). Spec:
 * README.md.
 *
 * Renders TE's voice rate, WH_SR = 88.2 kHz (128 samples per 64-frame audio block), computed at
 * half that rate and interpolated. Pitch comes in
 * as TE's per-sample phase increments (2^32 = one cycle of the 8' fundamental; glide, bend, LFO
 * and tune applied). Parameters are 8 values 0..32767. Output is a raw, full-length note (TE's
 * envelope, velocity and volume shape it afterwards). Per-sample work is 32-bit integer maths
 * only, per-block set-up may use 64-bit. */
#ifndef WHEEL_CORE_H
#define WHEEL_CORE_H

#include <stdint.h>

enum {
    WH_REG = 0,     /* registration: 16 classic drawbar settings */
    WH_PERC,        /* percussion: OFF 2ND 3RD 2ND SOFT 3RD SOFT 2ND SLOW 3RD SLOW */
    WH_DRIVE,       /* tube-style overdrive (0 = clean) */
    WH_ROTARY,      /* rotary speaker: OFF / SLOW / FAST (speed eases between them) */
    WH_BASS,        /* 16' + 5 1/3' bars -8..+8 around the registration (centre = as is) */
    WH_BODY,        /* 8' + 4' bars -8..+8 */
    WH_TOP,         /* 2 2/3' .. 1' bars -8..+8 */
    WH_CLICK,       /* key click level (0 = none) */
    WH_PARAMS
};

#define WH_BARS     9
#define WH_REGS     16
#define WH_PERCS    7
#define WH_ROTS     3
#define WH_SR       88200
#define WH_LEVEL    18000                       /* peak of one full drawbar: headroom for percussion,
                                                 * click and chords (24576 clipped, v0.6) */

/* rotor state: one per sound, shared by all voices (advanced once per audio block) */
typedef struct wh_rotor {
    uint32_t horn0, horn1;      /* horn angle at the start / end of this block, 2^32 = one turn */
    uint32_t drum0, drum1;      /* drum (bass rotor) angle */
    int32_t horn_hz, drum_hz;   /* current speeds, Q16 Hz (ease to the selected speed) */
} wh_rotor;

typedef struct wh_voice {
    uint32_t qh, qd;            /* phase of the 16' bar (half the note) for horn / drum partials */
    int32_t perc;               /* percussion envelope, Q30 */
    int32_t click;              /* click envelope, Q30 */
    int32_t hp;                 /* click high-pass state */
    uint32_t lfsr;              /* click noise */
    int16_t last;               /* previous half-rate sample (interpolation) */
} wh_voice;

/* start a note: phases restart (tonewheels are free-running on a real organ; a fixed start keeps
 * every note the same), percussion and click fire */
void wh_note_on(wh_voice *v);

/* advance the rotors by one audio block (call once per block, before the voices) */
void wh_rotor_block(wh_rotor *r, const int16_t *p, int n);

/* render n samples (n <= 128); inc[i] = phase increment of the 8' fundamental at sample i */
void wh_render(wh_voice *v, const wh_rotor *r, int16_t *out, int n, const uint32_t *inc,
               const int16_t *p);

/* knob value -> registration / percussion / rotary index, bar offset, and names for the UI */
int wh_reg(int16_t knob);
int wh_perc(int16_t knob);
int wh_rot(int16_t knob);
int wh_offset(int16_t knob);
const char *wh_reg_name(int i);
const char *wh_perc_name(int i);
const char *wh_rot_name(int i);
/* drawbar level 0..8 of bar b for these parameters (registration + offsets) */
int wh_bar(const int16_t *p, int b);

#endif
