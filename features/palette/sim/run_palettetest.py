#!/usr/bin/env python3
"""Run the PALETTE engine (and the engine host) of recipes/op1plus.json (+ palette, before
engine_plus, while it is not in the recipe) in the Blackfin simulator.

    run_palettetest.py        (composes the image first; no firmware needed)

Checks: plain drwave untouched; the 'palette' preset = bank entry 1, in drwave's ranges; the host
routes extended tag 9 (slots 5-7 tag 7, slot 8 tag 2) to PALETTE; poly: held keys sound together, a
key up releases its voice; knob push = page 1..6; LEVEL clamps with its fields kept, WAVE / PITCH /
TIME one step per detent, the tag kept; bank (22 in range, then P-- with the knobs back); page 6 =
FX knobs; "palette" -> "drwave" in the engine list; cycles; TE's LFO on slot 1 does not touch the
fields stored under it; screens (pages 1 and 2); polyphony by tones (4 / 3 / 6 notes for 3 / 4 / 1 tones); the sounding note; the
patch's name for 1 s.
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed check bits, see driver.c)."""

import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "palettetest")    # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, run_gdb, composed  # noqa: E402
from build import compile_project  # noqa: E402

RECIPE = json.load(open(os.path.join(SDK, "recipes", "op1plus.json")))
DRIVER_AT, STACK = 0x01F00000, 0x01F7FFFC
STUBS = {0x0197C870: 0, 0x0197A826: 1, 0x01A00D14: 2, 0x019B6C74: 3, 0x019B6C0C: 4, 0x019FF664: 5, 0x019B2674: 6}


def features():
    fs = list(RECIPE["features"])
    if "palette" not in fs:         # engine_plus consumes knob pushes on any engine screen: before it
        fs.insert(fs.index("engine_plus") if "engine_plus" in fs else len(fs), "palette")
    return fs


def main():
    proj, cfg = composed(features(), "palettebench")
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    driver, d = c_bin(
        [os.path.join(HERE, "crt.S"), os.path.join(HERE, "driver.c")],
        DRIVER_AT, os.path.join(OUT, "driver"),
        {"STACK": STACK, "_wh_te_render": syms["_eh_te_render"], "_wh_te_note_on": syms["_eh_te_note_on"],
         "_compose_init": syms["_compose_init"],
         "_engine_name": syms["_compose_engine_name"], "_pl_page": syms["_pl_page"], "_pl_bank": syms["_pl_bank"],
         "_pl_title_on": syms["_pl_title_on"], "_pl_note_shown": syms["_pl_note_shown"], "_pl_tick": syms["_pl_tick"],
         "_pl_param": syms["_pl_param"], "_pl_param_set": syms["_pl_param_set"], "_pl_bank_knobs": syms["_pl_bank_knobs"],
         "_pl_npatches": syms["_pl_npatches"],
         "_hook_key": syms["__sdk_hook_019b6c74"], "_hook_frame": syms["__sdk_hook_019ab8b2"], "_FB": 0x03000000,
         "_hook_knob": syms["__sdk_hook_019b6c0c"]},
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
    elf_run = os.path.join(OUT, "palettetest_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=300)
    vals = run_gdb(elf_run, [f"*(unsigned*){d[s]:#x}" for s in ("_cycles_one", "_cycles_four")]
                   + [f"/x *(unsigned*){d['_fails']:#x}"],
                   [("palette_screen", 0x03000000), ("palette_screen_p2", 0x03040000)], timeout=300)
    if len(vals) == 3 and r.returncode:
        print(f"failed check bits: {vals[2]}")
    if len(vals) >= 2:
        print(f"cycles per voice block (128 samples): one tone (organ) {vals[0]}, "
              f"four tones (VOX CHOIR) {vals[1]} (x 3 sounding notes, 12-tone budget: {3 * int(vals[1])})")
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode}; R7 = failed check bits)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
