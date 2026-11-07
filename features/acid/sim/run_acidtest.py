#!/usr/bin/env python3
"""Run the ACID 3 engine (and the engine host) of recipes/op1plus.json in the Blackfin simulator.

    run_acidtest.py        (composes recipes/op1plus.json (+ acid) first; no firmware needed)

Checks: plain drwave untouched; the 'acid 3' preset = bank entry 1; the host routes extended tag 8
to ACID; pitch from TE's per-sample buffer (a saw, 441 Hz); mono: the newest voice sounds, the one
before fades within a block; slide on a legato key (kb note observers), none without; knob push =
page 1..6; CUTOFF clamps with its fields kept, discrete fields one step, tag kept; bank (16 in range,
every sound audible and below clipping, then P--); page 6 = FX knobs; "acid 3" -> "drwave"; cycles;
TE's LFO on a slot does not touch the fields stored under it; screens; ACCENTS 2ND alternates; the
sound's name for 1 s.
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed check bits, see driver.c)."""

import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "acidtest")     # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, run_gdb, composed  # noqa: E402
from build import compile_project  # noqa: E402

RECIPE = json.load(open(os.path.join(SDK, "recipes", "op1plus.json")))
DRIVER_AT, STACK = 0x01F00000, 0x01F7FFFC
STUBS = {0x0197C870: 0, 0x0197A826: 1, 0x01A00D14: 2, 0x019B6C74: 3, 0x019B6C0C: 4, 0x019FF664: 5, 0x019B2674: 6}


def features():
    """the recipe, with acid before engine_plus (its knob pages claim every engine screen's pushes)"""
    fs = list(RECIPE["features"])
    if "acid" not in fs:
        fs.insert(fs.index("engine_plus") if "engine_plus" in fs else len(fs), "acid")
    return fs


def main():
    proj, cfg = composed(features(), "acidbench")
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    driver, d = c_bin(
        [os.path.join(HERE, "crt.S"), os.path.join(HERE, "driver.c")],
        DRIVER_AT, os.path.join(OUT, "driver"),
        {"STACK": STACK, "_eh_render_" if False else "_wh_te_render": syms["_eh_te_render"], "_wh_te_note_on": syms["_eh_te_note_on"],
         "_compose_init": syms["_compose_init"],
         "_engine_name": syms["_compose_engine_name"], "_ac_page": syms["_ac_page"], "_ac_bank": syms["_ac_bank"],
         "_ac_title_on": syms["_ac_title_on"], "_ac_note_shown": syms["_ac_note_shown"], "_ac_tick": syms["_ac_tick"],
         "_ac_param": syms["_ac_param"], "_ac_param_set": syms["_ac_param_set"], "_ac_bank_knobs": syms["_ac_bank_knobs"],
         "_kb_note_on": syms["_compose_kb_note_on"], "_kb_note_off": syms["_compose_kb_note_off"],
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
    elf_run = os.path.join(OUT, "acidtest_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=300)
    vals = run_gdb(elf_run, [f"*(unsigned*){d[s]:#x}" for s in ("_cycles_plain", "_cycles_heavy")]
                   + [f"/x *(unsigned*){d['_fails']:#x}"]
                   + [f"((int*){d['_bank_peak']:#x})[{i}]" for i in range(16)] + [f"((int*){d['_bank_rms']:#x})[{i}]" for i in range(16)] + [f"((int*){d['_acc_pk']:#x})[{i}]" for i in range(4)], [("acid_screen", 0x03000000), ("acid_screen_p2", 0x03040000)], timeout=300)
    if len(vals) >= 35:
        print('accent peaks:', vals[35:39])
        print("bank peak / rms dBFS:", ", ".join(f"{i + 1}: {int(vals[3 + i])} / {20 * __import__('math').log10(max(1, 16 * int(vals[19 + i]) ** 0.5) / 32768):.1f}" for i in range(16)))
    if len(vals) >= 3 and r.returncode:
        print(f"failed check bits: {vals[2]}")
    if len(vals) >= 2:
        print(f"cycles per audio block (one mono voice, 128 samples): a saw, open {vals[0]}, "
              f"+ sub, overdrive, resonance, filter FM, tracking, accent HIGH, slide, muffler {vals[1]}")
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode}; R7 = failed check bits)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
