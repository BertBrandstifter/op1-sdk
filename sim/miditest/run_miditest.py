#!/usr/bin/env python3
"""Run the midi_events entry hook in the Blackfin simulator.

    run_miditest.py         (builds examples/midi_events first; no firmware needed)

Driver (0x01f00000) builds three te_bytes MIDI messages and calls the hook once
per message; the check stub sits at the dispatcher address (0x01a85c20) with the
original LINK 0x10 entry, so the hook's replay path runs for real. On the third
call it verifies: R0/RETS/SP/FP state, per-type counters and last-event values.
HLT = exit 0 = pass, ABORT = exit 1 = fail (failure bits in R7)."""

import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, run_model  # noqa: E402
from build import compile_project  # noqa: E402

PROJ = os.path.join(SDK, "examples", "midi_events")
V1, V2, V3 = 0x01F79000, 0x01F79010, 0x01F79020
MB1, MB2, MB3 = 0x01F79100, 0x01F79110, 0x01F79120
SCOUNT, STACK, ZEROS = 0x01F79200, 0x01F7FFFC, 0x01F70000
DISPATCH_AT, DRIVER_AT = 0x01A85C20, 0x01F00000


def main():
    cfg = json.load(open(os.path.join(PROJ, "project.json")))
    code, syms, elf = compile_project(PROJ, cfg)
    print(f"project: {len(code)} bytes, hook at {syms['_hook_midi']:#x} ({elf})")

    driver, dsyms = asm_bin(
        os.path.join(HERE, "driver.S"),
        DRIVER_AT,
        os.path.join(HERE, "driver"),
        {
            "STACK": STACK,
            "HOOK": syms["_hook_midi"],
            "V1": V1,
            "V2": V2,
            "V3": V3,
            "MB1": MB1,
            "MB2": MB2,
            "MB3": MB3,
        },
    )
    check, _ = asm_bin(
        os.path.join(HERE, "check_stub.S"),
        DISPATCH_AT,
        os.path.join(HERE, "check_stub"),
        {
            "STACK": STACK,
            "V3": V3,
            "BACK3": dsyms["_back3"],
            "SCOUNT": SCOUNT,
            "TOTAL": syms["_midi_total"],
            "NON": syms["_midi_note_on"],
            "NOFF": syms["_midi_note_off"],
            "CCC": syms["_midi_cc"],
            "BEND": syms["_midi_bend"],
            "OTHER": syms["_midi_other"],
            "NOTE": syms["_last_note"],
            "VEL": syms["_last_velocity"],
            "CCN": syms["_last_cc_num"],
            "CCV": syms["_last_cc_val"],
            "BENDV": syms["_last_bend"],
        },
        entry="_dispatch",
    )

    r = run_model(
        os.path.join(HERE, "miditest_run.elf"),
        [
            (DISPATCH_AT, check),
            (0x01E00000, code),
            (DRIVER_AT, driver),
            (ZEROS, bytes(0x10000)),
        ],
        DRIVER_AT,
    )
    print(
        "checks: R0/RETS/SP/FP after replayed LINK, total==3, note_on/cc/bend==1, "
        "note==60 vel==100 cc==12:45 bend==0x2021"
    )
    print(
        "PASS"
        if r.returncode == 0
        else f"FAIL (exit {r.returncode}; run in bfin-elf-gdb and read R7 for the failed checks)"
    )
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
