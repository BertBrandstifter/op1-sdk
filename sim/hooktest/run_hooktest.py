#!/usr/bin/env python3
"""Run the hello hook in the Blackfin simulator: driver -> hook -> check stub at the TE target -> exit code."""
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(SDK, 'tools'))
import ldr2elf  # noqa: E402

rd = lambda p: open(os.path.join(HERE, p), 'rb').read()
segs = [(0x01a42344, rd('check_stub.bin')),
        (0x01e00000, open(os.path.join(SDK, 'examples/hello/build/hello.bin'), 'rb').read()),
        (0x01f00000, rd('driver.bin')),
        (0x01f70000, bytes(0x10000))]
elf = os.path.join(HERE, 'hooktest_run.elf')
ldr2elf.write_elf(elf, segs, 0x01f00000)
r = subprocess.run([os.path.join(SDK, 'toolchain/bfin/bin/bfin-elf-run'), '--model', 'bf524',
                    elf], timeout=30)
print('checks: R0 preserved, P0 preserved, RETS = caller, SP restored, counter == 1')
print('PASS' if r.returncode == 0 else f'FAIL (exit {r.returncode}; run in bfin-elf-gdb and read R7 for the failed checks)')
sys.exit(r.returncode)
