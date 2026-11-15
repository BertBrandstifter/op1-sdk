#!/usr/bin/env python3
"""Smoke bench for booting a stock OP-1 firmware in the simulator (TE-derived, local only).

Needs a firmware image and a plaintext L1 dump, which are not in git:

    OP1_FIRMWARE=/path/to/op1_246.op1 python3 sim/te_boot/run_te_boot.py

The dump is features/l1dump's output (analysis/l1dump/layout.json + bins). Without both
inputs the bench skips (exit 0), so tools/test_all.py stays green on a clean checkout.
"""

import json, os, re, subprocess, sys

sys.path.insert(
    0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)

SDK = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DUMP = os.path.join(SDK, "analysis", "l1dump")


def main():
    fw = os.environ.get("OP1_FIRMWARE")
    if (
        not fw
        or not os.path.exists(fw)
        or not os.path.exists(os.path.join(DUMP, "layout.json"))
    ):
        print(
            "SKIP te_boot: set OP1_FIRMWARE=<op1_246.op1> and run features/l1dump (analysis/l1dump)"
        )
        return 0
    r = subprocess.run(
        [
            sys.executable,
            os.path.join(SDK, "tools", "op1sim.py"),
            fw,
            DUMP,
            "--cycles",
            "5000000",
        ],
        capture_output=True,
        text=True,
        timeout=600,
    )
    out = r.stdout + r.stderr
    bad = [
        w
        for w in ("double fault", "Bus error", "Fatal signal", "invalid MMR")
        if w in out
    ]
    m = re.search(r"STOP pc=(0x[0-9a-f]+)", out)
    print(out[-2000:])
    ok = r.returncode == 0 and not bad and m and int(m.group(1), 16) != 0xFFA00000
    print(("PASS" if ok else "FAIL") + f" te_boot (pc {m.group(1) if m else '?'})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
