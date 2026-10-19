#!/usr/bin/env python3
"""STATION 1 engine data generator.

    gen_m1.py --sf2 "<path>/<sample set>.sf2" [--budget 1500000]
        -> the 100 multisounds of your own sample set (.sf2), converted for the OP-1 (the samples
           are not ours: build/ is committed with a NOTICE, not MIT, and never packed):
           build/m1_data.inc  the tables (compiled in)
           build/m1_rom.bin   the sample data: a content file of the update (content/m1_rom.bin),
                              loaded at runtime into TE's sample memory (m1.c)
           build/m1_rom.inc   the same data as a C array, for the host tests and the SoundFont bench
                              (compiled with -DM1_ROM_INLINE)
    gen_m1.py --static
        -> m1_tables.h (filter / pitch / envelope tables), m1_builtin.inc (the built-in waves used
           when there is no SoundFont, and in the simulator benches), m1_testvec.h (decoder test
           vectors for test_m1_core.c). Own data, MIT; committed.

Conversion, per sample (spec: README.md "Data"):
  1. cut at the loop end (looped) or at the -60 dB tail (one-shots, 64-sample fade);
  2. short loops (single cycles) are tuned to their exact period (loop length, whole cycles);
     measure the real bandwidth (smoothed spectrum above -60 dB) and resample to 2.4 x that
     (>= 16 kHz, <= the source rate; samples of <= 8 KB as ADPCM keep the source rate). Loops stay
     exactly periodic: the loop gets an integer length L' and the rate follows (rate x L' / L), so
     pitch is unchanged;
  3. store 4-bit block ADPCM (16 samples in 9 bytes: shift + predictor header, 8 bytes of nibbles;
     decoder in m1_core.c) or PCM16 for tiny samples (single cycles, < 2048 samples).
If the total exceeds --budget, the rate factor drops in steps (2.4 -> 1.8) until it fits.
Needs numpy. Prints a size / quality report."""

import argparse, math, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
FMT_ADPCM, FMT_PCM16 = 0, 1
NOLOOP = 0xFFFFFFFF
NOMIP = 0xFFFF
# octave-down copies, up to C8: single cycles / tiny samples (PCM16, rich in harmonics) to a step <= 1
# (no folding above 44.1 kHz), ADPCM to <= 4 (m1_core.c's cap; chosen at <= 2 where there is one)
MIP_TOP, MIP_STEP_PCM, MIP_STEP_ADPCM = 108, 1.0, 4.0
BLOCK = 16
FILT = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60), (30, 0), (90, -24), (126, -63)]   # /64
MAX_SHIFT = 13
# lowest stored rate, and samples this small (as ADPCM at the source rate) keep the source rate: lower
# rates put the ADPCM noise and linear interpolation images into the audible band (organ 2 at 8 kHz:
# +8 dB at 5-10 kHz, +20 dB at 10-16 kHz)
MIN_RATE, FULL_RATE_BYTES = 16000.0, 8192
NAME_LEN = 12


# ------------------------------------------------------------------ codec (bit-exact with m1_core.c)

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


def tune_loops(shdr, keys):
    """short loops (<= 4096 samples) hold a whole number of cycles: take the pitch from the loop when
    it is within 30 cents of the SoundFont's (the DWGS saw is 7 cents flat by its root key)"""
    n = 0
    for si, looped in keys:
        h = shdr[si]
        L = h["le"] - h["ls"]
        if not looped or not 0 < L <= 4096 or "f_loop" in h:
            continue
        f = froot_of(h)
        cycles = round(L * f / h["rate"])
        if cycles >= 1:
            fl = cycles * h["rate"] / L
            if abs(1200 * math.log2(fl / f)) < 30:
                h["f_loop"] = fl
                n += 1
    return n


def convert(path, budget):
    import numpy as np
    presets, insts, shdr, smpl = sf2_load(path)
    pcm = np.frombuffer(smpl, dtype="<i2").astype(np.float64)
    ms = multisounds(presets, insts, shdr)
    keys = sorted({(z[2], z[3]) for _, _, zs in ms for z in zs})
    print(f"{tune_loops(shdr, keys)} short loops tuned to their exact period")
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

    for fac in (2.4, 2.3, 2.2, 2.1, 2.0, 1.9, 1.8):
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
                data, dec, hist = encode(q)
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
            samples.append((off, len(q), NOLOOP if ls is None else ls, k, h1, h2, fmt, mip))
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
          f"(raw 16-bit: {len(smpl) / 1048576:.2f} MB)")
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
    """rotate-xor over little-endian words (m1.c checks the loaded file with it)"""
    h = 0x811C9DC5
    for (w,) in struct.iter_unpack("<I", data):
        h = (((h << 5) | (h >> 27)) & 0xFFFFFFFF) ^ w
    return h


