#!/usr/bin/env python3
"""Generate ac_tables.h: ACID 3's tables. The voice model follows Open303 (Robin Schmidt, MIT, see
NOTICE): the TB-303 filter ("TB_303" mode of rosic::TeeBeeFilter: a linear diode-ladder model after
mystran & kunn, its coefficient / feedback / gain fits), the measured cutoff / env-mod mapping and
the envelope / RC time constants. Here: per-semitone filter coefficients for both filter rates
(2 x: 176.4 kHz, Open303's own fits, used while FILTER FM is on; 1 x: 88.2 kHz, half the cost;
b0 and k fitted so that the filter's response matches the 2 x one, weighted to high resonance;
half: 44.1 kHz for PLAY's ECO tracks: b0 and k at five resonances 0.5 .. 1 so that the resonance
peak's frequency and height match the 2 x filter's, capped at HALF_TOP),
control-rate coefficients (one update per 8 output samples), resonance skew, tanh, exp2 / log2.
Also checks the filter's stability: ring-down at full resonance, and every table entry's poles
(linear model) inside the unit circle at every rate and resonance node; k must stay below 64
(Q25 in an int32).
Run: python3 gen_tables.py (needs numpy; the fit takes a minute)"""
import math, os
import numpy as np

SR = 88200                      # TE's voice rate
FS = 2 * SR                     # the 2 x rate (Open303's fits are made for it)
CTL = SR / 8                    # control updates per second (one per 8 output samples)
TOP = 140                       # semitones above 20 Hz in the filter table (capped at 14 kHz)
HALF_TOP = 108                  # half rate (44.1 kHz): the cutoff stops at 10.2 kHz. The half-rate
                                # k passes 64 at 11.6 kHz (entry 110): Q25 wrapped negative in
                                # ac_core.c, the filter latched up at the rail (+11 .. +20 dB, found
                                # 2026-11-17); the fit is poor that close to Nyquist anyway
HALF_R = (0.5, 0.625, 0.75, 0.875, 1.0)    # half rate: resonance nodes of the k table (below 0.5: node 0)


def arr(name, ctype, vals, comment, per=8):
    out = [f"/* {comment} */", f"static const {ctype} {name}[{len(vals)}] = {{"]
    out += ["    " + ", ".join(str(v) for v in vals[i:i + per]) + "," for i in range(0, len(vals), per)]
    return out + ["};", ""]


def tb303(fc, fs=FS):
    """Open303 TeeBeeFilter::calculateCoefficientsApprox4, TB_303 mode: b0 and k at r = 1"""
    wc = 2 * math.pi * fc / fs
    fx = wc / math.sqrt(2) / (2 * math.pi)
    b0 = (0.00045522346 + 6.1922189 * fx) / (1.0 + 12.358354 * fx + 4.4156345 * fx * fx)
    k = fx * (fx * (fx * (fx * (fx * (fx + 7198.6997) - 5837.7917) - 476.47308) + 614.95611) + 213.87126) + 16.998792
    return b0, k


def q31(x):
    return min((1 << 31) - 1, round(x * (1 << 31)))


def hp_coeff(fc, fs):
    """one-pole high-pass y = a (y1 + x - x1), a = e^(-2 pi fc / fs)"""
    return math.exp(-2 * math.pi * fc / fs)


def run_filter(b0, kr, r, fs, fhp=150.0):
    """float model of the filter (impulse): the last quarter's peak / the first quarter's"""
    n = fs // 4
    k = kr * r
    a = hp_coeff(fhp, fs)
    y1 = y2 = y3 = y4 = 0.0
    hx = hy = 0.0
    pk0 = pk1 = 0.0
    for i in range(n):
        x = 1.0 if i == 0 else 0.0
        f = k * y4
        hy = a * (hy + f - hx)
        hx = f
        y0 = x - hy
        y1 += 2 * b0 * (y0 - y1 + y2)
        y2 += b0 * (y1 - 2 * y2 + y3)
        y3 += b0 * (y2 - 2 * y3 + y4)
        y4 += b0 * (y3 - 2 * y4)
        if i < n // 4:
            pk0 = max(pk0, abs(y4))
        elif i > 3 * n // 4:
            pk1 = max(pk1, abs(y4))
    return pk1 / pk0 if pk0 else 0


