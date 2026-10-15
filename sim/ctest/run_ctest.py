#!/usr/bin/env python3
"""Run the counter_c C hook in the Blackfin simulator.

    run_ctest.py            (builds examples/counter_c first; no firmware needed)

Driver (0x01f00000) builds te_string "pattern" in free SDRAM and calls the hook;
the check stub sits at the TE target address (0x01a42344). Checks: R0 (string ptr)
and P0 preserved, RETS = caller, SP restored, counter == 1, last_name == "pattern".
HLT = exit 0 = pass, ABORT = exit 1 = fail (failure bits in R7)."""

import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(SDK, "tools"))
import ldr2elf  # noqa: E402
from build import BIN, compile_project  # noqa: E402

import json

PROJ = os.path.join(SDK, "examples", "counter_c")
STRING, STACK, ZEROS = 0x01F79000, 0x01F7FFFC, 0x01F70000
CHECK_AT, DRIVER_AT = 0x01A42344, 0x01F00000


def run(*cmd):
    subprocess.run(cmd, check=True)


def asm_bin(src, addr, out, defsyms, entry="_start"):
    """assemble + link at addr + objcopy to raw binary; return (bytes, symbols)"""
    obj, elf = out + ".o", out + ".elf"
    d = sum((["--defsym", f"{k}={v:#x}"] for k, v in defsyms.items()), [])
    run(BIN + "as", "-o", obj, *d, src)
    run(BIN + "ld", "-o", elf, f"-Ttext={addr:#x}", "-e", entry, obj)
    run(BIN + "objcopy", "-O", "binary", elf, out + ".bin")
    syms = {}
    txt = subprocess.run(
        [BIN + "nm", elf], capture_output=True, text=True, check=True
    ).stdout
    for line in txt.splitlines():
        f = line.split()
        if len(f) == 3:
            syms[f[2]] = int(f[0], 16)
    return open(out + ".bin", "rb").read(), syms


def main():
    import json as _json

    cfg = _json.load(open(os.path.join(PROJ, "project.json")))
    code, syms, elf = compile_project(PROJ, cfg)
    print(f"project: {len(code)} bytes, hook at {syms['_hook_seqids']:#x} ({elf})")

    driver, dsyms = asm_bin(
        os.path.join(HERE, "driver.S"),
        DRIVER_AT,
        os.path.join(HERE, "driver"),
        {"STACK": STACK, "STRING": STRING, "HOOK": syms["_hook_seqids"]},
    )
    check, _ = asm_bin(
        os.path.join(HERE, "check_stub.S"),
        CHECK_AT,
        os.path.join(HERE, "check_stub"),
        {
            "STRING": STRING,
            "STACK": STACK,
            "BACK": dsyms["_back"],
            "CNT": syms["_lookup_count"],
            "NAME": syms["_last_name"],
        },
        entry="_check",
    )

    out = os.path.join(HERE, "ctest_run.elf")
    ldr2elf.write_elf(
        out,
        [
            (CHECK_AT, check),
            (0x01E00000, code),
            (DRIVER_AT, driver),
            (ZEROS, bytes(0x10000)),
        ],
        DRIVER_AT,
    )
    r = subprocess.run([BIN + "run", "--model", "bf524", out], timeout=30)
    print(
        "checks: R0 = string ptr, P0 preserved, RETS = caller, SP restored, "
        'counter == 1, last_name == "pattern"'
    )
    print(
        "PASS"
        if r.returncode == 0
        else f"FAIL (exit {r.returncode}; run in bfin-elf-gdb and read R7 for the failed checks)"
    )
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
