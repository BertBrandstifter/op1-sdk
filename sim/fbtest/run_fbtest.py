#!/usr/bin/env python3
"""Run the fb_test example (framebuffer drawing + both hook types) in the simulator.

    run_fbtest.py         (builds examples/fb_test first; no firmware needed)

Driver: seqids hook call (draws corner markers), then three MIDI messages
(note-on 60/100 -> white bar, CC 12/45 -> green block, bend -> ignored).
Check stub at the dispatcher verifies 12 things in the framebuffer + CPU state.
HLT = exit 0 = pass, ABORT = exit 1 = fail (failure bits in R7)."""

import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, run_model  # noqa: E402
from build import compile_project  # noqa: E402

PROJ = os.path.join(SDK, "examples", "fb_test")
FB, FB_W, FB_H, FB_S = 0x02000000, 160, 320, 256  # must match te246.h / fb.c
PX = lambda x, y: FB + (y * FB_S + x) * 2  # noqa: E731
STRING, SCOUNT, STACK, ZEROS = 0x01F79000, 0x01F79200, 0x01F7FFFC, 0x01F70000
V1, V2, V3 = 0x01F79040, 0x01F79050, 0x01F79060
MB1, MB2, MB3 = 0x01F79100, 0x01F79110, 0x01F79120
SEQ_AT, DISPATCH_AT, DRIVER_AT = 0x01A42344, 0x01A85C20, 0x01F00000

# what fb.c must have drawn (keep in sync with its geometry math)
NOTE, VEL, CCN = 60, 100, 12
bar_x = NOTE * FB_W // 128
bar_h = VEL * (FB_H - 16) // 127 + 1
cc_x = CCN * (FB_W - 8) // 128


def main():
    cfg = json.load(open(os.path.join(PROJ, "project.json")))
    code, syms, elf = compile_project(PROJ, cfg)
    print(f"project: {len(code)} bytes ({elf})")

    driver, dsyms = asm_bin(
        os.path.join(HERE, "driver.S"),
        DRIVER_AT,
        os.path.join(HERE, "driver"),
        {
            "STACK": STACK,
            "STRING": STRING,
            "SHOOK": syms["_hook_seqids"],
            "HOOK": syms["_hook_midi"],
            "V1": V1,
            "V2": V2,
            "V3": V3,
            "MB1": MB1,
            "MB2": MB2,
            "MB3": MB3,
        },
    )
    seqstub, _ = asm_bin(
        os.path.join(HERE, "seqstub.S"),
        SEQ_AT,
        os.path.join(HERE, "seqstub"),
        {},
        entry="_seqstub",
    )
    check, _ = asm_bin(
        os.path.join(HERE, "check_stub.S"),
        DISPATCH_AT,
        os.path.join(HERE, "check_stub"),
        {
            "STACK": STACK,
            "BACK3": dsyms["_back3"],
            "SCOUNT": SCOUNT,
            "TOTAL": syms["_midi_total"],
            "MTL": PX(0, 0),
            "MTR": PX(FB_W - 1, 0),
            "MBR": PX(FB_W - 1, FB_H - 1),
            "MBL": PX(0, FB_H - 1),
            "BART": PX(bar_x, 0),
            "BARE": PX(bar_x, bar_h - 1),
            "BARA": PX(bar_x, bar_h),
            "CCB": PX(cc_x, FB_H - 14),
        },
        entry="_dispatch",
    )

    r = run_model(
        os.path.join(HERE, "fbtest_run.elf"),
        [
            (SEQ_AT, seqstub),
            (DISPATCH_AT, check),
            (0x01E00000, code),
            (DRIVER_AT, driver),
            (ZEROS, bytes(0x10000)),
            (FB, bytes(FB_H * FB_S * 2)),
        ],
        DRIVER_AT,
    )
    print(
        "checks: 4 corner markers, note bar (top/end/below), CC block, "
        "midi_total==3, RETS/SP/FP after replayed LINK"
    )
    print(
        "PASS"
        if r.returncode == 0
        else f"FAIL (exit {r.returncode}; run in bfin-elf-gdb and read R7 for the failed checks)"
    )
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
