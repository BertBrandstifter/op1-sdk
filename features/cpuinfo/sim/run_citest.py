#!/usr/bin/env python3
"""Run the cpuinfo probe of recipes/op1cpu.json in the Blackfin simulator.

    run_citest.py           (composes recipes/op1cpu.json first; no firmware needed)

Checks: nothing before the panel opens; shift + speech wraps TE's audio callback (it still runs
every block); the cycle counter enabled; per-window work < period, SDK engines < work; a re-set
callback is wrapped again. Saves the panel as build/demo/cpuinfo_screen.png.
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed check bits, see driver.c)."""

import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "citest")     # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, run_gdb, composed  # noqa: E402
from build import compile_project  # noqa: E402

RECIPE = json.load(open(os.path.join(SDK, "recipes", "op1cpu.json")))
DRIVER_AT, STACK = 0x01F40000, 0x01F7FFFC   # the composed image is past 1 MiB (0x01F00000)
STUBS = {0x0197C870: 0, 0x0197A826: 1, 0x01A00D14: 2, 0x019B6C74: 3, 0x019B6C0C: 4, 0x019FF664: 5, 0x019B2674: 6}
MMRS = [(0xffc00000, 0x100)]                   # PLL_CTL / PLL_DIV / VR_CTL (not in the sim's default map)
TE_STUBS = os.path.join(SDK, "features", "acid", "sim", "te_stubs.S")    # same TE stand-ins


def main():
    proj, cfg = composed(RECIPE["features"], "cibench")
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    need = ["ci_main", "ci_avg_w", "ci_avg_p", "ci_avg_e", "ci_windows", "ci_rewraps", "ci_ccen_was",
            "ac_bank_knobs"]
    driver, d = c_bin(
        [os.path.join(HERE, "crt.S"), os.path.join(HERE, "driver.c")],
        DRIVER_AT, os.path.join(OUT, "driver"),
        {"STACK": STACK, **{"_" + n: syms["_" + n] for n in need},
         "_wh_te_render": syms["_eh_te_render"], "_wh_te_note_on": syms["_eh_te_note_on"], "_compose_init": syms["_compose_init"],
         "_hook_key": syms["__sdk_hook_019b6c74"], "_hook_frame": syms["__sdk_hook_019ab8b2"], "_FB": 0x03000000},
    )
    segs = [(0x01E00000, code), (DRIVER_AT, driver), (0x03000000, bytes(320 * 512))]
    for addr, which in STUBS.items():
        stub, _ = asm_bin(
            TE_STUBS, addr, os.path.join(OUT, f"stub{which}"),
            {"WHICH": which, "DRWAVE_RENDER": d["_drwave_render"], "DRWAVE_NOTE_ON": d["_drwave_note_on"],
             "LOG_SAVE": d["_log_save"], "LOG_FXSAVE": d["_log_fxsave"], "LOG_KEY": d["_log_key"], "LOG_KNOB": d["_log_knob"]},
            entry="_stub",
        )
        segs.append((addr, stub))
    elf_run = os.path.join(OUT, "citest_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=300, regions=MMRS)
    vals = run_gdb(elf_run, [f"*(unsigned*){syms['_' + n]:#x}" for n in ("ci_avg_w", "ci_avg_p", "ci_avg_e")]
                   + [f"/x *(unsigned*){d['_fails']:#x}"], [("cpuinfo_screen", 0x03000000)], timeout=300, regions=MMRS)
    if len(vals) >= 4:
        print(f"last window: work {vals[0]}, period {vals[1]}, SDK engines {vals[2]} cycles per block")
        if r.returncode:
            print(f"failed check bits: {vals[3]}")
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode}; R7 = failed check bits)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
