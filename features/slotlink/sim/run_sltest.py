#!/usr/bin/env python3
"""Run slotlink of recipes/op1plus.json in the Blackfin simulator.

    run_sltest.py           (composes recipes/op1plus.json first; no firmware needed)

Checks, through the generated call-site hooks (TE's directory copy first, then ours) against an
in-memory file system behind TE's file calls: missing linked slot files are made from their factory
file byte for byte (several read blocks, exactly one block, one byte); files TE copied or the user
has are never touched; a missing factory file or a failing write is counted, every descriptor closed;
a second run changes nothing; other directories do nothing.
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed check bits, see driver.c)."""

import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "sltest")     # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, run_gdb, composed  # noqa: E402
from build import compile_project  # noqa: E402

RECIPE = json.load(open(os.path.join(SDK, "recipes", "op1plus.json")))
DRIVER_AT, STACK = 0x01F00000, 0x01F7FFFC
STUBS = {0x01A997F4: 0, 0x01A99C90: 1, 0x01A99908: 2, 0x01A99F84: 3, 0x0196EB7E: 4}


def main():
    proj, cfg = composed(RECIPE["features"], "slbench")
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    driver, d = c_bin(
        [os.path.join(HERE, "crt.S"), os.path.join(HERE, "driver.c")],
        DRIVER_AT, os.path.join(OUT, "driver"),
        {"STACK": STACK, "_sl_made": syms["_sl_made"], "_sl_failed": syms["_sl_failed"],
         "_hook_synth": syms["__sdk_hook_0196f8b6"], "_hook_drum": syms["__sdk_hook_0196f8ce"]},
    )
    segs = [(0x01E00000, code), (DRIVER_AT, driver)]
    for addr, which in STUBS.items():
        stub, _ = asm_bin(
            os.path.join(HERE, "te_stubs.S"), addr, os.path.join(OUT, f"stub{which}"),
            {"WHICH": which, "FS_OPEN": d["_fs_open"], "FS_READ": d["_fs_read"], "FS_CLOSE": d["_fs_close"],
             "FS_WRITE": d["_fs_write"], "DIR_COPY": d["_dir_copy"]},
            entry="_stub",
        )
        segs.append((addr, stub))
    elf_run = os.path.join(OUT, "sltest_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=300)
    if r.returncode:
        vals = run_gdb(elf_run, [f"/x *(unsigned*){d['_fails']:#x}"] +
                       [f"*(int*){syms['_' + n]:#x}" for n in ("sl_made", "sl_failed")], timeout=300)
        print(f"failed check bits: {vals[:1]}, made / failed: {vals[1:]}")
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode}; R7 = failed check bits)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
