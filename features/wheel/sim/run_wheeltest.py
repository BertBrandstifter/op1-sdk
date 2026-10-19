#!/usr/bin/env python3
"""Run the WHEEL engine of recipes/op1plus.json in the Blackfin simulator.

    run_wheeltest.py        (composes recipes/op1plus.json first; no firmware needed)

Checks: plain drwave sound = TE's drwave render untouched; WHEEL sound renders from TE's per-sample
pitch buffer (pitch checked), note_on restarts the voice; knob push = page 1/2/3 (engine_plus and
LOFI do not see it), knob turns edit slots 1-4 / 5-8 inside drwave's ranges and save; shift + knob and
other screens stay TE's; page 3 edits the
sound's FX knobs inside TE's FX ranges and saves through TE's FX save; engine-list rename (shared point, chained with LOFI's); screens (bank entry 2 with a
key sounding, page 2) to build/demo/. Prints the cycles
per 128-sample voice block.
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed check bits, see driver.c)."""

import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "wheeltest")     # build output, never in the package
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
        {"STACK": STACK, "_wh_te_render": syms["_eh_te_render"], "_wh_te_note_on": syms["_eh_te_note_on"],
         "_compose_init": syms["_compose_init"],
         "_engine_name": syms["_compose_engine_name"], "_wh_page": syms["_wh_page"], "_wh_bank": syms["_wh_bank"],
         "_wh_title_on": syms["_wh_title_on"], "_wh_tick": syms["_wh_tick"],
         "_lf_page": syms["_lf_page"], "_ep_page": syms["_ep_page"],
         "_hook_key": syms["__sdk_hook_019b6c74"], "_hook_knob": syms["__sdk_hook_019b6c0c"],
         "_hook_frame": syms["__sdk_hook_019ab8b2"], "_FB": 0x03000000},
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
    elf_run = os.path.join(OUT, "wheeltest_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=300)
    vals = run_gdb(elf_run, [f"*(unsigned*){d[s]:#x}" for s in ("_cycles_flute", "_cycles_full", "_cycles_heavy")]
                   + [f"/x *(unsigned*){d['_fails']:#x}"], [("wheel_screen", 0x03000000), ("wheel_screen_p2", 0x03040000)])
    if len(vals) == 4 and r.returncode:
        print(f"failed check bits: {vals[3]}")
    if len(vals) >= 3:
        print(f"cycles per voice block (128 samples): FLUTE {vals[0]}, FULL {vals[1]}, "
              f"FULL + perc + click + rotary + drive {vals[2]}")
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode}; R7 = failed check bits)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
