#!/usr/bin/env python3
"""Run the MONO 3 engine (and the engine host) of recipes/op1plus.json in the Blackfin simulator.

    run_monotest.py        (composes recipes/op1plus.json first; no firmware needed)

Checks: plain drwave untouched; the 'mono 3' preset = bank entry 1; the host routes tag 2 to MONO;
pitch from TE's per-sample buffer (a saw, 441 Hz); poly: every voice sounds, held keys together, a
key up releases its voice; knob push = page 1..7; CUTOFF clamps with its fields kept, RANGE / WAVE /
FREQ / KEYBOARD one step per detent (max), tag 2 kept; bank (16 in range, then P-- with the knobs
back); page 7 = FX knobs; "mono 3" -> "drwave" in the engine list; cycles; TE's LFO on a slot does
not touch the fields stored under it; screens (bank entry 2 with a key sounding, page 5); 4 of 6
notes sound.
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed check bits, see driver.c)."""

import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "monotest")     # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, run_gdb, composed  # noqa: E402
from build import compile_project  # noqa: E402

RECIPE = json.load(open(os.path.join(SDK, "recipes", "op1plus.json")))
DRIVER_AT, STACK = 0x01F00000, 0x01F7FFFC
STUBS = {0x0197C870: 0, 0x0197A826: 1, 0x01A00D14: 2, 0x019B6C74: 3, 0x019B6C0C: 4, 0x019FF664: 5, 0x019B2674: 6}


def main():
    proj, cfg = composed(RECIPE["features"], RECIPE["name"])
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    driver, d = c_bin(
        [os.path.join(HERE, "crt.S"), os.path.join(HERE, "driver.c")],
        DRIVER_AT, os.path.join(OUT, "driver"),
        {"STACK": STACK, "_eh_render_" if False else "_wh_te_render": syms["_eh_te_render"], "_wh_te_note_on": syms["_eh_te_note_on"],
         "_compose_init": syms["_compose_init"],
         "_engine_name": syms["_compose_engine_name"], "_mo_page": syms["_mo_page"], "_mo_bank": syms["_mo_bank"],
         "_mo_title_on": syms["_mo_title_on"], "_mo_tick": syms["_mo_tick"],
         "_mo_param": syms["_mo_param"], "_mo_param_set": syms["_mo_param_set"], "_mo_bank_knobs": syms["_mo_bank_knobs"],
         "_hook_key": syms["__sdk_hook_019b6c74"], "_hook_frame": syms["__sdk_hook_019ab8b2"], "_FB": 0x03000000, "_hook_knob": syms["__sdk_hook_019b6c0c"]},
    )
    segs = [(0x01E00000, code), (DRIVER_AT, driver), (0x03000000, bytes(320 * 512)), (0x03040000, bytes(320 * 512))]
    for addr, which in STUBS.items():
        stub, _ = asm_bin(
            os.path.join(HERE, "te_stubs.S"), addr, os.path.join(OUT, f"stub{which}"),
            {"WHICH": which, "DRWAVE_RENDER": d["_drwave_render"], "DRWAVE_NOTE_ON": d["_drwave_note_on"],
             "LOG_SAVE": d["_log_save"], "LOG_FXSAVE": d["_log_fxsave"], "LOG_KEY": d["_log_key"], "LOG_KNOB": d["_log_knob"]},
            entry="_stub",
        )
        segs.append((addr, stub))
    elf_run = os.path.join(OUT, "monotest_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=300)
    vals = run_gdb(elf_run, [f"*(unsigned*){d[s]:#x}" for s in ("_cycles_plain", "_cycles_heavy")]
                   + [f"/x *(unsigned*){d['_fails']:#x}"], [("mono_screen", 0x03000000), ("mono_screen_p5", 0x03040000)], timeout=300)
    if len(vals) == 3 and r.returncode:
        print(f"failed check bits: {vals[2]}")
    if len(vals) >= 2:
        print(f"cycles per voice block (128 samples): a saw, open {vals[0]}, "
              f"+ 3 oscillators, pink noise, emphasis, contour, glide {vals[1]} (x 4 sounding voices: {4 * int(vals[1])})")
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode}; R7 = failed check bits)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
