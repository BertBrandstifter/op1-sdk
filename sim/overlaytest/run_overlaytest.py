#!/usr/bin/env python3
"""Run features/key_probe (generated "after" call-site hook + gfx overlay + key entry hook)
in the Blackfin simulator.

    run_overlaytest.py      (builds features/key_probe first; no firmware needed)

A stub stands in for TE's frame render (0x019b2674): it scribbles over the pixel where the
overlay's red swatch goes and returns 0x5a5a. The test proves the overlay is drawn after
render (swatch survives), TE's return value is kept, the UI->panel transform puts 'K' and the
swatch where gfx.h says, and the key hook passes (id, down) through to on_key unchanged.
HLT = exit 0 = pass, ABORT = exit 1 = fail (failure bits in R7, see driver.S)."""

import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, run_model, composed  # noqa: E402
from build import compile_project  # noqa: E402

FEATURES, APP = ["key_probe"], "key_probe"   # composed by tools/compose.py
BUF, UI_W, STRIDE = 0x03000000, 320, 256  # gfx defaults when TE's canvas words read 0
PX = lambda x, y: BUF + ((UI_W - 1 - x) * STRIDE + y) * 2  # noqa: E731  (gfx.h mapping)
KCOUNT, KARGS, RCOUNT, RBUF = 0x01F79200, 0x01F79210, 0x01F79220, 0x01F79230
STACK, ZEROS, DRIVER_AT = 0x01F7FFFC, 0x01F70000, 0x01F00000
RENDER_AT, ONKEY_AT = 0x019B2674, 0x019B6C74
SWATCH = PX(2, 38)  # probe.c: 4th text row y = 2 + 3*12, red swatch at x = 2
KPIX = PX(2, 2)     # 'K' top-left pixel (glyph row 0 = 101)
BOXPIX = PX(150, 40)  # inside the black box, no text there


def main():
    proj, cfg = composed(FEATURES, APP)
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    stub, _ = asm_bin(os.path.join(HERE, "stubs.S"), RENDER_AT, os.path.join(HERE, "stubs"),
                      {"RCOUNT": RCOUNT, "RBUF": RBUF, "SWATCH": SWATCH}, entry="_render")
    kstub, _ = asm_bin(os.path.join(HERE, "keystub.S"), ONKEY_AT, os.path.join(HERE, "keystub"),
                       {"KCOUNT": KCOUNT, "KARGS": KARGS}, entry="_onkey")
    driver, _ = asm_bin(
        os.path.join(HERE, "driver.S"), DRIVER_AT, os.path.join(HERE, "driver"),
        {"STACK": STACK, "KHOOK": syms["__sdk_hook_019b6c74"], "FHOOK": syms["__sdk_hook_019ab8b2"],
         "BUF": BUF, "KCOUNT": KCOUNT, "KARGS": KARGS, "LASTKEY": syms["_last_key"],
         "RCOUNT": RCOUNT, "RBUF": RBUF, "SWATCH": SWATCH, "KPIX": KPIX, "BOXPIX": BOXPIX},
    )
    r = run_model(
        os.path.join(HERE, "overlaytest_run.elf"),
        [(RENDER_AT, stub), (ONKEY_AT, kstub), (0x01E00000, code), (DRIVER_AT, driver),
         (ZEROS, bytes(0x10000)), (BUF, bytes(UI_W * STRIDE * 2)),
         (0xFF801000, bytes(0x1000))],  # L1 data: TE canvas size words read 0 -> gfx defaults
        DRIVER_AT,
    )
    print("checks: key hook passthrough, render-then-overlay order, return value, "
          "UI->panel pixels (K, swatch, box), SP/FP, R4-R6")
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode}; read R7 in bfin-elf-gdb)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