def step(s, x, b0, k, a):
    """one sample of the filter (as ac_core.c), state y1..y4, hx, hy"""
    y1, y2, y3, y4, hx, hy = s
    f = k * y4
    hy = a * (hy + f - hx)
    hx = f
    y0 = x - hy
    y1 += 2 * b0 * (y0 - y1 + y2)
    y2 += b0 * (y1 - 2 * y2 + y3)
    y3 += b0 * (y2 - 2 * y3 + y4)
    y4 += b0 * (y3 - 2 * y4)
    return np.array([y1, y2, y3, y4, hx, hy])


FIT_F = 20 * 2 ** np.linspace(0, math.log2(900), 120)     # 20 Hz .. 18 kHz
FIT_R = ((0.0, 0.3), (0.5, 0.6), (0.9, 3.0), (1.0, 3.0))   # resonance, weight (the 303 lives up there)


def resp(b0, k, fs):
    """|H| of the (linear) filter at FIT_F from its state matrices"""
    a = hp_coeff(150.0, fs)
    A = np.array([step(np.eye(6)[i], 0, b0, k, a) for i in range(6)]).T
    B = step(np.zeros(6), 1, b0, k, a)
    z = np.exp(2j * np.pi * FIT_F / fs)
    return np.abs(np.linalg.solve(z[:, None, None] * np.eye(6) - A, np.broadcast_to(B, (len(z), 6))[..., None])[:, 3, 0])


def gain(kr, r):                # Open303's gain compensation (ac_core.c coeffs)
    return ((kr / 17 - 1) * r + 1) * (1 + r)


def golden(fn, lo, hi, n=36):
    g = (math.sqrt(5) - 1) / 2
    c, d = hi - g * (hi - lo), lo + g * (hi - lo)
    fc, fd = fn(c), fn(d)
    for _ in range(n):
        if fc < fd:
            hi, d, fd = d, c, fc
            c = hi - g * (hi - lo)
            fc = fn(c)
        else:
            lo, c, fc = c, d, fd
            d = lo + g * (hi - lo)
            fd = fn(d)
    return (lo + hi) / 2


def fit_rate(fc, fs_to):
    """b0, k at fs_to (88.2 or 44.1 kHz) whose response (with the gain compensation) is closest to the 2 x filter's
    (weighted mean square dB over FIT_F down to -60 dB, at the resonances FIT_R); returns b0, k and
    the rms dB at r = 0.9"""
    b2, k2 = tb303(fc, FS)
    refs = [(r, w, resp(b2, k2 * r, FS) * gain(k2, r)) for r, w in FIT_R]

    def err(b0, kr, only=None):
        e = 0.0
        for r, w, ref in refs:
            h = resp(b0, kr * r, fs_to) * gain(kr, r)
            m = ref > ref.max() * 1e-3
            d = np.mean((20 * np.log10(h[m] / ref[m])) ** 2)
            if only == r:
                return d
            e += w * d
        return e

    b, k = tb303(fc, fs_to)
    b = min(b, 0.45)
    for _ in range(6):          # alternate the two (the error is smooth and single-minimum in each)
        b = golden(lambda x: err(x, k), b * 0.8, min(b * 1.5, 0.499))
        k = golden(lambda x: err(b, x), k * 0.75, k * 1.25)
    return b, k, math.sqrt(err(b, k, 0.9))


def resp_at(b0, kr, r, fs, f):
    """|H| (with the gain compensation) at frequencies f"""
    global FIT_F
    keep, FIT_F = FIT_F, f
    try:
        return resp(b0, kr * r, fs) * gain(kr, r)
    finally:
        FIT_F = keep


