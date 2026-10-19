#!/usr/bin/env python3
"""Run the LOFI engine of recipes/op1plus.json in the Blackfin simulator.

    run_lofitest.py         (composes recipes/op1plus.json first; no firmware needed)

Checks: plain pulse sound = TE's pulse render untouched; LOFI sound renders from TE's per-sample
pitch buffer (pitch checked), note_on restarts the voice; shift + white push is TE's (no gesture since
v0.7); knob push = page (engine_plus
does not see it), knob turns edit slots 1-4 / 5-8 and save; after LOFI is off everything is
TE's / engine_plus' again; screen (bank entry TRI BASS: build/demo/lofi_screen.png). Prints the
cycles per 128-sample voice block.
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed check bits, see driver.c)."""

import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "lofitest")     # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, run_gdb, composed  # noqa: E402
from build import compile_project  # noqa: E402

RECIPE = json.load(open(os.path.join(SDK, "recipes", "op1plus.json")))
DRIVER_AT, STACK = 0x01F00000, 0x01F7FFFC
STUBS = {0xFFA04B88: 0, 0x01A0DEE4: 1, 0x01A00D14: 2, 0x019B6C74: 3, 0x019B6C0C: 4}
L1I = (0xFFA00000, 0xC000)


def main():
    proj, cfg = composed(RECIPE["features"], RECIPE["name"])
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    driver, d = c_bin(
        [os.path.join(HERE, "crt.S"), os.path.join(HERE, "driver.c")],
        DRIVER_AT, os.path.join(OUT, "driver"),
        {"STACK": STACK, "_lf_te_render": syms["_lf_te_render"], "_lf_te_note_on": syms["_lf_te_note_on"],
         "_lf_te_range_check": syms["_lf_te_range_check"], "_lf_te_type_fetched": syms["_lf_te_type_fetched"],
         "_lf_page": syms["_lf_page"], "_lf_bank": syms["_lf_bank"], "_ep_page": syms["_ep_page"],
         "_hook_key": syms["__sdk_hook_019b6c74"], "_hook_knob": syms["__sdk_hook_019b6c0c"],
         "_hook_frame": syms["__sdk_hook_019ab8b2"], "_FB": 0x03000000},
    )
    segs = [(0x01E00000, code), (DRIVER_AT, driver), (0x03000000, bytes(320 * 512))]
    for addr, which in STUBS.items():
        stub, _ = asm_bin(
            os.path.join(HERE, "te_stubs.S"), addr, os.path.join(OUT, f"stub{which}"),
            {"WHICH": which, "PULSE_RENDER": d["_pulse_render"], "PULSE_NOTE_ON": d["_pulse_note_on"],
             "LOG_SAVE": d["_log_save"], "LOG_KEY": d["_log_key"], "LOG_KNOB": d["_log_knob"]},
            entry="_stub",
        )
        segs.append((addr, stub))
    elf_run = os.path.join(OUT, "lofitest_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=180, regions=[L1I])
    vals = run_gdb(elf_run, [f"*(unsigned*){d[s]:#x}" for s in ("_cycles_pulse", "_cycles_heavy")]
                   + [f"/x *(unsigned*){d['_fails']:#x}"], [("lofi_screen", 0x03000000)], timeout=180, regions=[L1I])
    if len(vals) == 3 and r.returncode:
        print(f"failed check bits: {vals[2]}")
    if len(vals) >= 2:
        print(f"cycles per voice block (128 samples): PULSE {vals[0]}, BYTE + all processing {vals[1]}")
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode}; R7 = failed check bits)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
