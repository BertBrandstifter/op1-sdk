/* PALETTE bank: own patches (included by pl_core.c). Tone fields in pl_tone_def order:
 * wave, level, pitch step, fine, TVF mode, cutoff, reso, TVF env depth, {TVF T1..T4},
 * {TVF L1 L2 SUS L4}, {TVA T1..T4}, {TVA L1 L2 SUS}, TVF key follow, LFO depth pitch / TVF / TVA,
 * LFO select, pitch env L0 L1 (cents) T1 T2, pitch key follow - 100.
 * Times 0..100: 20 = 6 ms, 40 = 52 ms, 50 = 140 ms, 60 = 380 ms, 70 = 1 s, 80 = 2.8 s, 90 = 7.4 s.
 * Cutoff 0..100: 30 = 160 Hz, 50 = 660 Hz, 60 = 1.3 kHz, 70 = 2.7 kHz, 80 = 5.4 kHz, 90 = 11 kHz. */

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"   /* trailing fields = 0 */

/* palette PITCH step of n semitones (pl_pitch_st) */
#define ST(n)   ((n) == -24 ? 0 : (n) == -19 ? 1 : (n) == -17 ? 2 : (n) >= -12 && (n) <= 12 ? (n) + 15 \
                 : (n) == 17 ? 28 : (n) == 19 ? 29 : (n) == 24 ? 30 : 31)
/* an unused tone: raising its LEVEL gives a plain saw */
#define INIT    { PL_SAW, 0, ST(0), 0, PL_LPF, 100, 0, 0, {0, 0, 0, 40}, {0, 0, 0, 0}, {0, 0, 0, 40}, {100, 100, 100} }
#define SUS3    {100, 100, 100}
#define LFOS    { { 55, 50, 25, PL_LTRI, 0, 0 }, { 25, 0, 0, PL_LTRI, 0, 0 } }