def peak(b0, kr, r, fs, fc):
    """the response's maximum near the cutoff fc: frequency, dB (a coarse grid, then a fine one)"""
    f = fc * 2 ** np.linspace(-1.5, 1.0, 300)
    i = int(resp_at(b0, kr, r, fs, f).argmax())
    lo, hi = f[max(i - 1, 0)], f[min(i + 1, len(f) - 1)]
    f = lo * (hi / lo) ** np.linspace(0, 1, 200)
    h = resp_at(b0, kr, r, fs, f)
    return f[h.argmax()], 20 * math.log10(h.max())


def fit_half(fc, b0, k):
    """half rate: b0 (the peak's frequency at r = 1) and k at each of HALF_R (the peak's height),
    starting from fit_rate's b0, k. Near Nyquist one k cannot match both r = 1 and r = 0.9 (r = 1
    came out 1 .. 4 dB weak, r = 0.9 1 .. 2 dB strong above 4 kHz), one k per resonance can."""
    fs = SR // 2
    b2, k2 = tb303(fc, FS)
    f1, h1 = peak(b2, k2, 1.0, FS, fc)
    for _ in range(3):
        k = golden(lambda x: (peak(b0, x, 1.0, fs, fc)[1] - h1) ** 2, k * 0.85, k * 1.15, 28)
        b0 = golden(lambda x: math.log(peak(x, k, 1.0, fs, fc)[0] / f1) ** 2, b0 * 0.92, min(b0 * 1.08, 0.499), 28)
    ks = []
    for r in HALF_R:
        h = peak(b2, k2, r, FS, fc)[1]
        ks.append(k if r == 1.0 else golden(lambda x: (peak(b0, x, r, fs, fc)[1] - h) ** 2, k * 0.5, k * 1.3, 28))
    return b0, ks


def kr_half(ks, r):
    """k at full resonance for resonance r: the nodes interpolated as ac_core.c coeffs()"""
    if r <= HALF_R[0]:
        return ks[0]
    x = (r - HALF_R[0]) * 8
    i = min(int(x), len(HALF_R) - 2)
    return ks[i] + (ks[i + 1] - ks[i]) * (x - i)


def radius(b0, kr, r, fs):
    """the linear filter's largest pole magnitude (state y1..y4 + feedback high-pass)"""
    a = hp_coeff(150.0, fs)
    A = np.array([step(np.eye(6)[i], 0, b0, kr * r, a) for i in range(6)]).T
    return max(abs(np.linalg.eigvals(A)))


