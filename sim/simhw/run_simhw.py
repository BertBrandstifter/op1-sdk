#!/usr/bin/env python3
"""Bench for the simulator's operating environment (the hardware tree from the toolchain build).

The operating environment only works when the simulator is linked with its module list
(`sim/bfin/modules.c`). On macOS the Makefile's old `sed` produced an empty list, so GDB
aborted (`sim_hw_parse` -> STATE_HW is NULL) and `--environment operating` was unusable.
It must also be built at -O0 (see docs/toolchain.md) or 16-bit immediate instructions
silently become no-ops.

The test program is plain bfin asm: 16-bit immediates into scratchpad, a store/load round
trip, and a read of a modeled device (the core timer). It runs under `target sim
--environment operating`, where those accesses go through the real device tree.
"""

import os, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from simtest import BIN, SDK, asm_bin, nm  # noqa: E402

OUT = os.path.join(SDK, "build", "bench", "simhw")
SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "simhw.S")
AT = 0x01F00000


def main():
    os.makedirs(OUT, exist_ok=True)
    elf = os.path.join(OUT, "simhw.elf")
    syms = {}
    subprocess.run(
        [
            BIN + "gcc",
            "-mcpu=bf524",
            "-nostdlib",
            f"-Wl,-Ttext={AT:#x}",
            "-Wl,-e,_start",
            "-Wl,--no-warn-rwx-segments",
            SRC,
            "-o",
            elf,
        ],
        check=True,
    )
    syms = __import__("simtest").nm(elf)
    end = syms["_end"]

    cmds = [
        "-batch",
        "-nx",
        "-q",
        "-ex",
        "target sim --model bf524 --environment operating",
        "-ex",
        "load",
        "-ex",
        f"break *{end:#x}",
        "-ex",
        "run",
        "-ex",
        "x/6wx 0xffb00000",
        "-ex",
        "x/1wx 0xffe02000",
        "-ex",
        "x/1hx 0xffc00610",
    ]
    r = subprocess.run(
        [BIN + "gdb", *cmds, elf], capture_output=True, text=True, timeout=120
    )
    out = r.stdout + r.stderr
    fails = 0

    def check(ok, what):
        nonlocal fails
        print(("PASS " if ok else "FAIL ") + what)
        fails += 0 if ok else 1

    lines = [ln for ln in out.splitlines() if ln.startswith("0xffb00000:")]
    words = []
    if lines:
        words = [int(w, 16) for w in lines[0].split(":", 1)[1].split()[:6]]
    check(
        words[:4] == [5, 6, 7, 0x1F00008], f"16-bit immediates round trip: {words[:4]}"
    )
    check(
        "invalid MMR" not in out and "Cannot access memory" not in out,
        "core devices mapped in operating env (EVT, GPTIMER1)",
    )
    check(
        "SIGSEGV" not in out and "Fatal signal" not in out,
        "operating env does not crash gdb",
    )
    print(f"{'PASS' if not fails else 'FAIL'} simhw ({fails} failed checks)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
