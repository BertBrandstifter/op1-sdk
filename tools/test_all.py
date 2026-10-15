#!/usr/bin/env python3
"""Run every host unit test (examples|features/*/test_X.c, built with its sibling X.c by the system cc)
and every simulator bench (sim/*/run_*.py, features/*/[sim/]run_*.py; OP1_SDK = the SDK root);
report PASS/FAIL. Exit 1 if any fails.

    test_all.py [-v]
"""
import glob, os, subprocess, sys, tempfile

SDK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    verbose = "-v" in sys.argv
    hosts = sorted(glob.glob(os.path.join(SDK, "examples", "*", "test_*.c"))
                   + glob.glob(os.path.join(SDK, "features", "*", "test_*.c")))
    benches = sorted(glob.glob(os.path.join(SDK, "sim", "*", "run_*.py"))
                     + glob.glob(os.path.join(SDK, "features", "*", "run_*.py"))
                     + glob.glob(os.path.join(SDK, "features", "*", "sim", "run_*.py")))
    failed = []
    tmp = tempfile.mkdtemp()
    for t in hosts + benches:
        name = os.path.relpath(t, SDK)
        if t.endswith(".c"):
            unit = os.path.join(os.path.dirname(t), os.path.basename(t)[5:])
            exe = os.path.join(tmp, os.path.basename(t)[:-2])
            r = subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-O2", "-o", exe, t, unit],
                               capture_output=True, text=True)
            if r.returncode == 0:
                r = subprocess.run([exe], capture_output=True, text=True)
        else:
            r = subprocess.run([sys.executable, t], capture_output=True, text=True,
                               env={**os.environ, "OP1_SDK": SDK})
        ok = r.returncode == 0
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
        if verbose or not ok:
            sys.stdout.write(r.stdout[-2000:] + r.stderr[-2000:])
        if not ok:
            failed.append(name)
    total = len(hosts) + len(benches)
    print(f"{total - len(failed)}/{total} tests passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
