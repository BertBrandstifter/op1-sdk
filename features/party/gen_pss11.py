#!/usr/bin/env python3
"""PSS-11 engine data generator.

    gen_pss11.py --pss-rom "<path>/xl437a00.ic4" [--rate 32000] [--out DIR]
        -> the PSS-11's samples from its GEW7 ROM (your own dump): every valid entry of the 512-entry
           sample table at 0x4000 as a sound of its own, the 8- / 12-bit sample bytes as stored, with
           the entry's envelope and LFO bytes; periodic loops tuned to their period. Or
    gen_pss11.py --sf2 "<path>/<sample set>.sf2" [--budget 1500000]
        -> the multisounds of your own sample set (.sf2), converted (ADPCM / PCM16, below).
    Both write (the samples are not ours: build/ has a NOTICE, not MIT, never packed):
           build/pss_data.inc  the tables (compiled in)
           build/pss_rom.bin   the sample data: a content file of the update (content/pss_rom.bin),
                              loaded at runtime into TE's sample memory (pss.c)
           build/pss_rom.inc   the same data as a C array, for the host tests and the sample bench
                              (compiled with -DPSS_ROM_INLINE)
    gen_pss11.py --static
        -> pss_tables.h (filter / pitch / envelope / GEW7 tables), pss_builtin.inc (the built-in waves
           used when there is no sample set, and in the simulator benches), pss_testvec.h (decoder and
           GEW7 envelope test vectors for test_pss_core.c). Own data, MIT; committed.

The GEW7 model (gew_* below) follows MAME's gew.cpp / gew7.cpp (BSD-3-Clause, see NOTICE).

Conversion, per sample (spec: README.md "Data"):
  1. cut at the loop end (looped) or at the -60 dB tail (one-shots, 64-sample fade);
  2. short loops (single cycles) are tuned to their exact period (loop length, whole cycles);
     measure the real bandwidth (smoothed spectrum above -60 dB) and resample to 2.4 x that
     (>= 16 kHz, <= the source rate; samples of <= 8 KB as ADPCM keep the source rate). Loops stay
     exactly periodic: the loop gets an integer length L' and the rate follows (rate x L' / L), so
     pitch is unchanged;
  3. store 4-bit block ADPCM (16 samples in 9 bytes: shift + predictor header, 8 bytes of nibbles;
     decoder in pss_core.c) or PCM16 for tiny samples (single cycles, < 2048 samples).
Every sample stays at its source rate when that fits --budget (the PSS-11 ROM, 31.25 kHz: it does; the room
left keeps the samples ADPCM would hiss most on as PCM16); else the rate factor drops in steps
(2.4 -> 1.8) until it fits.
Needs numpy. Prints a size / quality report."""

import argparse, math, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
FMT_ADPCM, FMT_PCM16, FMT_GEW8, FMT_GEW12 = 0, 1, 2, 3     # GEW: the chip's own formats
NOLOOP = 0xFFFFFFFF
NOMIP = 0xFFFF
# octave-down copies, up to C8: single cycles / tiny samples (PCM16, rich in harmonics) to a step <= 1
# (no folding above 44.1 kHz), ADPCM to <= 4 (pss_core.c's cap; chosen at <= 2 where there is one)
MIP_TOP, MIP_STEP_PCM, MIP_STEP_ADPCM = 108, 1.0, 4.0
BLOCK = 16
FILT = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60), (30, 0), (90, -24), (126, -63)]   # /64
MAX_SHIFT = 13
# lowest stored rate, and samples this small (as ADPCM at the source rate) keep the source rate: lower
# rates put the ADPCM noise and linear interpolation images into the audible band (organ 2 at 8 kHz:
# +8 dB at 5-10 kHz, +20 dB at 10-16 kHz)
MIN_RATE, FULL_RATE_BYTES = 16000.0, 8192
NAME_LEN = 12
# source-rate data (the PSS-11 ROM, 31.25 kHz) fills the budget less: the room left keeps as PCM16 the samples
# whose ADPCM noise rises above the sound in some third octave (1 kHz up) by more than this, worst first
# (E.Piano, MagicOrgan: quiet upper bands that ADPCM fills with noise; noisy sounds mask theirs)
PCM_HISS = 0.0


# ------------------------------------------------------------------ codec (bit-exact with pss_core.c)

def clamp16(v):
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def encode(x):
    """int16 list -> (bytes, decoded list, decoder history (s1, s2) at the start of every block)"""
    n = (len(x) + BLOCK - 1) // BLOCK * BLOCK
    xp = list(x) + [0] * (n - len(x))
    s1 = s2 = 0
    out, dec, hist = bytearray(), [], []
    for b in range(0, n, BLOCK):
        blk = xp[b:b + BLOCK]
        hist.append((s1, s2))
        best = None
        for fi, (a1, a2) in enumerate(FILT):
            t1, t2, mx = s1, s2, 0
            for v in blk:                           # open-loop residual -> shift estimate
                d = v - ((a1 * t1 + a2 * t2) >> 6)
                mx = max(mx, abs(d))
                t2, t1 = t1, v
            sh0 = 0
            while sh0 < MAX_SHIFT and (7 << sh0) < 2 * mx:
                sh0 += 1
            for sh in range(max(0, sh0 - 1), min(MAX_SHIFT, sh0 + 1) + 1):
                t1, t2, err, qs, ys = s1, s2, 0, [], []
                for v in blk:
                    p = (a1 * t1 + a2 * t2) >> 6
                    d = v - p
                    q = (d * 2 + ((1 << sh) >> 1)) >> sh if sh else d * 2
                    q = -8 if q < -8 else 7 if q > 7 else q
                    r = clamp16(((q * (1 << sh)) >> 1) + p)
                    err += (r - v) * (r - v)
                    if best is not None and err >= best[0]:
                        break
                    qs.append(q)
                    ys.append(r)
                    t2, t1 = t1, r
                else:
                    if best is None or err < best[0]:
                        best = (err, fi << 4 | sh, qs, ys, t1, t2)
        _, hdr, qs, ys, s1, s2 = best
        out.append(hdr)
        for j in range(0, BLOCK, 2):
            out.append((qs[j] & 15) | (qs[j + 1] & 15) << 4)
        dec += ys
    return bytes(out), dec[:len(x)], hist


