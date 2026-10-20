#!/usr/bin/env python3
"""Run the M1 engine (and the engine host) of recipes/op1plus.json in the Blackfin simulator.

    run_m1test.py        (composes recipes/op1plus.json first; no firmware needed)

Bench builds define SDK_BENCH, so M1 plays its built-in waves (the SoundFont data would not fit
next to the bench driver). Checks: plain drwave untouched; the host routes tag 3 to M1; pitch from
TE's per-sample buffer (SAW, 441 Hz) and note restart; knob push = page 1..4; WAVE A / B one
multisound per detent; page 1 clamp; 6-bit fields step one per detent and keep tag 3 (B PITCH limited
to +24); shift + blue = bank (8 entries in range, tag 3); page 4 = FX knobs; "station 1" -> "drwave" in the
engine list; screen draw (bank entry 2, page 1; SINE PAD on page 4); the keyboard lights a held key's
voice and clears it on release (TE keeps rendering the voice). Prints cycles per voice block.
When build/m1_data.inc exists (gen_m1.py --rom / --sf2), a second run measures the SoundFont data: m1_core.c
alone at 0x00100000 (-O2, as in the image), worst cycles per voice block over keys 36..108 (sounding) for piano,
clav, harpsichord, organ 2, strings, lore and saw, alone, layered and with every filter / level stage on.
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed check bits, see driver.c)."""

import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "m1test")     # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, run_gdb, composed  # noqa: E402
from build import compile_project  # noqa: E402

RECIPE = json.load(open(os.path.join(SDK, "recipes", "op1plus.json")))
DRIVER_AT, STACK = 0x01F00000, 0x01F7FFFC
STUBS = {0x0197C870: 0, 0x0197A826: 1, 0x01A00D14: 2, 0x019B6C74: 3, 0x019B6C0C: 4, 0x019FF664: 5, 0x019B2674: 6}


def main():
    proj, cfg = composed(RECIPE["features"], RECIPE["name"])
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    driver, d = c_bin(
        [os.path.join(HERE, "crt.S"), os.path.join(HERE, "driver.c")],
        DRIVER_AT, os.path.join(OUT, "driver"),
        {"STACK": STACK, "_eh_render_" if False else "_wh_te_render": syms["_eh_te_render"], "_wh_te_note_on": syms["_eh_te_note_on"],
         "_compose_init": syms["_compose_init"],
         "_engine_name": syms["_compose_engine_name"], "_m1_page": syms["_m1_page"], "_m1_bank": syms["_m1_bank"],
         "_hook_key": syms["__sdk_hook_019b6c74"], "_hook_frame": syms["__sdk_hook_019ab8b2"], "_FB": 0x03000000, "_hook_knob": syms["__sdk_hook_019b6c0c"]},
    )
    segs = [(0x01E00000, code), (DRIVER_AT, driver), (0x03000000, bytes(320 * 512)), (0x03040000, bytes(320 * 512)), (0x03080000, bytes(320 * 512))]
    for addr, which in STUBS.items():
        stub, _ = asm_bin(
            os.path.join(HERE, "te_stubs.S"), addr, os.path.join(OUT, f"stub{which}"),
            {"WHICH": which, "DRWAVE_RENDER": d["_drwave_render"], "DRWAVE_NOTE_ON": d["_drwave_note_on"],
             "LOG_SAVE": d["_log_save"], "LOG_FXSAVE": d["_log_fxsave"], "LOG_KEY": d["_log_key"], "LOG_KNOB": d["_log_knob"]},
            entry="_stub",
        )
        segs.append((addr, stub))
    elf_run = os.path.join(OUT, "m1test_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=300)
    vals = run_gdb(elf_run, [f"*(unsigned*){d[s]:#x}" for s in ("_cycles_plain", "_cycles_two", "_cycles_heavy")]
                   + [f"/x *(unsigned*){d['_fails']:#x}"], [("m1_screen", 0x03000000), ("m1_screen_fx", 0x03040000), ("m1_screen_own", 0x03080000)], timeout=300)
    if len(vals) == 4 and r.returncode:
        print(f"failed check bits: {vals[3]}")
    if len(vals) >= 3:
        print(f"cycles per voice block (128 samples): one wave {vals[0]}, two waves + filter {vals[1]}, "
              f"+ resonance, filter env, key track, A decay, B fade {vals[2]}")
    rc = r.returncode or rom_bench()
    print("PASS" if rc == 0 else f"FAIL (exit {rc}; R7 = failed check bits)")
    sys.exit(rc)


def rom_bench():
    if not os.path.exists(os.path.join(HERE, "..", "build", "m1_data.inc")):
        print("SoundFont data: none (gen_m1.py --rom / --sf2), skipped")
        return 0
    at = 0x00100000
    drv, d = c_bin([os.path.join(HERE, "crt.S"), os.path.join(HERE, "rom_driver.c"),
                    os.path.join(HERE, "..", "m1_core.c")], at, os.path.join(OUT, "rom"),
                   {"STACK": 0x00FFFFF0}, cflags=["-O2", "-DM1_ROM_INLINE"])
    elf_run = os.path.join(OUT, "m1rom_run.elf")
    r = run_model(elf_run, [(at, drv)], d["_start"], timeout=600)
    vals = run_gdb(elf_run, [f"*(unsigned*){d[s]:#x}" for s in ("_worst_one", "_worst_two", "_worst_heavy")],
                   timeout=600)
    if len(vals) == 3:
        print(f"SoundFont data ({len(drv)} bytes), worst cycles per voice block: one wave {vals[0]}, "
              f"two waves {vals[1]}, two waves + every stage {vals[2]}")
    return r.returncode


if __name__ == "__main__":
    main()
