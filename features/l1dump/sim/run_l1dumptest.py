#!/usr/bin/env python3
"""Run the l1dump feature in the Blackfin simulator: boot capture, audio modem and USB
carrier patching against planted L1 content.

    run_l1dumptest.py           (composes features/l1dump first; no firmware needed)

Checks (driver): nothing transmitted before shift + speech, capture built and callback
wrapped, key toggle consumed, USB carrier blocks patched (partial tail, foreign sector
untouched, one cache flush). Checks (here): the captured audio decodes (tools/l1dump_rx.py)
to exactly the planted L1I/A/B + scratchpad content. Saves the status line as
build/demo/l1dump_screen.png. HLT = exit 0 = pass, ABORT = exit 1 (R7 = failed check bits)."""

import json, os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(
    os.path.join(HERE, "..", "..", "..")
)
OUT = os.path.join(
    SDK, "build", "bench", "l1dumptest"
)  # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
sys.path.insert(0, os.path.join(SDK, "tools"))
from simtest import asm_bin, c_bin, run_model, run_gdb, composed, BIN  # noqa: E402
from build import compile_project  # noqa: E402
import l1dump_rx  # noqa: E402

DRIVER_AT, STACK = 0x01F00000, 0x01F7FFFC
# TE addresses -> WHICH in te_stubs.S
STUBS = {
    0x019B2674: 2,
    0x019B6C74: 3,
    0x01A5E3B0: 4,
    0x01AA6E10: 5,
    0x01AA185C: 6,
}
REGIONS = []  # L1I is instruction-only in the sim (and the default env has no DTEST
# device): the bench mirrors it in SDRAM (l1dump.c SDK_BENCH)
CAP, PANEL = 0x02000000, 0x03000000
REG_BYTES = 0x400  # SDK_BENCH region size (l1dump.c)
IMG_BYTES = 12 + 4 * 16 + 4 * REG_BYTES + 4
SEEDS = {"L1I0": 11, "L1A0": 22, "L1B0": 33, "SCRP": 44}
FEATURE = ["l1dump"]


def planted(tag):
    x, out = SEEDS[tag], bytearray()
    for _ in range(REG_BYTES):
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
        out.append(x >> 24)
    return bytes(out)


def main():
    proj, cfg = composed(FEATURE, "ldbench")
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")

    driver, d = c_bin(
        [os.path.join(HERE, "crt.S"), os.path.join(HERE, "driver.c")],
        DRIVER_AT,
        os.path.join(OUT, "driver"),
        {
            "STACK": STACK,
            "_hook_key": syms["__sdk_hook_019b6c74"],
            "_hook_frame": syms["__sdk_hook_019ab8b2"],
            **{
                "_" + n: syms["_" + n]
                for n in (
                    "ld_img",
                    "ld_img_size",
                    "ld_ready",
                    "ld_usb_patched",
                    "ld_tx_on",
                    "ld_cb",
                    "ld_msc_after",
                )
            },
        },
    )
    segs = [(0x01E00000, code), (DRIVER_AT, driver), (PANEL, bytes(320 * 512))]
    for addr, which in STUBS.items():
        stub, _ = asm_bin(
            os.path.join(HERE, "te_stubs.S"),
            addr,
            os.path.join(OUT, f"stub{which}"),
            {
                "WHICH": which,
                "TE_NEW": d["_te_new"],
                "TE_MEMCPY": d["_te_memcpy"],
                "TE_FLUSH": d["_te_flush"],
            },
            entry="_stub",
        )
        segs.append((addr, stub))
    elf_run = os.path.join(OUT, "l1dumptest_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=300, regions=REGIONS)
    vals = run_gdb(
        elf_run,
        [f"/x *(unsigned*){d['_fails']:#x}", f"*(unsigned*){d['_cap_n']:#x}"],
        [("l1dump_screen", PANEL)],
        timeout=300,
        regions=REGIONS,
    )
    fails, cap_n = int(vals[0], 16), int(vals[1])
    print(
        f"driver: {cap_n} samples captured"
        + (f", failed check bits: {fails}" if fails else "")
    )

    dump = os.path.join(OUT, "cap.raw")
    mem = ["0xff800000,0x8000", "0xff900000,0x8000"] + [
        f"{a:#x},{n:#x}" for a, n in REGIONS
    ]
    subprocess.run(
        [
            BIN + "gdb",
            "-batch",
            "-ex",
            "target sim --model bf524 " + " ".join(f"--memory-region {m}" for m in mem),
            "-ex",
            "load",
            "-ex",
            "run",
            "-ex",
            f"dump binary memory {dump} {CAP:#x} {CAP + 2 * cap_n:#x}",
            elf_run,
        ],
        check=True,
        timeout=300,
        capture_output=True,
    )
    wav = os.path.join(OUT, "cap.wav")
    l1dump_rx.write_wav(wav, struct.unpack(f"<{cap_n}h", open(dump, "rb").read()))

    ok = r.returncode == 0

    def check(regions):
        good = True
        for reg in regions:
            want = bytearray(planted(reg["tag"]))
            if reg["tag"] == "L1A0":
                # the driver plants the pattern, then sets TE_AUDIO_BLOCK_CB (0xff80036c)
                want[0x36C:0x370] = struct.pack("<I", d["_te_cb"])
            got = reg["data"]
            if got != want:
                diff = next(
                    i for i in range(min(len(got), len(want))) if got[i] != want[i]
                )
                print(f"FAIL: region {reg['tag']} differs at byte {diff}")
                good = False
            elif reg["len"] != REG_BYTES:
                print(f"FAIL: region {reg['tag']}: length {reg['len']} != {REG_BYTES}")
                good = False
        return good

    try:
        regions, stats = l1dump_rx.decode(wav)
        if check(regions):
            print(
                f"audio: {stats['ok']} packets ok, {stats['bad']} dropped, "
                f"{stats['symbols']} symbols -> all regions byte-exact"
            )
        else:
            ok = False
        # clock drift: stretch the capture by 234 ppm (64.015 samples/symbol); the
        # decoder assumes 64.0 - the per-sync early-late relock must absorb the rest
        x, fs = l1dump_rx.read_wav(wav)
        drifted = l1dump_rx._resample(x, 64.0 / 64.015)
        regions, stats = l1dump_rx.decode_samples(drifted, fs, progress=False)
        if check(regions):
            print(
                f"drift: {stats['ok']} packets ok, {stats['bad']} dropped "
                f"(234 ppm clock error) -> byte-exact"
            )
        else:
            ok = False
    except (ValueError, KeyError) as e:
        print(f"FAIL: decode: {e}")
        ok = False
    print(
        "PASS"
        if ok and r.returncode == 0
        else f"FAIL (exit {r.returncode}; R7 = check bits {fails:#x})"
    )
    sys.exit(0 if ok and r.returncode == 0 else 1)


if __name__ == "__main__":
    main()