def decode(data, length, loop=None, hist=None, count=None):
    """reference decoder: `count` samples, wrapping at length to loop (history hist at loop's block)"""
    out, i, s1, s2, hdr = [], 0, 0, 0, 0
    count = length if count is None else count
    while len(out) < count:
        if i == length:
            if loop is None:
                break
            i = loop & ~(BLOCK - 1)
            s1, s2 = hist
            while i < loop:                         # replay the block up to the loop start
                v, s1, s2, hdr = _dec1(data, i, s1, s2, hdr)
                i += 1
        v, s1, s2, hdr = _dec1(data, i, s1, s2, hdr)
        out.append(v)
        i += 1
    return out


def _dec1(data, i, s1, s2, hdr):
    b = (i // BLOCK) * 9
    if i % BLOCK == 0:
        hdr = data[b]
    sh, (a1, a2) = hdr & 15, FILT[hdr >> 4]
    nib = (data[b + 1 + (i % BLOCK) // 2] >> (4 * (i & 1))) & 15
    q = nib - 16 if nib >= 8 else nib
    v = clamp16(((q * (1 << sh)) >> 1) + ((a1 * s1 + a2 * s2) >> 6))
    return v, v, s1, hdr


# ------------------------------------------------------------------ SoundFont 2

def sf2_load(path):
    d = open(path, "rb").read()
    if d[:4] != b"RIFF" or d[8:12] != b"sfbk":
        sys.exit(f"{path}: not a SoundFont 2 file")

    def chunks(o, e):
        while o < e - 8:
            cid, n = d[o:o + 4], struct.unpack("<I", d[o + 4:o + 8])[0]
            yield cid, o + 8, n
            o += 8 + n + (n & 1)

    c = {}
    for _, o, n in chunks(12, len(d)):
        for cid, o2, n2 in chunks(o + 4, o + n):
            c[cid.decode()] = d[o2:o2 + n2]

    def recs(name, size, fmt):
        b = c[name]
        return [struct.unpack(fmt, b[i:i + size]) for i in range(0, len(b) - size + 1, size)]

    def zones(hdr, bag, gen, bagfield):
        out = []
        for i in range(len(hdr) - 1):
            zs = []
            for z in range(hdr[i][bagfield], hdr[i + 1][bagfield]):
                g = {}
                for op, amt in gen[bag[z][0]:bag[z + 1][0]]:
                    g[op] = (amt & 255, amt >> 8) if op in (43, 44) else amt if op in (41, 46, 53, 54, 58) \
                        else struct.unpack("<h", struct.pack("<H", amt))[0]
                zs.append(g)
            out.append((hdr[i][0].split(b"\0")[0].decode("latin1"), zs))
        return out

    phdr = recs("phdr", 38, "<20sHHHIII")
    presets = zones(phdr, recs("pbag", 4, "<HH"), recs("pgen", 4, "<HH"), 3)
    insts = zones(recs("inst", 22, "<20sH"), recs("ibag", 4, "<HH"), recs("igen", 4, "<HH"), 1)
    shdr = []
    for r in recs("shdr", 46, "<20sIIIIIBbHH")[:-1]:
        shdr.append(dict(name=r[0].split(b"\0")[0].decode("latin1"), start=r[1], end=r[2], ls=r[3], le=r[4],
                         rate=r[5], root=r[6], corr=r[7]))
    order = sorted(range(len(presets)), key=lambda i: (phdr[i][2], phdr[i][1]))   # bank, program
    return [presets[i] for i in order], insts, shdr, c["smpl"]


def multisounds(presets, insts, shdr):
    """[(name, fixed key or None, [(lo, hi, sample index, looped)])] in program order"""
    out = []
    for pname, pzones in presets:
        inst = [z[41] for z in pzones if 41 in z]
        if len(inst) != 1:
            sys.exit(f"{pname}: expected one instrument per preset")
        _, izones = insts[inst[0]]
        glob = izones[0] if izones and 53 not in izones[0] else {}
        zs, fixed = [], None
        for z in izones:
            if 53 not in z:
                continue
            lo, hi = z.get(43, glob.get(43, (0, 127)))
            mode = z.get(54, glob.get(54, 0))
            if 46 in z:
                fixed = z[46]
            zs.append((lo, hi, z[53], mode in (1, 3)))
        zs.sort()
        name = pname[7:] if pname[:3] == "M01" else pname     # "M01 00 Piano" -> "Piano"
        out.append((" ".join(name.split()).upper()[:NAME_LEN - 1], fixed, zs))
    return out


# ------------------------------------------------------------------ analysis + resampling

def hiss(np, q, dec, rate):
    """ADPCM noise against the sound, worst third octave from 1 kHz up (dB; > 0: noise above the sound)"""
    q = np.asarray(q, dtype=np.float64)
    S = np.abs(np.fft.rfft(q)) ** 2
    E = np.abs(np.fft.rfft(q - np.asarray(dec, dtype=np.float64))) ** 2
    f = np.fft.rfftfreq(len(q), 1 / rate)
    worst, lo = -99.0, 1000.0
    while lo * 2 ** (1 / 3) < rate / 2:
        m = (f >= lo) & (f < lo * 2 ** (1 / 3))
        if m.any():
            worst = max(worst, 10 * math.log10((E[m].sum() + 1e-9) / (S[m].sum() + 1e-9)))
        lo *= 2 ** (1 / 3)
    return worst


def bandwidth(np, x, rate):
    n = len(x)
    F = np.abs(np.fft.rfft(x * np.hanning(n))) ** 2
    f = np.fft.rfftfreq(n, 1 / rate)
    k = max(1, len(F) // 400)
    Fs = np.convolve(F, np.ones(k) / k, "same")
    db = 10 * np.log10(Fs / max(Fs.max(), 1e-30) + 1e-30)
    idx = np.nonzero(db > -60)[0]
    return float(f[idx[-1]]) if len(idx) else rate / 2


def resample(np, x, r, loop):
    """x (float) at ratio r (out / in): windowed sinc; looped: x[loop:] repeats forever, the output
    loop is [ls', len) with len - ls' = round(L * r) and r adjusted to that. Returns (y, ls', r)."""
    n = len(x)
    if loop is not None:
        L = n - loop
        Lo = max(1, round(L * r))
        r = Lo / L
        ls = math.ceil(loop * r)                    # output loop starts inside the periodic part
        m = ls + Lo
    else:
        ls, m = None, math.ceil(n * r)
    fc = 0.5 * min(1.0, r) * 0.94                   # cycles per input sample
    half = int(math.ceil(16 / min(1.0, r)))
    taps = np.arange(-half, half + 1)
    y = np.empty(m)
    for c0 in range(0, m, 4096):
        k = np.arange(c0, min(m, c0 + 4096))
        t = k / r
        i0 = np.floor(t).astype(np.int64)
        idx = i0[:, None] + taps[None, :]
        u = t[:, None] - idx
        w = 2 * fc * np.sinc(2 * fc * u) * np.kaiser(2 * half + 1, 8.0)[None, :]
        if loop is not None:
            src = np.where(idx < n, idx, loop + (idx - loop) % (n - loop))
        else:
            src = idx
        valid = (src >= 0) & (src < n)
        y[c0:c0 + len(k)] = (np.where(valid, x[np.clip(src, 0, n - 1)], 0.0) * w).sum(axis=1)
    return y, ls, r


# ------------------------------------------------------------------ conversion

def froot_of(h):
    """the sample's recorded pitch at its root key: the SoundFont's root key and pitch correction, or,
    for short loops (single cycles), the loop's exact period: rate x cycles / length (tune_loops)"""
    if "f_loop" in h:
        return h["f_loop"]
    return 440.0 * 2 ** ((h["root"] - 69) / 12) * 2 ** (-h["corr"] / 1200)


def tune_loops(shdr, keys, pcm):
    """short loops (<= 4096 samples) hold a whole number of cycles: take the pitch from the loop when
    it is within 30 cents of the SoundFont's (the DWGS saw is 7 cents flat by its root key). Loops of a
    fading tail (one-shots on the PSS-11 loop a few quiet samples) are left alone."""
    n = 0
    for si, looped in keys:
        h = shdr[si]
        L = h["le"] - h["ls"]
        if not looped or not 0 < L <= 4096 or "f_loop" in h:
            continue
        if abs(pcm[h["ls"]:h["le"]]).max() < 0.1 * abs(pcm[h["start"]:h["le"]]).max():
            continue
        f = froot_of(h)
        cycles = round(L * f / h["rate"])
        if cycles >= 1:
            fl = cycles * h["rate"] / L
            if abs(1200 * math.log2(fl / f)) < 30:
                h["f_loop"] = fl
                n += 1
    return n


def sf2_source(path):
    import numpy as np
    presets, insts, shdr, smpl = sf2_load(path)
    return multisounds(presets, insts, shdr), shdr, np.frombuffer(smpl, dtype="<i2").astype(np.float64)


def convert(src, budget):
    """src: (multisounds, sample headers, samples as float) from sf2_source or rom_load"""
    import numpy as np
    ms, shdr, pcm = src
    keys = sorted({(z[2], z[3]) for _, _, zs in ms for z in zs})
    print(f"{tune_loops(shdr, keys, pcm)} short loops tuned to their exact period")
    prep = {}
    for si, looped in keys:
        h = shdr[si]
        x = pcm[h["start"]:h["end"]].copy()
        loop = None
        if looped and h["start"] <= h["ls"] < h["le"] <= h["end"]:
            x = pcm[h["start"]:h["le"]].copy()
            loop = h["ls"] - h["start"]
        else:
            a = np.abs(x)
            nz = np.nonzero(a > a.max() * 10 ** (-60 / 20))[0] if a.max() > 0 else []
            if len(nz) and nz[-1] + 1 < len(x):
                x = x[:nz[-1] + 1]
                f = min(64, len(x))
                x[-f:] *= np.linspace(1, 0, f)
        bw = bandwidth(np, x, h["rate"]) if len(x) >= 2048 else h["rate"] / 2
        prep[(si, looped)] = (x, loop, bw)

    # highest key each sample is played at (its zones' tops, up to MIP_TOP; a fixed-key sound: that key)
    top = {}
    for _, fixed, zs in ms:
        for lo, hi, si, lp in zs:
            top[(si, lp)] = max(top.get((si, lp), 0), fixed if fixed is not None else min(hi, MIP_TOP))

    def levels(key, r, n, tiny):
        """mip levels (octave-down copies) so the step at the sample's top key stays <= MIP_STEP_*"""
        h = shdr[key[0]]
        st = 440 * 2 ** ((top[key] - 69) / 12) / 88200 * (h["rate"] * r / froot_of(h))
        lv, cap = 0, MIP_STEP_PCM if tiny else MIP_STEP_ADPCM
        while st > cap and n / 2 ** (lv + 1) >= 16:
            st /= 2
            lv += 1
        return lv

    def size_of(n, tiny):
        size = 2 * n if tiny else (n + BLOCK - 1) // BLOCK * 9
        return size + (size & 1)

    for fac in (math.inf, 2.4, 2.3, 2.2, 2.1, 2.0, 1.9, 1.8):     # inf: every sample at its source rate
        total, plan, mips = 0, {}, 0
        for key, (x, loop, bw) in prep.items():
            rate = shdr[key[0]]["rate"]
            tiny = len(x) < 2048
            r = 1.0 if tiny else min(1.0, max(MIN_RATE, fac * bw) / rate)
            r = 1.0 if r > 0.92 or size_of(len(x), False) <= FULL_RATE_BYTES else r
            n = len(x) if r == 1.0 else math.ceil(len(x) * r)
            lv = levels(key, r, n, tiny)
            plan[key] = (r, tiny, lv)
            total += sum(size_of(math.ceil(n / 2 ** j), tiny) for j in range(lv + 1))
            mips += sum(size_of(math.ceil(n / 2 ** j), tiny) for j in range(1, lv + 1))
        if total <= budget:
            break
        print(f"rate factor {fac}: {total} bytes (mip levels {mips}) > budget {budget}, trying lower")
    else:
        sys.exit(f"does not fit the budget {budget} even at rate factor 1.8 ({total} bytes)")

    # everything at its source rate and room left: the samples ADPCM codes worst (most hiss) as PCM16
    enc, up = {}, []
    if fac == math.inf:
        for key, (x, loop, bw) in prep.items():
            if not plan[key][1]:
                q = [int(v) for v in np.clip(np.round(x), -32768, 32767)]
                enc[key] = encode(q)
                enc[key] += (hiss(np, q, enc[key][1], shdr[key[0]]["rate"]),)
        cand = []                                   # (key, extra bytes as PCM16, dB of noise removed)
        for key in sorted(enc, key=lambda k: -enc[k][3]):
            n, lv = len(prep[key][0]), plan[key][2]
            if enc[key][3] > PCM_HISS:
                cand.append((key, sum(size_of(math.ceil(n / 2 ** j), True) - size_of(math.ceil(n / 2 ** j), False)
                                      for j in range(lv + 1)), enc[key][3] - PCM_HISS))
        G = 256                                     # 0/1 knapsack in 256-byte steps: most dB in the room left
        cap = max(0, budget - total) // G
        best = [(0.0, ())] * (cap + 1)
        for i, (_, more, v) in enumerate(cand):
            w = -(-more // G)
            for c in range(cap, w - 1, -1):
                if best[c - w][0] + v > best[c][0]:
                    best[c] = (best[c - w][0] + v, best[c - w][1] + (i,))
        for i in best[cap][1]:
            key, more, _ = cand[i]
            plan[key] = (plan[key][0], True, plan[key][2])
            total += more
            up.append(f"{shdr[key[0]]['name']} {enc[key][3]:+.0f}")
        left = [f"{shdr[k[0]]['name']} {enc[k][3]:+.0f}" for k in sorted(enc, key=lambda k: -enc[k][3])
                if not plan[k][1] and enc[k][3] > PCM_HISS]
        print(f"{len(up)} samples kept as PCM16 (ADPCM noise above the sound in a third octave, dB): "
              + ", ".join(up) + f"; {total} bytes" + ("; still ADPCM: " + ", ".join(left) if left else ""))

    rom, samples, index, snrs = bytearray(), [], {}, []
    for n_done, (key, (x, loop, bw)) in enumerate(prep.items()):
        h = shdr[key[0]]
        r0, tiny, lv = plan[key]
        index[key] = len(samples)
        for j in range(lv + 1):                     # the sample, then its octave-down copies
            rj = r0 / 2 ** j
            if rj < 1.0:
                y, ls, r = resample(np, x, rj, loop)
            else:
                y, ls, r = x, loop, 1.0
            q = [int(v) for v in np.clip(np.round(y), -32768, 32767)]
            if len(rom) & 1:
                rom.append(0)
            off = len(rom)
            k = round(h["rate"] * r / froot_of(h) * 65536)
            h1 = h2 = 0
            if tiny:
                rom += struct.pack(f"<{len(q)}h", *q)
                fmt = FMT_PCM16
            else:
                data, dec, hist = enc[key][:3] if j == 0 and key in enc else encode(q)
                rom += data
                fmt = FMT_ADPCM
                if ls is not None:
                    h1, h2 = hist[ls // BLOCK]
                if j == 0:
                    e = sum((a - b) ** 2 for a, b in zip(q, dec))
                    p = sum(a * a for a in q)
                    snrs.append((10 * math.log10(p / e) if e else 99.0, h["name"]))
                if ls is not None:                  # loop seam: wrap decodes like the first pass
                    L = len(q) - ls
                    seam = decode(data, len(q), ls, (h1, h2), len(q) + 2 * L + 32)[len(q):]
                    if seam != [dec[ls + i % L] for i in range(len(seam))]:
                        sys.exit(f"{h['name']}: loop decode mismatch")
            mip = len(samples) + 1 if j < lv else NOMIP
            samples.append((off, len(q), NOLOOP if ls is None else ls, k, h1, h2, fmt, mip, 0, 0, 0, 0, 0))
        print(f"\r  {n_done + 1}/{len(prep)} samples", end="", flush=True)
    print()
    nmips = sum(1 for smp in samples if smp[7] != NOMIP)

    zones, multis = [], []
    for name, fixed, zs in ms:
        multis.append((name, len(zones), len(zs), 0xFF if fixed is None else fixed))
        zones += [(lo, hi, index[(si, lp)]) for lo, hi, si, lp in zs]

    snrs.sort()
    adpcm = [s for s, _ in snrs]
    print(f"rate factor {fac}: {len(rom)} bytes ({len(rom) / 1048576:.2f} MB) for {len(multis)} multisounds, "
          f"{len(samples) - nmips} samples + {nmips} octave-down copies, mip levels {mips} bytes "
          f"(raw 16-bit: {2 * sum(len(p[0]) for p in prep.values()) / 1048576:.2f} MB)")
    if adpcm:
        print(f"ADPCM SNR: median {sorted(adpcm)[len(adpcm) // 2]:.1f} dB, lowest "
              + ", ".join(f"{n} {s:.1f}" for s, n in snrs[:4]))
    return rom, samples, zones, multis


# ------------------------------------------------------------------ C output

def c_bytes(b):
    rows = []
    for i in range(0, len(b), 24):
        rows.append("    " + ",".join(str(v) for v in b[i:i + 24]) + ",")
    return "\n".join(rows)


def rom_sum(data):
    """rotate-xor over little-endian words (pss.c checks the loaded file with it)"""
    h = 0x811C9DC5
    for (w,) in struct.iter_unpack("<I", data):
        h = (((h << 5) | (h >> 27)) & 0xFFFFFFFF) ^ w
    return h


def write_data(path, rom, samples, zones, multis, comment, have_rom, rom_file=False):
    """the tables + the sample data: inline (rom_file False), or the tables alone with the data as
    <dir>/pss_rom.bin (padded to whole words) and <dir>/pss_rom.inc"""
    out = [f"/* generated by gen_pss11.py - do not edit. {comment} */",
           f"#define PSS_HAVE_ROM {1 if have_rom else 0}",
           f"#define PSS_NMULTI {len(multis)}",
           f"#define PSS_ROM_BYTES {len(rom)}",
           "static const pss_multi pss_multis[PSS_NMULTI] = {"]
    out += [f'    {{ "{n}", {z}, {nz}, {fx} }},' for n, z, nz, fx in multis]
    out += ["};", f"static const pss_zone pss_zones[{len(zones)}] = {{"]
    out += [f"    {{ {lo}, {hi}, {s} }}," for lo, hi, s in zones]
    out += ["};", f"static const pss_sample pss_samples[{len(samples)}] = {{"]
    out += [f"    {{ {o}u, {n}u, {lp:#x}u, {k}u, {h1}, {h2}, {fmt}, {mip:#x}, {{ {e0:#04x}, {e1:#04x}, {e2:#04x} }}, "
            f"{{ {l0:#04x}, {l1:#04x} }} }},"
            for o, n, lp, k, h1, h2, fmt, mip, e0, e1, e2, l0, l1 in samples]
    array = [f"static const uint8_t pss_rom[{max(1, len(rom))}] __attribute__((aligned(4))) = {{",
             c_bytes(rom or b"\0"), "};", ""]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if rom_file:
        data = rom + bytes(-len(rom) % 4)
        d = os.path.dirname(path)
        out += ["};", "/* the sample data: content/pss_rom.bin (build/pss_rom.bin), loaded at runtime */",
                "#define PSS_ROM_FILE \"/yaffs2/content/pss_rom.bin\"",
                f"#define PSS_ROM_FILE_BYTES {len(data)}", f"#define PSS_ROM_SUM {rom_sum(data):#010x}u", ""]
        open(os.path.join(d, "pss_rom.bin"), "wb").write(data)
        open(os.path.join(d, "pss_rom.inc"), "w").write("\n".join(
            [f"/* generated by gen_pss11.py - do not edit. {comment} Host tests / bench only. */"] + array))
        print(f"wrote {os.path.relpath(os.path.join(d, 'pss_rom.bin'))}, pss_rom.inc")
    else:
        out += ["};"] + array
    open(path, "w").write("\n".join(out))
    print(f"wrote {os.path.relpath(path)}")


# ------------------------------------------------------------------ the GEW7 (AWM) model

# Envelope, LFO and sample formats of the PSS-11's GEW7 tone generator (YMW270-F). The behaviour,
# rate steps and LFO constants follow MAME's src/devices/sound/gew.cpp + gew7.cpp (BSD-3-Clause,
# copyright Miguel Angel Horna, Devin Acker; see NOTICE): the chip runs at 8 MHz / 72 = 111.1 kHz,
# a slot's envelope moves once per chip sample. gew_eg() below is that per-sample code, the reference
# the C core (pss_core.c, per 32 output samples, closed form) is tested against.
CHIP_RATE = 8_000_000 / 72
N_SUB = 32 * CHIP_RATE / 88200                      # chip samples per 32-sample sub-block of the OP-1 voice
EG_SHIFT = 16
EG_TOP = 0x3FF << EG_SHIFT
EG_START = (0x3FF - 0x2A0) << EG_SHIFT              # retrigger level (MAME master), about -63 dB
EG_TARGET = 0x817 << (EG_SHIFT - 1)                 # the attack's exponential target, above the top
LFO_FREQ = [0.168, 2.019, 3.196, 4.206, 5.215, 5.888, 6.224, 7.066]            # Hz
VIB_CENTS = [0.0, 3.378, 5.065, 6.750, 10.114, 20.170, 40.180, 79.307]          # depth limits
TREM_DB = [0.0, 0.4, 0.8, 1.5, 3.0, 6.0, 12.0, 24.0]


def gew_steps():
    """GEW7's attack / decay-release steps per chip sample, rate index 0..63 (gew7.cpp)"""
    att, dec = [0] * 64, [0] * 64
    for i in range(4, 64):
        att[i] = [0x99, 0xAA, 0xCC, 0x100][i % 4] << (i // 4)
        dec[i] = att[i] >> 5
    att[63] = 0x400 << EG_SHIFT
    return att, dec


def gew_rate_index(reg, rate):
    """get_rate: register 0 = no movement, 15 = instant, else 4 x reg + key scaling, 0..63"""
    return 0 if reg == 0 else 63 if reg == 15 else max(0, min(63, 4 * reg + rate))


def gew_key_rate(krs, octave, bit9):
    """envelope_generator_calc: key-rate scaling from the slot's octave (signed) and pitch bit 9"""
    return 0 if krs == 15 else (octave + krs) * 2 + bit9


def gew_eg(eg, octave, bit9, times):
    """MAME's envelope, one chip sample at a time (gew.cpp envelope_generator_update, master):
    eg = table bytes 9..11; returns the level index (volume >> 16, 0..1023) after each chip-sample
    count in times (ascending), and the chip sample the attack reached the top (None: not yet).
    Linear stages jump exactly (same integers as sample by sample)."""
    att, dec = gew_steps()
    rate = gew_key_rate(eg[2] >> 4, octave, bit9)
    ar = att[gew_rate_index(eg[0] >> 4, rate)]
    d1 = dec[gew_rate_index(eg[0] & 15, rate)]
    d2 = dec[gew_rate_index(eg[1] >> 4, rate)]
    dl = 15 - (eg[2] & 15)
    v, st, t, out, t_top = EG_START, 0, 0, [], None
    for until in times:
        while t < until:
            if st == 0:                             # attack: exponential towards EG_TARGET
                v += ((EG_TARGET - v) * ar) >> 24
                t += 1
                if v >= EG_TOP:
                    v, st, t_top = EG_TOP, 1, t
            elif st == 1:                           # decay 1 (linear), then the decay level is checked
                th = (dl + 1) << (EG_SHIFT + 6)     # v >> 22 <= dl  <=>  v < th
                n = (v - th) // d1 + 1 if d1 and v >= th else 1
                if d1 == 0 and v >= th:
                    t = until
                    break
                if n > until - t:
                    v = max(0, v - (until - t) * d1)
                    t = until
                    break
                v = max(0, v - n * d1)
                t += n
                st = 2
            else:                                   # decay 2 (linear) to 0
                n = until - t
                v = max(0, v - n * d2)
                t = until
        out.append(v >> EG_SHIFT)
    return out, t_top


def gew_decode(data, fmt, n):
    """n samples of a GEW sample (gew.cpp): 8-bit signed, or 12-bit, two in three bytes"""
    out = []
    for i in range(n):
        if fmt == FMT_GEW12:
            a = (i >> 1) * 3
            v = (data[a + 2] << 8 | (data[a + 1] & 0xF0)) if i & 1 else (data[a] << 8 | (data[a + 1] & 0xF) << 4)
        else:
            v = data[i] << 8
        out.append(v - 65536 if v >= 32768 else v)
    return out


def gew_bytes(fmt, n):
    """bytes the chip reads for samples 0 .. n - 1"""
    return (n + 1) // 2 * 3 if fmt == FMT_GEW12 else n


# ------------------------------------------------------------------ static files (MIT)

def write_tables():
    SR, BLK = 88200, 128
    cut = [round(math.tan(math.pi * (30 * (16000 / 30) ** (k / 64)) / SR) * 32768) for k in range(65)]
    semi = [round(2 ** (s / 12) * 65536) for s in range(12)]
    dec = [min(32767, round(math.exp(-BLK / SR / (0.005 * 400 ** (k / 31))) * 32768)) for k in range(32)]
    dec.append(dec[-1])
    # A DECAY (an amount: more = faster) 1..63: level time constant 8 s .. 50 ms; B FADE 1..63:
    # 10 ms .. 4 s (per 128-sample block)
    adec = [32768] + [min(32767, round(math.exp(-BLK / SR / (8.0 / 160 ** ((k - 1) / 62))) * 32768))
                      for k in range(1, 64)]
    bfad = [32768] + [max(1, round((1 - math.exp(-BLK / SR / (0.01 * 400 ** ((k - 1) / 62)))) * 32768))
                      for k in range(1, 64)]

    def arr(t, name, ctype, vals, per=11):
        lines = [f"static const {ctype} {name}[{len(vals)}] = {{"]
        lines += ["    " + ", ".join(str(v) for v in vals[i:i + per]) + "," for i in range(0, len(vals), per)]
        return [f"/* {t} */"] + lines + ["};", ""]

    out = ["/* generated by gen_pss11.py --static - do not edit */", "#ifndef PSS_TABLES_H", "#define PSS_TABLES_H",
           "", "#include <stdint.h>", ""]
    out += arr("SVF g = tan(pi fc / 88200), Q15, CUTOFF in 64 steps (+ guard): 30 Hz .. 16 kHz", "pss_cut",
               "uint16_t", cut)
    out += arr("2^(s / 12), Q16, s = 0 .. 11", "pss_semi", "uint32_t", semi, 6)
    out += arr("per-block filter envelope decay, Q15, F.DECAY in 32 steps: 5 ms .. 2 s", "pss_fdecay", "uint16_t",
               dec)
    out += arr("per-block osc A level decay, Q15 (0 = hold), A DECAY 1..63: 8 s .. 50 ms", "pss_adecay",
               "uint16_t", adec)
    out += arr("per-block osc B fade-in step towards full, Q15 (0 = instant), B FADE 1..63: 10 ms .. 4 s",
               "pss_bfade", "uint16_t", bfad)
    att, dec = gew_steps()
    keep = [min(1 << 30, round(max(0.0, 1 - a / 2 ** 24) ** N_SUB * (1 << 30))) for a in att]
    keep[63] = 0
    out += arr("GEW7 attack per 32 output samples (%.4f chip samples): what is left of the distance to the "
               "target, Q30 (gew_steps)" % N_SUB, "pss_eg_keep", "uint32_t", keep, 6)
    out += arr("GEW7 decay / release per 32 output samples, level units << 16", "pss_eg_dec", "uint32_t",
               [round(d * N_SUB) for d in dec], 6)
    out += arr("GEW7 decay / release per chip sample (gew7.cpp), level units << 16", "pss_eg_dec1", "uint32_t",
               dec, 6)
    out += arr("GEW7 level index 0..1023 (96 dB) -> gain, Q16", "pss_eg_exp", "uint16_t",
               [min(65535, round(10 ** (-(96 - 96 * i / 1024) / 20) * 65536)) for i in range(1024)], 12)
    out += arr("GEW7 LFO phase step per 32 output samples (phase: 16 bits, 256 table steps)", "pss_lfo_inc",
               "uint32_t", [round(int(256 * f * 256 / CHIP_RATE) * N_SUB) for f in LFO_FREQ], 8)
    out += arr("GEW7 vibrato depth limits, cents x 1000", "pss_vib_mc", "uint32_t",
               [round(c * 1000) for c in VIB_CENTS], 8)
    out += arr("GEW7 tremolo depth limits, level-index steps (96 / 1024 dB) x 16", "pss_trem_q4", "uint16_t",
               [round(d * 1024 / 96 * 16) for d in TREM_DB], 8)
    out += ["#endif", ""]
    open(os.path.join(HERE, "pss_tables.h"), "w").write("\n".join(out))


def builtin():
    """8 own single-cycle waves (256 samples at 44.1 kHz, root = 44100 / 256 Hz), PCM16"""
    N = 256

    def additive(amps):
        t = [2 * math.pi * i / N for i in range(N)]
        v = [sum(a * math.sin((h + 1) * x) for h, a in enumerate(amps)) for x in t]
        m = max(abs(s) for s in v)
        return [round(s / m * 26000) for s in v]

    H = 100
    waves = [
        ("SAW", additive([1 / h for h in range(1, H)])),
        ("SQUARE", additive([1 / h if h % 2 else 0 for h in range(1, H)])),
        ("PULSE 25%", additive([math.sin(math.pi * h / 4) / h for h in range(1, H)])),
        ("TRIANGLE", additive([(1 / h ** 2) * (1 if h % 4 == 1 else -1) if h % 2 else 0 for h in range(1, H)])),
        ("SINE", additive([1])),
        ("ORGAN", additive([1, 0.8, 0.6, 0.5, 0, 0.3, 0, 0.25])),
        ("E.PIANO", additive([1, 0.12, 0.25, 0, 0.06])),
        ("VOICE", additive([0.6, 1, 0.5, 0.2, 0.35, 0.3, 0.1, 0.05, 0.08])),
    ]
    # AWM envelope per wave (GEW7 table bytes 9..11; 0 = none): E.PIANO decays like a struck sound
    egs = {"E.PIANO": (0xF5, 0x30, 0x24)}
    import numpy as np
    rom, samples, zones, multis = bytearray(), [], [], []
    for i, (name, w) in enumerate(waves):       # + octave-down copies 128 .. 16 (step <= 1 up to C8)
        zones.append((0, 127, len(samples)))
        multis.append((name, len(zones) - 1, 1, 0xFF))
        for j in range(5):
            y, _, _ = resample(np, np.array(w, dtype=float), 1 / 2 ** j, 0) if j else (w, 0, 1)
            q = [int(v) for v in np.clip(np.round(y), -32768, 32767)]
            off = len(rom)
            rom += struct.pack(f"<{len(q)}h", *q)
            samples.append((off, len(q), 0, len(q) * 65536, 0, 0, FMT_PCM16, len(samples) + 1 if j < 4 else NOMIP)
                           + egs.get(name, (0, 0, 0)) + (0, 0))
    return rom, samples, zones, multis


def write_testvec():
    """deterministic signals -> encoded bytes + the reference decode (with two loop wraps)"""
    import random
    rnd = random.Random(1)
    sig = []
    for i in range(1000):                           # decaying two-tone chirp with a little noise
        t = i / 22050
        sig.append(clamp16(round(20000 * math.exp(-i / 700) * math.sin(2 * math.pi * (200 + 3000 * t) * t * 30)
                                 + 6000 * math.sin(i * 0.05) + rnd.uniform(-300, 300))))
    data, dec, hist = encode(sig)
    loop = 613                                      # not on a block boundary
    ref = decode(data, len(sig), loop, hist[loop // BLOCK], len(sig) + 2 * (len(sig) - loop) + 50)
    out = ["/* generated by gen_pss11.py --static - do not edit: decoder test vectors */",
           f"#define TV_LEN {len(sig)}", f"#define TV_LOOP {loop}", f"#define TV_H1 {hist[loop // BLOCK][0]}",
           f"#define TV_H2 {hist[loop // BLOCK][1]}", f"#define TV_REF_LEN {len(ref)}",
           f"static const uint8_t tv_data[{len(data)}] = {{", c_bytes(data), "};",
           f"static const int16_t tv_ref[{len(ref)}] = {{"]
    out += ["    " + ",".join(str(v) for v in ref[i:i + 16]) + "," for i in range(0, len(ref), 16)]
    out += ["};", ""]
    out += gew_testvec()
    open(os.path.join(HERE, "pss_testvec.h"), "w").write("\n".join(out))


def gew_octave(st):
    """the chip's octave (signed) and pitch bit 9 for a step st (16.16 source samples per 88.2 kHz
    output sample), in floating point (pss_core.c does it in fixed point)"""
    y = st / 65536 * 2 * 88200 / CHIP_RATE
    e = math.floor(math.log2(y))
    bit9 = 1 if y / 2 ** e >= 1.5 else 0
    e = max(-8, min(7, e))
    o = e + 1
    return (o - 16 if o > 7 else o), bit9


EG_CHECKS = [1, 2, 3, 4, 6, 8, 11, 16, 23, 32, 45, 64, 91, 128, 181, 256, 362, 512, 724, 1024, 1448, 2048,
             2896, 4096, 5793, 8192, 11585, 16384, 23170, 32768, 46341, 65536, 92682, 131072]


def gew_testvec():
    """GEW7 envelope windows (gew_eg, chip sample by chip sample) and GEW sample decoding"""
    import random
    rnd = random.Random(7)
    st1 = 52000                                     # octave 1, bit 9 = 0 (y = 1.26)
    cfg = []
    cfg += [((ar << 4), 0x00, 0xF0, st1) for ar in range(1, 16)]                 # attack, then hold
    cfg += [(0xF0 | d1, 0x00, 0xF8, st1) for d1 in range(1, 15)]               # decay 1 to DL 8
    cfg += [(0xFF, d2 << 4, 0xF4, st1) for d2 in range(1, 15)]                 # decay 2 below DL 4
    cfg += [(0xF8, 0x20, 0xF0 | dl, st1) for dl in range(16)]                  # decay levels
    cfg += [(0xA9, 0x50, (krs << 4) | 6, st) for krs in (0, 2, 5, 9, 14)
            for st in (3300, 13000, 40000, 80000, 300000)]                      # key scaling by octave
    cfg += [(0xF5, 0x30, 0x24, 26000), (0xC4, 0x31, 0x77, 700000), (0x00, 0x00, 0x00, st1)]
    rows = []
    for e0, e1, e2, st in cfg:
        o, b9 = gew_octave(st)
        times = []
        for k in EG_CHECKS:
            times += [round((k - 1) * N_SUB), round(k * N_SUB), round((k + 1) * N_SUB)]
        ref, t_top = gew_eg((e0, e1, e2), o, b9, sorted(set(times)))
        at = dict(zip(sorted(set(times)), ref))
        lohi = []
        for k in EG_CHECKS:
            w = [at[round((k - 1) * N_SUB)], at[round(k * N_SUB)], at[round((k + 1) * N_SUB)]]
            if t_top is not None and (k - 1) * N_SUB <= t_top <= (k + 1) * N_SUB:
                w.append(1023)                      # the attack's peak lies in between
            lohi += [max(0, min(w) - 2), min(1023, max(w) + 2)]
        rows.append(f"    {{ {{ {e0:#04x}, {e1:#04x}, {e2:#04x} }}, {st}u, {{ " + ", ".join(map(str, lohi)) + " } },")
    # decoding: 8- and 12-bit, looped, the 12-bit one starting reversed
    data = bytes(rnd.randrange(256) for _ in range(240))
    dec_rows = []
    for fmt, n, loop, rev in ((FMT_GEW8, 200, 61, 0), (FMT_GEW12, 157, 40, 1)):
        x = gew_decode(data, fmt, n)
        seq, i, r = [], 0, rev
        for _ in range(500):
            if i >= n:
                i, r = loop, 0
            seq.append(x[n - 1 - i] if r else x[i])
            i += 1
        dec_rows.append((fmt, n, loop, rev, seq))
    out = ["/* GEW7 envelope (gen_pss11.py gew_eg: MAME's code, per chip sample): after k = TV_EG_CHECKS[j]",
           " * steps of 32 output samples the level index lies in [lo, hi] (the chip's at k - 1 .. k + 1, +-2) */",
           f"#define TV_EG_N {len(cfg)}", f"#define TV_EG_K {len(EG_CHECKS)}",
           f"static const uint32_t tv_eg_checks[TV_EG_K] = {{ {', '.join(map(str, EG_CHECKS))} }};",
           "static const struct { uint8_t eg[3]; uint32_t st; uint16_t lohi[2 * TV_EG_K]; } tv_eg[TV_EG_N] = {"]
    out += rows + ["};", "/* GEW sample decoding (gew.cpp): 500 reads through end -> loop wraps */",
                   f"static const uint8_t tv_gew_data[{len(data)}] = {{", c_bytes(data), "};",
                   "static const struct { uint8_t fmt; uint16_t n, loop; uint8_t rev; int16_t seq[500]; } tv_gew[2] = {"]
    for fmt, n, loop, rev, seq in dec_rows:
        out.append(f"    {{ {fmt}, {n}, {loop}, {rev}, {{ " + ",".join(map(str, seq)) + " } },")
    out += ["};", ""]
    return out


# ------------------------------------------------------------------ PSS-11 ROM extraction

# The GEW7 sample table (gew7.cpp init_sample): 512 entries of 12 bytes at ROM offset 0x4000.
#   0..2   start: 21-bit address, format in bits 21..23 (bit 22 = 12-bit, two samples in 3 bytes;
#          else signed 8-bit)
#   3..4   loop: sample index (from the start) the chip jumps back to at the end
#   5..6   end: 0x4000 - this = the number of samples; the chip always loops (end -> loop)
#   7      vibrato (bits 0..2) | tremolo (3..5) | reverse (7: backwards until the first loop)
#   8      LFO speed (bits 0..2)
#   9..11  attack | decay 1, decay 2 | release, key scaling | decay level
# Which entries (and keys, octaves, levels) each of the 100 voices uses is in the firmware, not in this
# table: every entry becomes a multisound of its own (S000 .. S511, by table index).
PSS_TABLE = 0x4000
PSS_ENTRIES = 512


def gew_period(x, loop, n):
    """samples per cycle of the looped part (an AWM loop holds whole cycles: L / k for some k), or
    None when it is not periodic (noise, long loops, silence)"""
    import numpy as np
    L = n - loop
    if L < 8 or L > 8192:
        return None
    seg = np.asarray(x[loop:n], dtype=np.float64)
    seg = seg - seg.mean()
    e = float(np.dot(seg, seg))
    if e < 1e-3 * L * 256 * 256:                   # (near) silent loop
        return None
    idx = np.arange(L, dtype=np.float64)
    tiled = np.concatenate([seg, seg[:1]])
    best = None
    for k in range(2 if L > 1024 else 1, L // 8 + 1):     # a long loop is a period only with whole cycles in it                  # the shortest period L / k that repeats the loop
        pos = (idx + L / k) % L                     # the loop shifted by L / k, interpolated
        j = pos.astype(int)
        sh = tiled[j] + (tiled[j + 1] - tiled[j]) * (pos - j)
        if float(np.dot(seg, sh)) / e > 0.9:
            best = L / k
    return best


def pss_rom_source(path, rate):
    """the GEW7 sample table -> (rom bytes, samples, zones, multis) in the chip's own formats"""
    d = open(path, "rb").read()
    if len(d) < PSS_TABLE + PSS_ENTRIES * 12:
        sys.exit(f"{path}: too small for a GEW7 ROM ({len(d)} bytes)")
    rom, samples, zones, multis = bytearray(), [], [], []
    data_at, sound_at = {}, {}
    stats = {"entries": 0, "bad": 0, "dup": 0, "pitched": 0, "silent_attack": 0, "fmt12": 0, "dc": 0}
    for i in range(PSS_ENTRIES):
        e = d[PSS_TABLE + i * 12:PSS_TABLE + i * 12 + 12]
        raw = e[0] << 16 | e[1] << 8 | e[2]
        fmt = FMT_GEW12 if (raw >> 20) & 4 else FMT_GEW8
        st = raw & 0x1FFFFF
        loop = e[3] << 8 | e[4]
        n = 0x4000 - (e[5] << 8 | e[6])
        nb = gew_bytes(fmt, n)
        if n <= 1 or n > 0x4000 or loop >= n or st >= len(d) or st + nb > len(d) or e[:3] == b"\xff\xff\xff":
            stats["bad"] += 1
            continue
        stats["entries"] += 1
        key = (st, fmt, n, loop) + tuple(e[7:12])
        if key in sound_at:                         # the same sound twice in the table
            stats["dup"] += 1
            continue
        if (st, fmt, n) not in data_at:
            data_at[(st, fmt, n)] = len(rom)
            rom += d[st:st + nb]
        x = gew_decode(d[st:st + nb], fmt, n)
        mean = sum(x) / n
        if (sum((v - mean) ** 2 for v in x) / n) ** 0.5 < 64:     # constant: the idle DC-offset filler
            stats["dc"] += 1
            continue
        per = gew_period(x, loop, n)
        stats["pitched"] += per is not None
        stats["silent_attack"] += (e[9] >> 4) == 0
        stats["fmt12"] += fmt == FMT_GEW12
        k = round((per if per else rate / 261.6256) * 65536)       # unpitched: C4 at --rate
        sound_at[key] = len(samples)
        zones.append((0, 127, len(samples)))
        multis.append((f"S{i:03d}", len(zones) - 1, 1, 0xFF))
        samples.append((data_at[(st, fmt, n)], n, loop, k, 0, 0, fmt, NOMIP, e[9], e[10], e[11], e[7], e[8]))
    if not samples:
        sys.exit(f"{path}: no samples found in the GEW7 sample table")
    print(f"{stats['entries']} table entries ({stats['bad']} empty / invalid, {stats['dup']} repeated, "
          f"{stats['dc']} silent (constant)): "
          f"{len(samples)} sounds, {stats['fmt12']} 12-bit, {stats['pitched']} with a periodic loop (tuned to it, "
          f"the rest: C4 at {rate:.0f} Hz), {stats['silent_attack']} with attack 0 (stay at -63 dB, as on the chip); "
          f"{len(rom)} bytes of sample data")
    return bytes(rom), samples, zones, multis


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sf2", help="your sample set (.sf2)")
    ap.add_argument("--budget", type=int, default=1_500_000, help="max bytes of sample data (default 1500000)")
    ap.add_argument("--static", action="store_true", help="write the committed tables / built-in / test files")
    ap.add_argument("--pss-rom", help="the PSS-11 ROM dump (xl437a00.ic4, 256 KB)")
    ap.add_argument("--rate", type=float, default=32000.0,
                    help="--pss-rom: the rate unpitched samples (drums, noise) play at for C4 (default 32000)")
    ap.add_argument("--out", help="--pss-rom / --sf2: directory for pss_data.inc / pss_rom.bin / .inc "
                    "(default features/party/build)")
    a = ap.parse_args()
    if a.static:
        write_tables()
        rom, samples, zones, multis = builtin()
        write_data(os.path.join(HERE, "pss_builtin.inc"), rom, samples, zones, multis,
                   "Built-in waves (own, MIT): used without sample data and in the benches.", False)
        write_testvec()
        print("wrote pss_tables.h, pss_testvec.h")
    if a.sf2 or a.pss_rom:
        if a.pss_rom:
            rom, samples, zones, multis = pss_rom_source(a.pss_rom, a.rate)
            what = os.path.basename(a.pss_rom)
        else:
            rom, samples, zones, multis = convert(sf2_source(a.sf2), a.budget)
            what = os.path.basename(a.sf2)
        write_data(os.path.join(a.out or os.path.join(HERE, "build"), "pss_data.inc"), rom, samples, zones, multis,
                   f"From {what}: not MIT (see NOTICE), never packed.", True, rom_file=True)
    if not a.static and not a.sf2 and not a.pss_rom:
        ap.print_help()


if __name__ == "__main__":
    main()