def write_data(path, rom, samples, zones, multis, comment, have_rom, rom_file=False):
    """the tables + the sample data: inline (rom_file False), or the tables alone with the data as
    <dir>/m1_rom.bin (padded to whole words) and <dir>/m1_rom.inc"""
    out = [f"/* generated by gen_m1.py - do not edit. {comment} */",
           f"#define M1_HAVE_ROM {1 if have_rom else 0}",
           f"#define M1_NMULTI {len(multis)}",
           f"#define M1_ROM_BYTES {len(rom)}",
           "static const m1_multi m1_multis[M1_NMULTI] = {"]
    out += [f'    {{ "{n}", {z}, {nz}, {fx} }},' for n, z, nz, fx in multis]
    out += ["};", f"static const m1_zone m1_zones[{len(zones)}] = {{"]
    out += [f"    {{ {lo}, {hi}, {s} }}," for lo, hi, s in zones]
    out += ["};", f"static const m1_sample m1_samples[{len(samples)}] = {{"]
    out += [f"    {{ {o}u, {n}u, {lp:#x}u, {k}u, {h1}, {h2}, {fmt}, {mip:#x} }},"
            for o, n, lp, k, h1, h2, fmt, mip in samples]
    array = [f"static const uint8_t m1_rom[{max(1, len(rom))}] __attribute__((aligned(4))) = {{",
             c_bytes(rom or b"\0"), "};", ""]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if rom_file:
        data = rom + bytes(-len(rom) % 4)
        d = os.path.dirname(path)
        out += ["};", "/* the sample data: content/m1_rom.bin (build/m1_rom.bin), loaded at runtime */",
                "#define M1_ROM_FILE \"/yaffs2/content/m1_rom.bin\"",
                f"#define M1_ROM_FILE_BYTES {len(data)}", f"#define M1_ROM_SUM {rom_sum(data):#010x}u", ""]
        open(os.path.join(d, "m1_rom.bin"), "wb").write(data)
        open(os.path.join(d, "m1_rom.inc"), "w").write("\n".join(
            [f"/* generated by gen_m1.py - do not edit. {comment} Host tests / bench only. */"] + array))
        print(f"wrote {os.path.relpath(os.path.join(d, 'm1_rom.bin'))}, m1_rom.inc")
    else:
        out += ["};"] + array
    open(path, "w").write("\n".join(out))
    print(f"wrote {os.path.relpath(path)}")


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

    out = ["/* generated by gen_m1.py --static - do not edit */", "#ifndef M1_TABLES_H", "#define M1_TABLES_H",
           "", "#include <stdint.h>", ""]
    out += arr("SVF g = tan(pi fc / 88200), Q15, CUTOFF in 64 steps (+ guard): 30 Hz .. 16 kHz", "m1_cut",
               "uint16_t", cut)
    out += arr("2^(s / 12), Q16, s = 0 .. 11", "m1_semi", "uint32_t", semi, 6)
    out += arr("per-block filter envelope decay, Q15, F.DECAY in 32 steps: 5 ms .. 2 s", "m1_fdecay", "uint16_t",
               dec)
    out += arr("per-block osc A level decay, Q15 (0 = hold), A DECAY 1..63: 8 s .. 50 ms", "m1_adecay",
               "uint16_t", adec)
    out += arr("per-block osc B fade-in step towards full, Q15 (0 = instant), B FADE 1..63: 10 ms .. 4 s",
               "m1_bfade", "uint16_t", bfad)
    out += ["#endif", ""]
    open(os.path.join(HERE, "m1_tables.h"), "w").write("\n".join(out))


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
            samples.append((off, len(q), 0, len(q) * 65536, 0, 0, FMT_PCM16, len(samples) + 1 if j < 4 else NOMIP))
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
    out = ["/* generated by gen_m1.py --static - do not edit: decoder test vectors */",
           f"#define TV_LEN {len(sig)}", f"#define TV_LOOP {loop}", f"#define TV_H1 {hist[loop // BLOCK][0]}",
           f"#define TV_H2 {hist[loop // BLOCK][1]}", f"#define TV_REF_LEN {len(ref)}",
           f"static const uint8_t tv_data[{len(data)}] = {{", c_bytes(data), "};",
           f"static const int16_t tv_ref[{len(ref)}] = {{"]
    out += ["    " + ",".join(str(v) for v in ref[i:i + 16]) + "," for i in range(0, len(ref), 16)]
    out += ["};", ""]
    open(os.path.join(HERE, "m1_testvec.h"), "w").write("\n".join(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sf2", help="your sample set (.sf2)")
    ap.add_argument("--budget", type=int, default=1_500_000, help="max bytes of sample data (default 1500000)")
    ap.add_argument("--static", action="store_true", help="write the committed tables / built-in / test files")
    a = ap.parse_args()
    if a.static:
        write_tables()
        rom, samples, zones, multis = builtin()
        write_data(os.path.join(HERE, "m1_builtin.inc"), rom, samples, zones, multis,
                   "Built-in waves (own, MIT): used without a SoundFont and in the benches.", False)
        write_testvec()
        print("wrote m1_tables.h, m1_testvec.h")
    if a.sf2:
        rom, samples, zones, multis = convert(a.sf2, a.budget)
        write_data(os.path.join(HERE, "build", "m1_data.inc"), rom, samples, zones, multis,
                   f"From {os.path.basename(a.sf2)}: not MIT (see NOTICE), never packed.", True, rom_file=True)
    if not a.static and not a.sf2:
        ap.print_help()


if __name__ == "__main__":
    main()
