#!/usr/bin/env python3
"""Run NOTE ROLL (recipe op1plus) in the Blackfin simulator against an emulated VoiceCtl and an
emulated synth finger sequencer (TE's synth data, player with step events, get_note, mark-dirty).

    run_nrtest.py           (composes recipes/op1plus.json first; no firmware needed)

Checks (driver.c): opens only on the synth finger screen (shift + blue press; TE and requantize never
see it); import of a TE pattern; TE plays its own patterns; the first edit makes a pattern NOTE
ROLL's + TE's summary; chords; NOTE ROLL plays it on TE's step events (block exact, synth octave,
velocity, lengths, ties), stop on release; TE's step record / delete / length into NOTE ROLL
patterns; drum playmode silent (note-offs kept); page 5 CLEAR; op1.db load / save (table
sdk_noteroll); screen + the finger screen note. Prints cycles (frame, tick).
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed check bits, see driver.c)."""

import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "nrtest")     # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, run_gdb, composed, FB_BYTES  # noqa: E402
from build import compile_project  # noqa: E402

RECIPE = json.load(open(os.path.join(SDK, "recipes", "op1plus.json")))
DRIVER_AT, STACK = 0x01F00000, 0x01F7FFFC
STUBS = {0x01A85750: 0, 0x01A85840: 1, 0x01A0D2C0: 2, 0x019B2674: 2, 0x019B6C74: 3,
         0x019B6C0C: 3, 0x019789EC: 4, 0x01978926: 5, 0x019D2D1C: 2}
CALL_STUBS = {0x0196DD44: "_emu_db_op1", 0x0196F950: "_emu_db_exec", 0x0196E2F0: "_emu_db_prepare",
              0x01A7BB20: "_emu_step", 0x01A7C6B8: "_emu_column", 0x01A7B202: "_emu_finalize",
              0xFFA0B6E8: "_emu_pend", 0xFFA0B96C: "_emu_post",
              0x019ECCF0: "_emu_get_note", 0x019ECC5C: "_emu_set_note", 0x0193EF3C: "_emu_mark_dirty"}
L1_CODE = (0xFFA00000, 0x10000)                 # the VDK semaphore calls live in L1 instruction SRAM
SHOTS = ("noteroll_note", "noteroll_chord", "noteroll_finger", "noteroll_punch")


def main():
    proj, cfg = composed(RECIPE["features"], RECIPE["name"])
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    hooks = {"hook_tick": "01a431aa", "hook_key": "019b6c74", "hook_knob": "019b6c0c",
             "hook_frame": "019ab8b2", "hook_main": "019f0a6a"}
    driver, d = c_bin(
        [os.path.join(HERE, "crt.S"), os.path.join(HERE, "driver.c"), os.path.join(HERE, "..", "nr_store.c"),
         os.path.join(HERE, "..", "nr_core.c"), os.path.join(SDK, "lib", "rt.c")],
        DRIVER_AT, os.path.join(OUT, "driver"),
        {"STACK": STACK, "_FB": 0x03000000,
         **{s: syms[s] for s in ("_compose_init", "_nr_pat", "_nr_play", "_nr_open", "_nr_page", "_nr_cur",
                                 "_nr_cursor", "_nr_voice", "_nr_owned", "_nr_ask", "_nr_ask_yes", "_nr_ph_shown",
                                 "_nr_fs_get_note", "_rq_state", "_dr_open", "_nr_px_held", "_nr_px_bank", "_nr_cpitch", "_nr_rec", "_nr_undo_n", "_nr_redo_n")},
         **{"_" + k: syms[f"__sdk_hook_{v}"] for k, v in hooks.items()}},
    )
    segs = [(0x01E00000, code), (DRIVER_AT, driver), (0x03000000, bytes((len(SHOTS) + 1) * FB_BYTES))]
    for addr, which in STUBS.items():
        stub, _ = asm_bin(
            os.path.join(HERE, "te_stubs.S"), addr, os.path.join(OUT, f"stub{addr:08x}"),
            {"WHICH": which, "EMU_ON": d["_emu_on"], "EMU_OFF": d["_emu_off"], "LOG_INPUT": d["_log_input"],
             "EMU_RENDER": d["_emu_render"], "EMU_VOICE_ON": d["_emu_voice_on"]},
            entry="_stub",
        )
        segs.append((addr, stub))
    for addr, target in CALL_STUBS.items():
        stub, _ = asm_bin(
            os.path.join(HERE, "te_stubs.S"), addr, os.path.join(OUT, f"stub{addr:08x}"),
            {"WHICH": 6, "TARGET": d[target]}, entry="_stub",
        )
        segs.append((addr, stub))
    elf_run = os.path.join(OUT, "nrtest_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=600, regions=[L1_CODE])
    vals = run_gdb(elf_run, [f"*(unsigned*){d[s]:#x}" for s in ("_cycles_frame", "_cycles_tick")]
                   + [f"/x *(unsigned*){d['_fails']:#x}"],
                   [(n, 0x03000000 + (i + 1) * FB_BYTES) for i, n in enumerate(SHOTS)],
                   timeout=600, regions=[L1_CODE])
    if len(vals) == 3:
        print(f"cycles: NOTE ROLL screen per frame {vals[0]}; seq tick chain {vals[1]}")
        if r.returncode:
            print(f"failed check bits: {vals[2]}")
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode}; R7 = failed check bits)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
