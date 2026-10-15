#!/usr/bin/env python3
"""Run the engine layer of recipes/op1plus.json in the Blackfin simulator.

    run_eptest.py           (builds recipes/op1plus.json first; no firmware needed)

Checks: layer off = TE's output bit-exact, layer on changes it, knob click paging consumed
only on the synth engine screen, flipped knob writes slot 5-8 and calls TE's save, dsynth is
routed to TE's controller as id 4..7. Prints the DSP cost per 128-sample voice block.
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed check bits, see driver.c)."""

import json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "eptest")     # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, BIN, composed  # noqa: E402
from build import compile_project  # noqa: E402

FEATURES, APP = ["quantize", "engine_plus"], "op1plus"   # composed by tools/compose.py
DRIVER_AT, STACK = 0x01F00000, 0x01F7FFFC
STUBS = {0x01A4F098: 0, 0x01A00D14: 1, 0x019B6C74: 2, 0x019B6C0C: 3, 0x019A23D4: 4}


def main():
    proj, cfg = composed(FEATURES, APP)
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    driver, dsyms = c_bin(
        [os.path.join(HERE, "crt.S"), os.path.join(HERE, "driver.c")],
        DRIVER_AT, os.path.join(OUT, "driver"),
        {"STACK": STACK, "_ep_render": syms["_ep_render"], "_ep_page": syms["_ep_page"],
         "_hook_key": syms["__sdk_hook_019b6c74"], "_hook_knob": syms["__sdk_hook_019b6c0c"]},
    )
    segs = [(0x01E00000, code), (DRIVER_AT, driver)]
    for addr, which in STUBS.items():
        stub, _ = asm_bin(
            os.path.join(HERE, "te_stubs.S"), addr, os.path.join(OUT, f"stub{which}"),
            {"WHICH": which, "FAKE_RENDER": dsyms["_fake_render"], "LOG_SAVE": dsyms["_log_save"],
             "LOG_KEY": dsyms["_log_key"], "LOG_KNOB": dsyms["_log_knob"],
             "FM_RENDER": dsyms["_fm_render"]},
            entry="_stub",
        )
        segs.append((addr, stub))
    elf_run = os.path.join(OUT, "eptest_run.elf")
    r = run_model(elf_run, segs, dsyms["_start"], timeout=120)
    # cycle counts, read back with gdb at the final HLT
    out = subprocess.run(
        [BIN + "gdb", "-batch", "-ex", "target sim --model bf524 --memory-region 0xff800000,0x8000 "
         "--memory-region 0xff900000,0x8000", "-ex", "load", "-ex", "run",
         "-ex", f"p *(unsigned*){dsyms['_cycles_off']:#x}", "-ex", f"p *(unsigned*){dsyms['_cycles_on']:#x}",
         elf_run], capture_output=True, text=True).stdout.split("\n")
    vals = [l.split("=")[-1].strip() for l in out if l.startswith("$")]
    if len(vals) == 2:
        print(f"cycles per voice block (128 samples): layer off {vals[0]}, all four stages on {vals[1]}")
    print("checks: off = bit-exact, on = processed, click paging, slot write + save, dsynth -> TE id 4..7, "
          "other screens untouched, fm hidden ratios applied during render and restored")
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode}; R7 = failed check bits)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
