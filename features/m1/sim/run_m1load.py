#!/usr/bin/env python3
"""Run STATION 1's sample loader (m1_main) in the Blackfin simulator.

    run_m1load.py        (needs build/m1_rom.bin: gen_m1.py --rom / --sf2; skipped without)

Builds enginehost + m1 with the SoundFont tables (not SDK_BENCH) and stubs TE's yaffs open / read /
close and heap_malloc, serving build/m1_rom.bin from memory. Checks: no M1 sound = nothing loaded;
file missing = NOFILE; sample memory full = NOMEM, retried after ~250 passes; damaged file = BAD
and silent; the good file: 64 KB per pass, then READY with the data in the heap buffer, notes
sound. Screens: build/demo/m1_load_*.png.
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed check bits, see load_driver.c)."""

import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "m1load")     # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, run_gdb, composed  # noqa: E402
from build import compile_project  # noqa: E402

ROM = os.path.join(HERE, "..", "build", "m1_rom.bin")
DRIVER_AT, STACK, FILE_AT, HEAP_AT = 0x01F00000, 0x01F7FFFC, 0x00200000, 0x00400000
STUBS = {0x01A997F4: 0, 0x01A99C90: 1, 0x01A99908: 2, 0x01AA0318: 3, 0x01AA185C: 4}


def main():
    if not os.path.exists(ROM):
        print("SoundFont data: none (gen_m1.py --rom / --sf2), skipped")
        return 0
    data = open(ROM, "rb").read()
    proj, cfg = composed(["enginehost", "m1"], "m1load")
    cfg["cflags"] = [f for f in cfg["cflags"] if f != "-DSDK_BENCH"]   # the real tables, data from the file
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    driver, d = c_bin(
        [os.path.join(HERE, "crt.S"), os.path.join(HERE, "load_driver.c")],
        DRIVER_AT, os.path.join(OUT, "driver"),
        {"STACK": STACK, "_m1_main": syms["_m1_main"], "_m1_frame": syms["_m1_frame"],
         "_m1_load": syms["_m1_load"], "_m1_note_on": syms["_m1_note_on"],
         "_m1_render": syms["_m1_render"], "_FB": 0x03000000, "_FILE_DATA": FILE_AT,
         "_HEAP_BUF": HEAP_AT, "_FILE_BYTES": len(data)},
    )
    segs = [(0x01E00000, code), (DRIVER_AT, driver), (FILE_AT, data), (HEAP_AT, bytes(len(data))),
            (0x03000000, bytes(320 * 512)), (0x03040000, bytes(320 * 512)), (0x03080000, bytes(320 * 512))]
    for addr, which in STUBS.items():
        stub, _ = asm_bin(
            os.path.join(HERE, "load_stubs.S"), addr, os.path.join(OUT, f"stub{which}"),
            {"WHICH": which, "FILE_OPEN": d["_file_open"], "FILE_READ": d["_file_read"],
             "FILE_CLOSE": d["_file_close"], "HEAP_MALLOC": d["_heap_malloc"]},
            entry="_stub",
        )
        segs.append((addr, stub))
    elf_run = os.path.join(OUT, "m1load_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=600)
    vals = run_gdb(elf_run, [f"/x *(unsigned*){d['_fails']:#x}"],
                   [("m1_load_progress", 0x03000000), ("m1_load_nomem", 0x03040000),
                    ("m1_load_ready", 0x03080000)], timeout=600)
    if vals and r.returncode:
        print(f"failed check bits: {vals[0]}")
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode}; R7 = failed check bits)")
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
