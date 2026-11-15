#!/usr/bin/env python3
"""PLAY load bench (driver_load.c): three SYNTH tracks on ACID 3, a legato note every 1/16, ECO off
then on; per audio block the cycles of PLAY's hook (peak, average), the ACID renders, the blocks each
track rendered. A measurement, not a test (about 10 minutes; not run by tools/test_all.py, whose
glob takes sim/run_*.py only):

    python3 features/play/sim/load_play.py

The driver includes driver.c (its stand-ins); the segments, stubs and symbols come from
run_playtest.py, so both benches stay in step."""

import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_playtest as rp  # noqa: E402
from simtest import run_model, run_gdb, composed  # noqa: E402  (run_playtest put sim/ on the path)
from build import compile_project  # noqa: E402

OUT = os.path.join(rp.SDK, "build", "bench", "playload")      # build output, never in the package
os.makedirs(OUT, exist_ok=True)


def main():
    proj, cfg = composed(["play", "acid"], "plbench")
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    segs, d = rp.segments(code, syms, os.path.join(HERE, "driver_load.c"), OUT,
                          extra_need=["ac_bank_knobs", "py_trk_cyc", "eh_cycles", "eh_prof"])
    elf_run = os.path.join(OUT, "playload_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=1200)
    vals = run_gdb(elf_run, [f"*(unsigned*){d['_ld'] + 4 * i:#x}" for i in range(40)], [], timeout=600)
    for n, base in (("ECO off", 0), ("ECO on ", 20)):
        v = [int(x) for x in vals[base:base + 20]]
        print(f"{n}: block max {v[0]} avg {v[1]}  ACID renders {v[2]} cycles  notes {v[3]}  "
              f"rendered blocks per track {v[4:7]} cycles each {v[11:14]}  blocks over 120k {v[7]}")
    print("exit", r.returncode)
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
