#!/usr/bin/env python3
"""Run the PSS engine (and the engine host) of recipes/op1plus.json in the Blackfin simulator.

    run_psstest.py        (composes recipes/op1plus.json first; no firmware needed)

Bench builds define SDK_BENCH, so PSS plays its built-in waves (the PSS-11 ROM data would not fit
next to the bench driver). Checks: plain drwave untouched; the host routes tag 1 to PSS; pitch from
TE's per-sample buffer (SAW, 441 Hz) and note restart; knob push = page 1..4; WAVE A / B one
multisound per detent; page 1 clamp; 6-bit fields step one per detent and keep tag 1 (B PITCH limited
to +24); shift + blue = bank (8 entries in range, tag 1); page 4 = FX knobs; "pss-11" -> "drwave" in the
engine list; screen draw (bank entry 2, page 1, A4 held; SINE PAD on page 4); the lamps show a held key's
voice and clear it on release (TE keeps rendering the voice); P-- title hint; the 1 s title. Prints cycles per voice block.
When build/pss_data.inc exists (gen_pss11.py --pss-rom / --sf2), a second run measures the PSS-11 ROM data: pss_core.c
alone at 0x00100000 (-O2, as in the image), worst cycles per voice block over keys 36..108 (sounding) for piano,
clav, harpsichord, organ 2, strings, lore and saw, alone, layered and with every filter / level stage on.
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed check bits, see driver.c)."""

import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "psstest")     # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, run_gdb, composed  # noqa: E402
from build import compile_project  # noqa: E402

RECIPE = {"name": "psstest", "features": ["party"]}
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
         "_engine_name": syms["_compose_engine_name"], "_pss_page": syms["_pss_page"], "_pss_bank": syms["_pss_bank"], 
         "_pss_title_on": syms["_pss_title_on"], "_pss_note_shown": syms["_pss_note_shown"], "_pss_tick": syms["_pss_tick"],
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
    elf_run = os.path.join(OUT, "psstest_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=300)
    vals = run_gdb(elf_run, [f"*(unsigned*){d[s]:#x}" for s in ("_cycles_plain", "_cycles_two", "_cycles_heavy")]
                   + [f"/x *(unsigned*){d['_fails']:#x}"], [("pss_screen", 0x03000000), ("pss_screen_fx", 0x03040000), ("pss_screen_own", 0x03080000)], timeout=300)
    if len(vals) == 4 and r.returncode:
        print(f"failed check bits: {vals[3]}")
    if len(vals) >= 3:
        print(f"cycles per voice block (128 samples): one wave {vals[0]}, two waves + filter {vals[1]}, "
              f"+ resonance, filter env, key track, A decay, B fade {vals[2]}")
    rc = r.returncode or rom_bench()
    print("PASS" if rc == 0 else f"FAIL (exit {rc}; R7 = failed check bits)")
    sys.exit(rc)


def rom_bench(data_dir=None, what="PSS-11 ROM data"):
    """cycles with a sample data set: build/ (gen_pss11.py --pss-rom / --sf2) or data_dir"""
    extra = []
    if data_dir:
        extra = [f'-DPSS_DATA_INC="{os.path.join(data_dir, "pss_data.inc")}"',
                 f'-DPSS_ROM_INC="{os.path.join(data_dir, "pss_rom.inc")}"']
    elif not os.path.exists(os.path.join(HERE, "..", "build", "pss_data.inc")):
        print("PSS-11 ROM data: none (gen_pss11.py --pss-rom / --sf2), skipped")
        return 0
    at = 0x00100000
    drv, d = c_bin([os.path.join(HERE, "crt.S"), os.path.join(HERE, "rom_driver.c"),
                    os.path.join(HERE, "..", "pss_core.c")], at, os.path.join(OUT, "rom"),
                   {"STACK": 0x00FFFFF0}, cflags=["-O2", "-DPSS_ROM_INLINE"] + extra)
    elf_run = os.path.join(OUT, "pssrom_run.elf")
    r = run_model(elf_run, [(at, drv)], d["_start"], timeout=600)
    vals = run_gdb(elf_run, [f"*(unsigned*){d[s]:#x}" for s in ("_worst_one", "_worst_two", "_worst_heavy")],
                   timeout=600)
    if len(vals) == 3:
        print(f"{what} ({len(drv)} bytes), worst cycles per voice block: one wave {vals[0]}, "
              f"two waves {vals[1]}, two waves + every stage {vals[2]}")
    return r.returncode


if __name__ == "__main__":
    main()
