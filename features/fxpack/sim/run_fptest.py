#!/usr/bin/env python3
"""Run the FXPACK effects of recipes/op1plus.json in the Blackfin simulator.

    run_shtest.py           (composes recipes/op1plus.json first; no firmware needed)

Checks: plain phone = TE's process untouched; an FXPACK effect runs instead of phone (synth FX,
drum play mode, stock while TE switches); master FX per channel; the shared FX-name points
rename all seven names to "phone" (and still shimmer/reso); page 2: blue = TYPE (one effect per
click, marker kept), the others save in range; engine screen and other FX not claimed. Prints the
worst cycles per 64-sample block over the seven effects.
HLT = exit 0 = pass, ABORT = exit 1 = fail (failed check bits printed)."""

import json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "fptest")     # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, BIN, composed  # noqa: E402
from build import compile_project  # noqa: E402

RECIPE = json.load(open(os.path.join(SDK, "recipes", "op1plus.json")))
DRIVER_AT, STACK = 0x01F00000, 0x01F7FFFC
STUBS = {0x01A1BB74: 0, 0x01A1C25C: 1, 0x019FF664: 2, 0x019B6C74: 3, 0x019B6C0C: 4, 0x0193EF3C: 5}


def main():
    proj, cfg = composed(RECIPE["features"], RECIPE["name"])
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    need = ["fp_te_mono", "fp_te_stereo", "fp_page", "compose_fx_name", "compose_master_fx_name",
            "compose_fx_patch", "compose_fx_current_name", "compose_master_fx_current_name"]
    driver, d = c_bin(
        [os.path.join(HERE, "crt.S"), os.path.join(HERE, "driver.c")],
        DRIVER_AT, os.path.join(OUT, "driver"),
        {"STACK": STACK, **{"_" + n: syms["_" + n] for n in need},
         "_hook_key": syms["__sdk_hook_019b6c74"], "_hook_knob": syms["__sdk_hook_019b6c0c"]},
    )
    segs = [(0x01E00000, code), (DRIVER_AT, driver)]
    for addr, which in STUBS.items():
        stub, _ = asm_bin(
            os.path.join(HERE, "te_stubs.S"), addr, os.path.join(OUT, f"stub{which}"),
            {"WHICH": which, "PHONE_MONO": d["_phone_mono"], "PHONE_STEREO": d["_phone_stereo"],
             "LOG_SAVE": d["_log_save"], "LOG_MASTER": d["_log_master"], "LOG_KEY": d["_log_key"], "LOG_KNOB": d["_log_knob"]},
            entry="_stub",
        )
        segs.append((addr, stub))
    elf_run = os.path.join(OUT, "fptest_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=600)
    out = subprocess.run(
        [BIN + "gdb", "-batch", "-ex", "target sim --model bf524 --memory-region 0xff800000,0x8000 "
         "--memory-region 0xff900000,0x8000", "-ex", "load", "-ex", "run",
         "-ex", f"p *(unsigned*){d['_cycles']:#x}", "-ex", f"p/x *(unsigned*){d['_fails']:#x}",
         "-ex", f"p *(unsigned*){d['_cycles_st']:#x}", "-ex", f"p *(unsigned[7]*){d['_each']:#x}", elf_run],
        capture_output=True, text=True, timeout=600).stdout.split("\n")
    vals = [l.split("=")[-1].strip() for l in out if l.startswith("$")]
    if len(vals) == 4:
        print("per effect (PHASER FLANGER FOLD FUZZ DJ TRANSIENT TEXTURE):", vals[3])
        vals = vals[:3]
    if len(vals) == 3:
        print(f"cycles per FX block (64 samples): worst effect mono {vals[0]}, master stereo {vals[2]}")
    if len(vals) >= 2 and r.returncode:
        print(f"failed check bits: {vals[1]}")
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode})")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
