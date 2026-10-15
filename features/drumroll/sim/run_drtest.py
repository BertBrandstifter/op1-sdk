#!/usr/bin/env python3
"""Run DRUM ROLL (recipe op1plus) in the Blackfin simulator against an emulated VoiceCtl + drum voice.

    run_drtest.py           (composes recipes/op1plus.json first; no firmware needed)

Checks: shift + green press opens on the drum screen (TE never sees it), green press flips the focus,
other screens keep TE's knobs; step entry; one bar at 120 BPM (four on the floor + a 3-step lane, block
exact); TONE -> note-on extra; VEL + LEN -> voice gain and gate with fade; the note-off half a step
later; LP filter per sample (sequenced, previewed and played by hand), unfiltered sample untouched;
synth playmode: no drum notes, note-offs held; tape sync + rewind; stop: every note-on got its
note-off; screen + badge draw; page 6 CLEAR dialog; patterns + settings loaded from and saved to an
emulated op1.db (table sdk_drumroll). Prints cycles (filtered voice block, frame, tick).
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed check bits, see driver.c)."""

import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "drtest")     # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, run_gdb, composed, FB_BYTES  # noqa: E402
from build import compile_project  # noqa: E402

RECIPE = json.load(open(os.path.join(SDK, "recipes", "op1plus.json")))
DRIVER_AT, STACK = 0x01F00000, 0x01F7FFFC
STUBS = {0x01A85750: 0, 0x01A85840: 1, 0x01A0D2C0: 2, 0x019B2674: 2, 0x019B6C74: 3,
         0x019B6C0C: 3, 0x019789EC: 4, 0x01978926: 5, 0x019D2D1C: 2}
DB_STUBS = {0x0196DD44: "_emu_db_op1", 0x0196F950: "_emu_db_exec", 0x0196E2F0: "_emu_db_prepare",
            0x01A7BB20: "_emu_step", 0x01A7C6B8: "_emu_column", 0x01A7B202: "_emu_finalize",
            0xFFA0B6E8: "_emu_pend", 0xFFA0B96C: "_emu_post"}
L1_CODE = (0xFFA00000, 0x10000)                 # the VDK semaphore calls live in L1 instruction SRAM


def main():
    proj, cfg = composed(RECIPE["features"], RECIPE["name"])
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    hooks = {"hook_tick": "01a431aa", "hook_key": "019b6c74", "hook_knob": "019b6c0c",
             "hook_frame": "019ab8b2", "hook_main": "019f0a6a"}
    driver, d = c_bin(
        [os.path.join(HERE, "crt.S"), os.path.join(HERE, "driver.c"), os.path.join(HERE, "..", "dr_store.c"),
         os.path.join(HERE, "..", "dr_core.c"), os.path.join(SDK, "lib", "rt.c")],
        DRIVER_AT, os.path.join(OUT, "driver"),
        {"STACK": STACK, "_FB": 0x03000000,
         **{s: syms[s] for s in ("_compose_init", "_dr_te_note_on", "_dr_te_render", "_dr_pat", "_dr_run",
                                 "_dr_open", "_dr_page", "_dr_sel", "_dr_cursor", "_dr_param", "_dr_cur", "_rq_state", "_dr_te_drum_pick", "_dr_roll",
                                 "_dr_swing", "_dr_zoom", "_dr_ask", "_dr_ask_yes")},
         **{"_" + k: syms[f"__sdk_hook_{v}"] for k, v in hooks.items()}},
    )
    segs = [(0x01E00000, code), (DRIVER_AT, driver), (0x03000000, bytes(4 * FB_BYTES))]
    for addr, which in STUBS.items():
        stub, _ = asm_bin(
            os.path.join(HERE, "te_stubs.S"), addr, os.path.join(OUT, f"stub{addr:08x}"),
            {"WHICH": which, "EMU_ON": d["_emu_on"], "EMU_OFF": d["_emu_off"], "LOG_INPUT": d["_log_input"],
             "EMU_RENDER": d["_emu_render"], "EMU_VOICE_ON": d["_emu_voice_on"]},
            entry="_stub",
        )
        segs.append((addr, stub))
    for addr, target in DB_STUBS.items():
        stub, _ = asm_bin(
            os.path.join(HERE, "te_stubs.S"), addr, os.path.join(OUT, f"stub{addr:08x}"),
            {"WHICH": 6, "TARGET": d[target]}, entry="_stub",
        )
        segs.append((addr, stub))
    elf_run = os.path.join(OUT, "drtest_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=600, regions=[L1_CODE])
    vals = run_gdb(elf_run, [f"*(unsigned*){d[s]:#x}" for s in ("_cycles_render", "_cycles_frame", "_cycles_tick",
                                                                 "_cycles_lp", "_cycles_plain")]
                   + [f"/x *(unsigned*){d['_fails']:#x}"],
                   [("drumroll_step", 0x03000000 + FB_BYTES), ("drumroll_sound", 0x03000000 + 2 * FB_BYTES),
                    ("drumroll_clear", 0x03000000 + 3 * FB_BYTES)],
                   timeout=600, regions=[L1_CODE])
    if len(vals) == 6:
        print(f"cycles per voice block (128 samples, incl. TE's render stub {vals[4]}): LP {vals[3]}, "
              f"LP + gain + gate {vals[0]}; DRUM ROLL screen per frame {vals[1]}; seq tick chain {vals[2]}")
        if r.returncode:
            print(f"failed check bits: {vals[5]}")
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode}; R7 = failed check bits)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
