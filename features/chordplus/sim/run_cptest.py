#!/usr/bin/env python3
"""Run features/chordplus (with requantize behind it in the keyboard chain) in the Blackfin simulator.

    run_cptest.py           (composes chordplus + requantize first; no firmware needed)

driver.c plays TE: keyboard note call sites, on_key, on_knob and the Seq tick, and logs what TE's
voice code (stubs at 0x01a85750 / 0x01a85840) would play. Checks: off = stock, shift + insert
toggles (consumed), chords, black-key modifiers silent, shared chord notes sound once and stop
with the last chord, panel knobs,
strum timing + release before a strummed note, off again, drum playmode untouched.
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed-check bits, see driver.c)."""

import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "cptest")     # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, composed  # noqa: E402
from build import compile_project  # noqa: E402

FEATURES, APP = ["chordplus", "requantize"], "cptest"
DRIVER_AT, STACK = 0x01F00000, 0x01F7FFFC
STUBS = {0x01A85750: 0, 0x01A85840: 1, 0x01A0D2C0: 2, 0x019B6C74: 3, 0x019B2674: 4, 0x019B6C0C: 5}
FB = 0x03000000


def main():
    proj, cfg = composed(FEATURES, APP)
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    hooks = {"hook_kb_on": "019ac290", "hook_kb_off": "019ac3a6", "hook_tick": "01a431aa",
             "hook_key": "019b6c74", "hook_frame": "019ab8b2", "hook_knob": "019b6c0c"}
    driver, dsyms = c_bin(
        [os.path.join(HERE, "crt.S"), os.path.join(HERE, "driver.c")],
        DRIVER_AT, os.path.join(OUT, "driver"),
        {"STACK": STACK, "_FB": FB, "_cp_on": syms["_cp_on"],
         "_cp_strum": syms["_cp_strum"], "_cp_set": syms["_cp_set"],
         **{"_" + k: syms[f"__sdk_hook_{v}"] for k, v in hooks.items()}},
    )
    segs = [(0x01E00000, code), (DRIVER_AT, driver), (FB, bytes(320 * 512))]
    for addr, which in STUBS.items():
        stub, _ = asm_bin(
            os.path.join(HERE, "te_stubs.S"), addr, os.path.join(OUT, f"stub{which}"),
            {"WHICH": which, "LOG_ON": dsyms["_log_on"], "LOG_OFF": dsyms["_log_off"],
             "LOG_TICK": dsyms["_log_tick"], "LOG_KEY": dsyms["_log_key"],
             "LOG_KNOB": dsyms["_log_knob"]},
            entry="_stub",
        )
        segs.append((addr, stub))
    r = run_model(os.path.join(OUT, "cptest_run.elf"), segs, dsyms["_start"], timeout=120)
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode}; R7 = failed check bits)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
