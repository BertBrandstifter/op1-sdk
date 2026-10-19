#!/usr/bin/env python3
"""Run requantize (recipe op1plus) end to end against an emulated TE tape transport.

    run_rqtest.py           (composes recipes/op1plus.json first; no firmware needed)

Checks: shift+blue-press take/panel/exit, shift+stop left to TE, commit refused onto the original track, commit mutes the
original and selects the target track, seek -> injected rec+play -> quantized replay by tape
position (every note-on on the 1/16 tape grid) -> injected stop.
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed check bits, see driver.c)."""

import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "rqtest")     # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, run_gdb, composed, FB_BYTES  # noqa: E402
from build import compile_project  # noqa: E402

RECIPE = json.load(open(os.path.join(SDK, "recipes", "op1plus.json")))
DRIVER_AT, STACK = 0x01F00000, 0x01F7FFFC
STUBS = {0x01A85750: 0, 0x01A85840: 1, 0x01A0D2C0: 2, 0x019B2674: 2, 0x019B6C74: 3,
         0x019B6C0C: 3, 0x019C2F24: 4, 0x01A55024: 5, 0x01967ED0: 6, 0x019C2F04: 7}
FB = 0x03000000


def main():
    proj, cfg = composed(RECIPE["features"], RECIPE["name"])
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    hooks = {"hook_kb_on": "019ac290", "hook_kb_off": "019ac3a6", "hook_tick": "01a431aa",
             "hook_key": "019b6c74", "hook_knob": "019b6c0c", "hook_frame": "019ab8b2"}
    driver, d = c_bin(
        [os.path.join(HERE, "crt.S"), os.path.join(HERE, "driver.c")],
        DRIVER_AT, os.path.join(OUT, "driver"),
        {"STACK": STACK, "_FB": FB, "_rq_state": syms["_rq_state"], "_rq_nout": syms["_rq_nout"],
         "_rq_rec_start": syms["_rq_rec_start"], "_rq_rec_end": syms["_rq_rec_end"],
         **{s: syms[s] for s in ("_rq_page", "_rq_nnotes", "_rq_cur", "_rq_undo_n", "_rq_redo_n")},
         **{"_" + k: syms[f"__sdk_hook_{v}"] for k, v in hooks.items()}},
    )
    segs = [(0x01E00000, code), (DRIVER_AT, driver), (FB, bytes(3 * FB_BYTES))]
    for addr, which in STUBS.items():
        stub, _ = asm_bin(
            os.path.join(HERE, "te_stubs.S"), addr, os.path.join(OUT, f"stub{addr:08x}"),
            {"WHICH": which, "LOG_ON": d["_log_on"], "LOG_OFF": d["_log_off"], "LOG_INPUT": d["_log_input"],
             "EMU_PUSH": d["_emu_push"], "EMU_SEEK": d["_emu_seek"], "EMU_GAINS": d["_emu_gains"], "EMU_WAKE": d["_emu_wake"]},
            entry="_stub",
        )
        segs.append((addr, stub))
    elf_run = os.path.join(OUT, "rqtest_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=180)
    vals = run_gdb(elf_run, [f"/x *(unsigned*){d['_fails']:#x}"],
                   shots=[("requantize_take", FB), ("synthroll_note", FB + FB_BYTES),
                          ("synthroll_quant", FB + 2 * FB_BYTES)], timeout=180)
    if r.returncode and vals:
        print(f"failed check bits: {vals[0]}")
    print("checks (v2, emulated clip tape with overdub + lift + key latency): take/panel/exit, preview, "
          "commit replaces the take (lift, one clip with the original range, no merge, auto-stop), "
          "replay on the 1/16 grid while recording, commit to another track, piano roll (delete, "
          "transpose, fine move -> preview plays the edits)")
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode}; R7 = failed check bits)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
