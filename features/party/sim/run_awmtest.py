#!/usr/bin/env python3
"""AWM path end to end, without Yamaha's data: a synthetic GEW7 ROM (sample table at 0x4000, the chip's
8- and 12-bit formats, loops, envelope and LFO bytes) -> gen_pss11.py --pss-rom -> pss_core.c.

    run_awmtest.py

Checks: the extractor reads the table as gew7.cpp does (start / format / loop / end = 0x4000 - n),
skips invalid and repeated entries, keeps the sample bytes as they are (decode = the source), tunes
periodic loops to their period (L / k); the C voice (sim/awm_host.c, host cc) plays them at pitch
(A4, C3), with the chip's vibrato (depth 7: +-79.3 cents, speed 7: 6.78 Hz as MAME steps it) and tremolo (depth 7:
24 dB); then the same data on the Blackfin model: cycles per voice block (run_psstest.py rom_bench).
Exit 0 = pass."""

import math, os, random, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
FEAT = os.path.normpath(os.path.join(HERE, ".."))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(FEAT, "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "awmtest")
sys.path.insert(0, FEAT)
import gen_pss11 as g  # noqa: E402

fails = []


def check(ok, what):
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok:
        fails.append(what)


def pack8(x):
    return bytes((v >> 8) & 0xFF for v in x)


def pack12(x):
    """two 12-bit samples in 3 bytes, as gew.cpp reads them (ab.c / ..C. AB)"""
    x = list(x) + [0] * (len(x) & 1)
    out = bytearray()
    for a, c in zip(x[0::2], x[1::2]):
        a &= 0xFFFF
        c &= 0xFFFF
        out += bytes([a >> 8, ((c >> 4) & 0xF) << 4 | ((a >> 4) & 0xF), c >> 8])
    return bytes(out)


def q(v, bits):
    v = max(-32768, min(32767, int(round(v))))
    return v & ~((1 << (16 - bits)) - 1)


def wave(period, n, bits, amp=20000):
    return [q(amp * (0.8 * math.sin(2 * math.pi * i / period) + 0.2 * math.sin(4 * math.pi * i / period)), bits)
            for i in range(n)]


def synthetic_rom():
    rnd = random.Random(3)
    rom = bytearray(b"\xff" * 0x40000)
    at = [0x8000]
    sounds = []

    def put(i, fmt, x, loop, eg, lfo):
        data = pack12(x) if fmt == g.FMT_GEW12 else pack8(x)
        a = at[0]
        rom[a:a + len(data)] = data
        at[0] += len(data) + 5
        raw = a | (0x400000 if fmt == g.FMT_GEW12 else 0)
        n = len(x)
        rom[0x4000 + i * 12:0x4000 + i * 12 + 12] = bytes([raw >> 16, raw >> 8 & 0xFF, raw & 0xFF, loop >> 8,
                                                         loop & 0xFF, (0x4000 - n) >> 8, (0x4000 - n) & 0xFF,
                                                         lfo[0], lfo[1], eg[0], eg[1], eg[2]])
        sounds.append((i, fmt, x, loop, eg, lfo, a))

    hold = (0xF0, 0x00, 0xF0)
    # 0: 8-bit, a noise attack, then 4 cycles of period 50 looped
    x0 = [q(rnd.uniform(-9000, 9000) * (1 - i / 120), 8) for i in range(120)] + wave(50, 200, 8)
    put(0, g.FMT_GEW8, x0, 120, hold, (0, 0))
    # 1: 12-bit, 3 cycles of period 125 / 3 looped after 64 samples; vibrato 7, LFO speed 7
    put(1, g.FMT_GEW12, wave(125 / 3, 64 + 125, 12), 64, hold, (7, 7))
    # 2: 12-bit, period 32 (4 cycles looped after 100), reversed first pass, tremolo 7, LFO speed 3
    put(2, g.FMT_GEW12, wave(32, 228, 12), 100, hold, (0x80 | 7 << 3, 3))
    # 3: 8-bit noise, 2000 samples looped whole: unpitched
    put(3, g.FMT_GEW8, [q(rnd.uniform(-20000, 20000), 8) for _ in range(2000)], 0, hold, (0, 0))
    # 4: entry 0 again (a repeated entry); 5: start past the ROM (invalid)
    rom[0x4000 + 4 * 12:0x4000 + 5 * 12] = rom[0x4000:0x4000 + 12]
    rom[0x4000 + 5 * 12:0x4000 + 6 * 12] = bytes([0x1F, 0xFF, 0x00, 0, 0, 0x3F, 0x00, 0, 0, 0xF0, 0, 0xF0])
    return bytes(rom), sounds


def main():
    os.makedirs(OUT, exist_ok=True)
    rom, sounds = synthetic_rom()
    path = os.path.join(OUT, "synthetic_gew7.bin")
    open(path, "wb").write(rom)

    # 1. the extractor
    data, samples, zones, multis = g.pss_rom_source(path, 32000)
    check(len(samples) == 4 and [m[0] for m in multis] == ["S000", "S001", "S002", "S003"],
          f"4 sounds from 6 entries (repeated + invalid skipped): {[m[0] for m in multis]}")
    periods = [50, 125 / 3, 32, None]
    for (i, fmt, x, loop, eg, lfo, a), smp, per in zip(sounds, samples, periods):
        off, n, lp, k, _, _, sfmt, _, e0, e1, e2, l0, l1 = smp
        dec = g.gew_decode(data[off:off + g.gew_bytes(fmt, n)], fmt, n)
        check(dec == x and n == len(x) and lp == loop and sfmt == fmt,
              f"S{i:03d}: {n} samples {'12' if fmt == g.FMT_GEW12 else '8'}-bit as stored, loop {lp}")
        check((e0, e1, e2, l0, l1) == tuple(eg) + tuple(lfo), f"S{i:03d}: envelope / LFO bytes kept")
        if per:
            check(abs(k / 65536 - per) < per * 1e-4, f"S{i:03d}: tuned to its loop's period {k / 65536:.3f} "
                  f"(is {per:.3f})")
        else:
            check(abs(k / 65536 - 32000 / 261.6256) < 0.01, f"S{i:03d}: unpitched: C4 at 32 kHz")

    # 2. the voice, host build with this data
    r = subprocess.run([sys.executable, os.path.join(FEAT, "gen_pss11.py"), "--pss-rom", path, "--out", OUT],
                       capture_output=True, text=True)
    check(r.returncode == 0, "gen_pss11.py --pss-rom writes pss_data.inc / pss_rom.bin / pss_rom.inc")
    exe = os.path.join(OUT, "awm_host")
    r = subprocess.run(["cc", "-std=c99", "-O2", "-Wall", "-Wextra", f'-DPSS_DATA_INC="{OUT}/pss_data.inc"',
                        f'-DPSS_ROM_INC="{OUT}/pss_rom.inc"', "-o", exe, os.path.join(HERE, "awm_host.c"),
                        os.path.join(FEAT, "pss_core.c"), "-lm"], capture_output=True, text=True)
    check(r.returncode == 0, "awm_host.c builds with the synthetic data" + (": " + r.stderr[:400] if r.returncode else ""))
    if r.returncode == 0:
        m = dict(line.split(" ", 1) for line in subprocess.run([exe], capture_output=True, text=True).stdout.splitlines())
        f = {k: float(v) for k, v in m.items() if k.startswith("s")}
        check(abs(f["s0_a4_hz"] - 440) < 0.5, f"S000 (8-bit) key 69: {f['s0_a4_hz']:.2f} Hz (440)")
        check(abs(f["s0_c3_hz"] - 130.813) < 0.3, f"S000 key 48: {f['s0_c3_hz']:.2f} Hz (130.81)")
        check(abs(f["s0_f8_hz"] - 5587.65) < 3, f"S000 key 113 (step 3.2: direct reads): {f['s0_f8_hz']:.1f} Hz "
              "(5587.65)")
        check(abs(f["s1_a4_hz"] - 440) < 1.0, f"S001 (12-bit, period 41.667) key 69: {f['s1_a4_hz']:.2f} Hz (440)")
        check(abs(f["s1_vib_hi"] - 79.3) < 4 and abs(f["s1_vib_lo"] + 79.3) < 4,
              f"S001 vibrato depth 7: {f['s1_vib_lo']:+.1f} .. {f['s1_vib_hi']:+.1f} cents (+-79.3)")
        lfo7 = int(256 * g.LFO_FREQ[7] * 256 / g.CHIP_RATE) * g.CHIP_RATE / 65536   # MAME's truncated step
        check(abs(f["s1_vib_hz"] - lfo7) < 0.2, f"S001 LFO speed 7: {f['s1_vib_hz']:.2f} Hz "
              f"({lfo7:.3f}: 7.066 nominal, MAME's step truncates)")
        check(abs(f["s2_a4_hz"] - 440) < 1.0, f"S002 (reversed start) key 69: {f['s2_a4_hz']:.2f} Hz (440)")
        check(abs(f["s2_trem_db"] - 24) < 1.5, f"S002 tremolo depth 7: {f['s2_trem_db']:.1f} dB (24)")
        check(f["s3_mean_abs"] > 1000, f"S003 (noise) sounds: mean |x| {f['s3_mean_abs']:.0f}")

    # 3. cycles on the Blackfin model with this data (needs the toolchain, as every bench)
    sys.path.insert(0, HERE)
    sys.path.insert(0, os.path.join(SDK, "sim"))
    import run_psstest  # noqa: E402
    check(run_psstest.rom_bench(OUT, "synthetic GEW7 ROM") == 0, "cycles per voice block within the budget")

    print("PASS" if not fails else f"FAIL ({len(fails)})")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
