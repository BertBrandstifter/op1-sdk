#!/usr/bin/env python3
"""Run the WARP engine (and the engine host) of recipes/op1plus.json in the Blackfin simulator.

    run_warptest.py        (composes recipes/op1plus.json first; no firmware needed)

Checks: plain drwave untouched; the host routes tag 6 to WARP and tag 5 to WHEEL; WARP pitch from TE's
per-sample buffer (plain sine, 441 Hz) and note restart; knob push = page 1/2/3; knob edits keep tag 6
and stay in drwave's ranges; shift + blue = bank (all 16 entries in range, tag 6); page 3 = FX knobs;
"warp" -> "drwave" in the engine list (chained with LOFI and WHEEL). Prints cycles per voice block.
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed check bits, see driver.c)."""

import json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "warptest")     # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, BIN, composed  # noqa: E402
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
         "_engine_name": syms["_compose_engine_name"], "_wp_page": syms["_wp_page"], "_wp_bank": syms["_wp_bank"],
         "_lf_page": syms["_lf_page"], "_ep_page": syms["_ep_page"],
         "_hook_key": syms["__sdk_hook_019b6c74"], "_hook_frame": syms["__sdk_hook_019ab8b2"], "_FB": 0x03000000, "_hook_knob": syms["__sdk_hook_019b6c0c"]},
    )
    segs = [(0x01E00000, code), (DRIVER_AT, driver), (0x03000000, bytes(320 * 512))]
    for addr, which in STUBS.items():
        stub, _ = asm_bin(
            os.path.join(HERE, "te_stubs.S"), addr, os.path.join(OUT, f"stub{which}"),
            {"WHICH": which, "DRWAVE_RENDER": d["_drwave_render"], "DRWAVE_NOTE_ON": d["_drwave_note_on"],
             "LOG_SAVE": d["_log_save"], "LOG_FXSAVE": d["_log_fxsave"], "LOG_KEY": d["_log_key"], "LOG_KNOB": d["_log_knob"]},
            entry="_stub",
        )
        segs.append((addr, stub))
    elf_run = os.path.join(OUT, "warptest_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=300)
    out = subprocess.run(
        [BIN + "gdb", "-batch", "-ex", "target sim --model bf524 --memory-region 0xff800000,0x8000 "
         "--memory-region 0xff900000,0x8000", "-ex", "load", "-ex", "run",
         "-ex", f"p *(unsigned*){d['_cycles_plain']:#x}", "-ex", f"p *(unsigned*){d['_cycles_warp']:#x}",
         "-ex", f"p *(unsigned*){d['_cycles_heavy']:#x}", "-ex", f"p/x *(unsigned*){d['_fails']:#x}", elf_run],
        capture_output=True, text=True, timeout=300).stdout.split("\n")
    vals = [l.split("=")[-1].strip() for l in out if l.startswith("$")]
    if len(vals) == 4 and r.returncode:
        print(f"failed check bits: {vals[3]}")
    if len(vals) >= 3:
        print(f"cycles per voice block (128 samples): plain sine {vals[0]}, SAW + SYNC {vals[1]}, "
              f"FM + unison + colour + motion + sweep {vals[2]}")
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode}; R7 = failed check bits)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