const pl_patch pl_patches[] = {
    { "DIGI PAD", {
        { PL_DIGI1, 80, ST(0), 4, PL_LPF, 62, 15, 20, {65, 70, 70, 70}, {40, 20, 10, 0}, {60, 70, 70, 68}, {100, 90, 80}, 0, 2 },
        { PL_SOFTPAD, 70, ST(12), -5, PL_LPF, 70, 0, 0, {0, 0, 0, 0}, {0, 0, 0, 0}, {68, 0, 0, 72}, SUS3 },
        { PL_SAW, 45, ST(0), -9, PL_LPF, 50, 25, 25, {72, 72, 75, 70}, {50, 10, 0, 0}, {62, 70, 0, 70}, SUS3 },
        INIT }, LFOS },
    { "GLASS BELLS", {
        { PL_SINE, 90, ST(0), 0, PL_FOFF, 100, 0, 0, {0}, {0}, {0, 80, 82, 72}, {100, 40, 0} },
        { PL_SINE, 55, ST(17), 57, PL_FOFF, 100, 0, 0, {0}, {0}, {0, 68, 70, 60}, {100, 30, 0} },
        { PL_GLASS, 40, ST(24), -12, PL_LPF, 85, 0, 0, {0}, {0}, {0, 62, 65, 55}, {100, 20, 0} },
        { PL_WHITE, 30, ST(0), 0, PL_HPF, 85, 10, 0, {0}, {0}, {0, 30, 0, 20}, {100, 0, 0} } },
      { { 60, 60, 30, PL_LTRI, 0, 0 }, { 25, 0, 0, PL_LTRI, 0, 0 } } },
    { "STRINGS", {
        { PL_FATSAW, 80, ST(0), 0, PL_LPF, 64, 10, 10, {60, 70, 0, 65}, {30, 10, 10, 0}, {58, 0, 0, 66}, SUS3, 0, 3 },
        { PL_SAW, 50, ST(12), 6, PL_LPF, 60, 0, 0, {0}, {0}, {62, 0, 0, 66}, SUS3, 0, 3 },
        { PL_PWM, 40, ST(-12), -4, PL_LPF, 55, 0, 0, {0}, {0}, {60, 0, 0, 66}, SUS3 },
        INIT }, { { 55, 55, 30, PL_LTRI, 0, 0 }, { 30, 0, 0, PL_LTRI, 0, 0 } } },
    { "SYNTH BRASS", {
        { PL_SAW, 85, ST(0), 0, PL_LPF, 45, 20, 30, {45, 60, 0, 50}, {50, 25, 25, 0}, {35, 0, 0, 50}, SUS3, 0, 2 },
        { PL_SQUARE, 55, ST(0), 8, PL_LPF, 50, 10, 25, {45, 60, 0, 50}, {50, 25, 25, 0}, {35, 0, 0, 50}, SUS3 },
        { PL_BRASS, 50, ST(12), -6, PL_LPF, 60, 0, 20, {48, 60, 0, 50}, {40, 20, 20, 0}, {40, 0, 0, 50}, SUS3 },
        INIT }, { { 60, 65, 30, PL_LTRI, 0, 0 }, { 25, 0, 0, PL_LTRI, 0, 0 } } },
    { "DIGI PIANO", {
        { PL_EPIANO, 90, ST(0), 0, PL_LPF, 75, 0, 15, {0, 70, 0, 50}, {50, 0, 0, 0}, {0, 75, 82, 55}, {100, 50, 0} },
        { PL_SINE, 45, ST(19), 0, PL_FOFF, 100, 0, 0, {0}, {0}, {0, 55, 0, 50}, {100, 0, 0} },
        { PL_WHITE, 18, ST(0), 0, PL_HPF, 80, 0, 0, {0}, {0}, {0, 25, 0, 20}, {100, 0, 0} },
        INIT }, LFOS },
    { "ORGAN", {
        { PL_ORGAN, 85, ST(0), 0, PL_FOFF, 100, 0, 0, {0}, {0}, {5, 0, 0, 30}, SUS3, 0, 0, 0, 8, 4 },
        { PL_ORGAN, 45, ST(12), 3, PL_FOFF, 100, 0, 0, {0}, {0}, {5, 0, 0, 30}, SUS3, 0, 0, 0, 8, 4 },
        { PL_SINE, 40, ST(19), 0, PL_FOFF, 100, 0, 0, {0}, {0}, {0, 58, 0, 30}, {100, 0, 0} },
        INIT }, { { 55, 50, 25, PL_LTRI, 0, 0 }, { 70, 0, 0, PL_LTRI, 0, 0 } } },
    { "SAW BASS", {
        { PL_SAW, 75, ST(0), 0, PL_LPF, 35, 45, 30, {0, 52, 0, 40}, {50, 0, 0, 0}, {0, 0, 0, 35}, SUS3, 50 },
        { PL_SQUARE, 55, ST(-12), 0, PL_LPF, 40, 10, 20, {0, 50, 0, 40}, {50, 0, 0, 0}, {0, 0, 0, 35}, SUS3, 50 },
        INIT, INIT }, LFOS },
    { "SQUARE LEAD", {
        { PL_SQUARE, 85, ST(0), 0, PL_LPF, 70, 20, 10, {0, 60, 0, 50}, {40, 10, 10, 0}, {0, 0, 0, 45}, SUS3, 0, 4 },
        { PL_SAW, 40, ST(12), 7, PL_LPF, 72, 0, 0, {0}, {0}, {0, 0, 0, 45}, SUS3, 0, 4 },
        INIT, INIT }, { { 62, 62, 30, PL_LTRI, 0, 0 }, { 25, 0, 0, PL_LTRI, 0, 0 } } },
    { "VOX CHOIR", {
        { PL_VOXA, 80, ST(0), 0, PL_LPF, 75, 0, 0, {0}, {0}, {62, 0, 0, 68}, SUS3, 0, 3 },
        { PL_VOXO, 55, ST(12), -4, PL_LPF, 70, 0, 0, {0}, {0}, {65, 0, 0, 68}, SUS3, 0, 3 },
        { PL_WHITE, 15, ST(0), 0, PL_BPF, 78, 30, 0, {0}, {0}, {55, 0, 0, 60}, SUS3 },
        { PL_SOFTPAD, 40, ST(-12), 0, PL_LPF, 60, 0, 0, {0}, {0}, {66, 0, 0, 70}, SUS3 } }, LFOS },
    { "WIND", {
        { PL_WHITE, 80, ST(0), 0, PL_BPF, 60, 70, 0, {0}, {0}, {70, 0, 0, 75}, SUS3, 0, 0, 25, 0, 2 },
        { PL_PINK, 60, ST(0), 0, PL_LPF, 55, 20, 0, {0}, {0}, {72, 0, 0, 75}, SUS3, 0, 0, -15, 0, 2 },
        INIT, INIT }, { { 55, 50, 25, PL_LTRI, 0, 0 }, { 12, 0, 0, PL_LRND, 0, 0 } } },
    { "KALIMBA", {
        { PL_SINE, 90, ST(0), 0, PL_FOFF, 100, 0, 0, {0}, {0}, {0, 62, 0, 45}, {100, 0, 0} },
        { PL_SINE, 40, ST(31), -20, PL_FOFF, 100, 0, 0, {0}, {0}, {0, 40, 0, 30}, {100, 0, 0} },
        { PL_WHITE, 25, ST(0), 0, PL_BPF, 80, 40, 0, {0}, {0}, {0, 22, 0, 15}, {100, 0, 0} },
        INIT }, LFOS },
    { "SWEEP PAD", {
        { PL_SPECTRAL, 75, ST(0), 0, PL_LPF, 40, 40, 40, {85, 85, 0, 75}, {50, 20, 20, 0}, {60, 0, 0, 72}, SUS3, 0, 2 },
        { PL_SAW, 50, ST(0), 9, PL_LPF, 50, 20, 30, {85, 85, 0, 75}, {50, 20, 20, 0}, {60, 0, 0, 72}, SUS3, 0, 2 },
        { PL_SAW, 50, ST(0), -9, PL_LPF, 50, 20, 30, {85, 85, 0, 75}, {50, 20, 20, 0}, {60, 0, 0, 72}, SUS3, 0, 2 },
        INIT }, LFOS },
    { "CLAV", {
        { PL_CLAV, 90, ST(0), 0, PL_LPF, 85, 15, 15, {0, 45, 0, 30}, {50, 0, 0, 0}, {0, 50, 60, 30}, {100, 50, 0} },
        { PL_PULSE12, 40, ST(12), 0, PL_LPF, 80, 0, 0, {0}, {0}, {0, 40, 0, 25}, {100, 0, 0} },
        INIT, INIT }, LFOS },
    { "PLUCK", {
        { PL_SAW, 85, ST(0), 0, PL_LPF, 40, 30, 35, {0, 55, 0, 45}, {50, 0, 0, 0}, {0, 72, 0, 55}, {100, 0, 0} },
        { PL_PULSE25, 45, ST(12), 5, PL_LPF, 50, 0, 30, {0, 55, 0, 45}, {50, 0, 0, 0}, {0, 60, 0, 50}, {100, 0, 0} },
        INIT, INIT }, LFOS },
    { "WIRE HARP", {
        { PL_WIRE, 85, ST(0), 0, PL_LPF, 78, 0, 10, {0, 60, 0, 0}, {40, 0, 0, 0}, {0, 78, 0, 70}, {100, 0, 0} },
        { PL_GLASS, 35, ST(12), 0, PL_FOFF, 100, 0, 0, {0}, {0}, {0, 55, 0, 50}, {100, 0, 0} },
        INIT, INIT }, LFOS },
    { "REED", {
        { PL_REED, 85, ST(0), 0, PL_LPF, 68, 10, 0, {0}, {0}, {25, 0, 0, 40}, SUS3, 0, 3 },
        { PL_WHITE, 12, ST(0), 0, PL_BPF, 75, 30, 0, {0}, {0}, {30, 45, 0, 30}, {100, 30, 30} },
        INIT, INIT }, { { 60, 62, 35, PL_LTRI, 0, 0 }, { 25, 0, 0, PL_LTRI, 0, 0 } } },
    { "SPACE BELL", {
        { PL_GLASS, 70, ST(0), 0, PL_LPF, 80, 0, 0, {0}, {0}, {0, 85, 0, 78}, {100, 0, 0} },
        { PL_SINE, 45, ST(31), 30, PL_FOFF, 100, 0, 0, {0}, {0}, {0, 75, 0, 70}, {100, 0, 0} },
        { PL_SPECTRAL, 30, ST(0), 0, PL_HPF, 70, 0, 0, {0}, {0}, {70, 0, 0, 80}, SUS3, 0, 0, 0, 20 },
        { PL_SINE, 40, ST(-12), 0, PL_FOFF, 100, 0, 0, {0}, {0}, {55, 0, 0, 78}, SUS3 } }, LFOS },
    { "ZAP BASS", {
        { PL_SYNBASS, 90, ST(0), 0, PL_LPF, 45, 35, 25, {0, 48, 0, 40}, {50, 0, 0, 0}, {0, 0, 0, 35}, SUS3,
          0, 0, 0, 0, 0, 2400, 0, 38, 0 },
        { PL_SINE, 70, ST(-12), 0, PL_FOFF, 100, 0, 0, {0}, {0}, {0, 0, 0, 35}, SUS3 },
        INIT, INIT }, LFOS },
    { "SOFT KEYS", {
        { PL_TRI, 80, ST(0), 0, PL_FOFF, 100, 0, 0, {0}, {0}, {5, 72, 0, 58}, {100, 40, 40} },
        { PL_SINE, 50, ST(12), 0, PL_FOFF, 100, 0, 0, {0}, {0}, {5, 65, 0, 58}, {100, 20, 20} },
        { PL_DIGI2, 25, ST(12), 5, PL_LPF, 60, 0, 0, {0}, {0}, {0, 50, 0, 50}, {100, 0, 0} },
        INIT }, LFOS },
    { "BREATH PAD", {
        { PL_WHITE, 50, ST(0), 0, PL_HPF, 70, 20, 0, {0}, {0}, {40, 62, 0, 50}, {100, 20, 0} },
        { PL_SOFTPAD, 75, ST(0), 0, PL_LPF, 65, 0, 0, {0}, {0}, {64, 0, 0, 70}, SUS3 },
        { PL_DIGI1, 40, ST(12), 0, PL_LPF, 60, 0, 0, {0}, {0}, {70, 0, 0, 70}, SUS3, 0, 0, 0, 20, 4 },
        INIT }, { { 55, 50, 25, PL_LTRI, 0, 0 }, { 35, 0, 0, PL_LTRI, 0, 0 } } },
    { "PWM POLY", {
        { PL_PWM, 65, ST(0), 0, PL_LPF, 60, 15, 15, {0, 60, 0, 55}, {40, 10, 10, 0}, {10, 0, 0, 55}, SUS3 },
        { PL_PWM, 50, ST(0), 8, PL_LPF, 60, 15, 15, {0, 60, 0, 55}, {40, 10, 10, 0}, {10, 0, 0, 55}, SUS3 },
        INIT, INIT }, { { 45, 0, 0, PL_LTRI, 0, 0 }, { 25, 0, 0, PL_LTRI, 0, 0 } } },
    { "SYN BASS", {
        { PL_SYNBASS, 90, ST(0), 0, PL_LPF, 50, 20, 20, {0, 50, 0, 40}, {50, 10, 10, 0}, {0, 0, 0, 35}, SUS3 },
        { PL_SINE, 60, ST(-12), 0, PL_FOFF, 100, 0, 0, {0}, {0}, {0, 0, 0, 35}, SUS3 },
        INIT, INIT }, LFOS },
};
const int pl_npatches = (int)(sizeof(pl_patches) / sizeof(pl_patches[0]));