b0q, kq, b0q1, kq1, b0qh, kqh, fit_worst, pk_worst, pk_at = [], [], [], [], [], [[] for _ in HALF_R], 0.0, [0.0, 0.0], [None, None]
for st in range(TOP + 1):
    fc = min(20 * 2 ** (st / 12), 14000)
    b0, k = tb303(fc)
    b0q.append(q31(b0))
    kq.append(round(k * (1 << 25)))
    b0, k, e = fit_rate(fc, SR)
    fit_worst = max(fit_worst, e)
    b0q1.append(q31(b0))
    kq1.append(round(k * (1 << 25)))
    if st <= HALF_TOP:
        b0r, kr_, e = fit_rate(fc, SR // 2)
        b0, ks = fit_half(fc, b0r, kr_)
        w = min(1.0, max(0.0, math.log2(fc / 150)))     # below 150 Hz the feedback high-pass blurs the peak:
        b0 = b0r + (b0 - b0r) * w                       # fit_rate's b0, k; to 300 Hz blended into the peak fit
        ks = [kr_ + (x - kr_) * w for x in ks]
        b2, k2 = tb303(fc, FS)
        if 200 <= fc <= 8000:   # the peak's height between the nodes too (below 200 Hz the feedback high-pass blurs it)
            for r in (0.55, 0.7, 0.8, 0.9, 0.95, 1.0):
                d = peak(b0, kr_half(ks, r), r, SR // 2, fc)[1] - peak(b2, k2, r, FS, fc)[1]
                hi = int(r >= 0.7)      # [0]: r 0.55 (mild, a shallow peak), [1]: r 0.7 .. 1 (the 303's squelch)
                if abs(d) > pk_worst[hi]:
                    pk_worst[hi], pk_at[hi] = abs(d), (round(fc), r)
        hb, hk = q31(b0), [round(x * (1 << 25)) for x in ks]
    b0qh.append(hb)             # above HALF_TOP: HALF_TOP's (the cap)
    for j in range(len(HALF_R)):
        kqh[j].append(hk[j])
assert fit_worst < 0.3, f"1 x fit at r = 0.9: {fit_worst} dB rms"
assert pk_worst[1] < 0.3 and pk_worst[0] < 0.5, f"half-rate resonance peak (200 Hz .. 8 kHz): {pk_worst} dB off at (Hz, r) {pk_at}"
assert max(kq + kq1 + sum(kqh, [])) < 1 << 31, "k past Q25's range (ac_core.c reads the tables as int32)"

# stability: full resonance must ring down at every cutoff the engine can reach, at every rate
for fc in (30, 60, 120, 250, 500, 1000, 2000, 4000, 8000, 14000):
    st = min(TOP, round(12 * math.log2(fc / 20)))
    for fs, b0t, krt in ((FS, b0q, kq), (SR, b0q1, kq1), (SR // 2, b0qh, kqh[-1])):
        q = run_filter(b0t[st] / 2 ** 31, krt[st] / 2 ** 25, 1.0, fs)
        assert q < 0.5, f"filter unstable at {fc} Hz, r = 1, {fs} Hz: {q}"
# and every entry's poles inside the unit circle (half rate: at the nodes and between them)
for st in range(TOP + 1):
    for fs, b0t, krt in ((FS, b0q, kq), (SR, b0q1, kq1)):
        rad = radius(b0t[st] / 2 ** 31, krt[st] / 2 ** 25, 1.0, fs)
        assert rad < 1, f"pole outside the unit circle: entry {st}, {fs} Hz: {rad}"
    ks = [kqh[j][st] / 2 ** 25 for j in range(len(HALF_R))]
    for r in (0.25, 0.5, 0.5625, 0.625, 0.6875, 0.75, 0.8125, 0.875, 0.9375, 1.0):
        rad = radius(b0qh[st] / 2 ** 31, kr_half(ks, r), r, SR // 2)
        assert rad < 1, f"pole outside the unit circle: half-rate entry {st}, r = {r}: {rad}"

tnh = [round(math.tanh(-4 + i / 64) * 16384) for i in range(513)]
tnh[-1] = tnh[-2]
ex2 = [round(2 ** (i / 256) * 65536) for i in range(257)]
lg2 = [round(math.log2(1 + i / 256) * 65536) for i in range(257)]
# resonance skew (Open303): r = (1 - exp(-3x)) / (1 - exp(-3)), x = 0..1 in 256 steps, Q15
res = [round((1 - math.exp(-3 * x / 255)) / (1 - math.exp(-3)) * 32767) for x in range(256)]


def c_of(ms):                   # one control update of a decay / leaky integrator, time constant ms, Q31
    return q31(math.exp(-1.0 / (CTL * ms / 1000.0)))


DEC = [30 * 100 ** (c / 255) for c in range(256)]       # DECAY 30 .. 3000 ms (stock 200 .. 2000 inside)
ACD = [30 * 100 ** (n / 31) for n in range(32)]         # ACC DECAY 30 .. 3000 ms (stock 200)
SOFT = [0.3 * 100 ** (n / 31) for n in range(32)]       # SOFT ATK (rc1) 0.3 .. 30 ms (stock 3)
SLIDE = [10 * 36 ** (n / 31) for n in range(32)]        # SLIDE 10 .. 360 ms (stock 60); slew = 0.2 x
SPEED = [60.0, 15.0, 5.0]                               # rc2: SLOW, NORM (Open303: 15 ms), FAST
out = ["/* generated by gen_tables.py - do not edit. Model constants from Open303 (MIT, see NOTICE). */",
       "#ifndef AC_TABLES_H", "#define AC_TABLES_H", "", "#include <stdint.h>", "",
       f"#define AC_TOP {TOP}",
       f"#define AC_HALF_TOP {HALF_TOP} /* half rate: entries above repeat this one (the cutoff stops at 10.2 kHz) */",
       f"#define AC_HALF_R {len(HALF_R)} /* half rate: k nodes at resonance 0.5 .. 1 in steps of 1/8 */",
       f"#define AC_HP_FB {q31(hp_coeff(150.0, FS))} /* feedback high-pass 150 Hz at 176.4 kHz (2 x), Q31 */",
       f"#define AC_HP_PRE {q31(hp_coeff(44.486, FS))} /* pre-filter high-pass 44.5 Hz at 176.4 kHz, Q31 */",
       f"#define AC_HP_FB1 {q31(hp_coeff(150.0, SR))} /* feedback high-pass 150 Hz at 88.2 kHz (1 x), Q31 */",
       f"#define AC_HP_PRE1 {q31(hp_coeff(44.486, SR))} /* pre-filter high-pass 44.5 Hz at 88.2 kHz, Q31 */",
       f"#define AC_HP_FBH {q31(hp_coeff(150.0, SR / 2))} /* feedback high-pass 150 Hz at 44.1 kHz (ECO), Q31 */",
       f"#define AC_HP_PREH {q31(hp_coeff(44.486, SR / 2))} /* pre-filter high-pass 44.5 Hz at 44.1 kHz, Q31 */",
       f"#define AC_HP_POSTH {q31(hp_coeff(24.167, SR / 2))} /* post-filter high-pass 24.2 Hz at 44.1 kHz, Q31 */",
       f"#define AC_HP_POST {q31(hp_coeff(24.167, SR))} /* post-filter high-pass 24.2 Hz at 88.2 kHz, Q31 */",
       f"#define AC_AMP_DEC {c_of(1230.0)} /* amp envelope decay 1230 ms (to 0) */",
       f"#define AC_AMP_REL {c_of(0.5)} /* amp release, normal notes 0.5 ms */",
       f"#define AC_AMP_REL_ACC {c_of(50.0)} /* amp release, accented notes 50 ms */",
       f"#define AC_DECLICK {q31(math.exp(-2 * math.pi * 200 / CTL))} /* amp de-click: 2 x one-pole 200 Hz */",
       f"#define AC_RC_FAST {c_of(0.5)} /* SWEEP OFF: the accent without the sweep circuit */", ""]
out += arr("ac_b0", "uint32_t", b0q, "2 x: filter stage coefficient b0, Q31, per semitone above 20 Hz (capped 14 kHz)")
out += arr("ac_kr", "uint32_t", kq, "2 x: filter feedback k at full resonance, Q25, per semitone")
out += arr("ac_b0_1", "uint32_t", b0q1, "1 x (88.2 kHz, fitted to the 2 x response): b0, Q31, per semitone")
out += arr("ac_kr_1", "uint32_t", kq1, "1 x: k at full resonance, Q25, per semitone")
out += arr("ac_b0_h", "uint32_t", b0qh, "half rate (44.1 kHz, PLAY's ECO; fitted to the 2 x response): b0, Q31")
out += ["/* half rate: k at full resonance, Q25, per resonance node (r = 0.5 0.625 0.75 0.875 1) and semitone */",
        f"static const uint32_t ac_kr_h[{len(HALF_R)}][{TOP + 1}] = {{"]
for j in range(len(HALF_R)):
    out += ["    {"] + ["        " + ", ".join(str(v) for v in kqh[j][i:i + 8]) + "," for i in range(0, TOP + 1, 8)] + ["    },"]
out += ["};", ""]
out += arr("ac_tanh", "int16_t", tnh, "tanh(x), Q14, x = -4 .. 4 in 512 steps of 1/64 (+ guard)", 12)
out += arr("ac_texp2", "uint32_t", ex2, "2^(i/256), Q16")
out += arr("ac_tlog2", "uint32_t", lg2, "log2(1 + i/256), Q16")
out += arr("ac_res", "uint16_t", res, "resonance skew (1 - e^-3x) / (1 - e^-3), Q15", 12)
out += arr("ac_dec", "uint32_t", [c_of(t) for t in DEC], "MEG decay per control update, Q31: DECAY 30 .. 3000 ms", 6)
out += arr("ac_dec_ms", "uint16_t", [round(t) for t in DEC], "DECAY time constants, ms", 12)
out += arr("ac_accdec", "uint32_t", [c_of(t) for t in ACD], "ACC DECAY per control update, Q31: 30 .. 3000 ms", 6)
out += arr("ac_taccdec_ms", "uint16_t", [round(t) for t in ACD], "ACC DECAY, ms", 12)
out += arr("ac_soft", "uint32_t", [c_of(t) for t in SOFT], "rc1 (SOFT ATK) per control update, Q31: 0.3 .. 30 ms", 6)
out += arr("ac_tsoft_10us", "uint16_t", [round(t * 10) for t in SOFT], "SOFT ATK, 0.1 ms", 12)
out += arr("ac_slide", "uint32_t", [c_of(0.2 * t) for t in SLIDE], "pitch slew per control update, Q31: 0.2 x SLIDE, Q31", 6)
out += arr("ac_tslide_ms", "uint16_t", [round(t) for t in SLIDE], "SLIDE, ms", 12)
out += arr("ac_speed", "uint32_t", [c_of(t) for t in SPEED], "rc2 (accent sweep circuit): SLOW NORM FAST, Q31")
out += arr("ac_drive", "uint16_t", [round(2 ** (3 * n / 31) * 256) for n in range(32)], "overdrive input gain, Q8: 1 .. 8")
out += ["#endif", ""]
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "ac_tables.h"), "w").write("\n".join(out))
print("b0", b0q[0], b0q[70], b0q[-1], "k", round(kq[0] / 2 ** 25, 2), round(kq[70] / 2 ** 25, 2), round(kq[-1] / 2 ** 25, 2))
print("1 x: b0", b0q1[0], b0q1[70], b0q1[-1], "k", round(kq1[0] / 2 ** 25, 2), round(kq1[70] / 2 ** 25, 2),
      round(kq1[-1] / 2 ** 25, 2), f"worst fit at r = 0.9 {fit_worst:.2f} dB rms")
print("half: b0", b0qh[0], b0qh[70], b0qh[-1], "k (r = 1)", round(kqh[-1][0] / 2 ** 25, 2), round(kqh[-1][70] / 2 ** 25, 2),
      round(kqh[-1][-1] / 2 ** 25, 2), f"worst resonance peak (200 Hz .. 8 kHz) r 0.55: {pk_worst[0]:.2f} dB at {pk_at[0]}, r 0.7 .. 1: {pk_worst[1]:.2f} dB at {pk_at[1]}")
st = round(12 * math.log2(1000 / 20))
for fs, b0t, krt in ((FS, b0q, kq), (SR, b0q1, kq1), (SR // 2, b0qh, kqh[-1])):
    print(f"stable; {fs} Hz, r=1 ring-down at 1 kHz:", round(run_filter(b0t[st] / 2 ** 31, krt[st] / 2 ** 25, 1.0, fs), 4),
          "r=1.3:", round(run_filter(b0t[st] / 2 ** 31, krt[st] / 2 ** 25, 1.3, fs), 4))
