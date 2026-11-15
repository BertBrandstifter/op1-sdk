#!/usr/bin/env python3
"""Start a stock OP-1 firmware in the Blackfin simulator.

The .op1's L1 blocks are encrypted; te-boot decrypts them into the chip's L1 SRAM while
loading, so the file alone cannot start in the simulator. features/l1dump gets the
plaintext out of a running device; this tool merges it with the plaintext SDRAM image and
runs the result in the simulator's operating environment (the one with cores devices: timers,
DMA, GPIO, MMU/EVT, ...).

    op1sim.py OP1_246.op1 analysis/l1dump/ [--cycles N] [--fb out.png] [--env operating]

    --l1data initial   only the LDR's L1 data blocks (the dump supplies their plaintext),
                       everything the loader would fill with zeros stays zero (default)
    --l1data dump      the whole runtime L1 data A/B snapshot from the dump
    --l1data zero      no L1 data at all
    --scratch dump|zero  L1 scratchpad from the dump or zeroed (default zero)

Requires the toolchain's simulator built with its hardware modules; see docs/toolchain.md
(macOS needs the modules.c sed fix and CFLAGS="-g -O0").

The dump is TE's firmware: keep the generated ELF and screenshots local (build/ and
analysis/ are gitignored).
"""

import argparse, json, os, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ldr  # noqa: E402
import ldr2elf  # noqa: E402
import op1pack  # noqa: E402

SDK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(SDK, "toolchain", "bfin", "bin", "bfin-elf-")
L1I_ADDR = 0xFFA00000
L1A_ADDR, L1B_ADDR, SCR_ADDR = 0xFF800000, 0xFF900000, 0xFFB00000
ENTRY = 0xFFA00000


def stock_ldr(op1, outdir):
    members = dict((m.name, data) for m, data in op1pack.read(op1))
    name = next((n for n in members if n.endswith(".ldr")), None)
    if name is None:
        sys.exit(f"{op1}: no .ldr member")
    path = os.path.join(outdir, os.path.basename(name))
    open(path, "wb").write(members[name])
    return path


def load_dump(dumpdir):
    layout = json.load(open(os.path.join(dumpdir, "layout.json")))
    regions = {}
    for r in layout:
        data = open(os.path.join(dumpdir, r["file"]), "rb").read()
        got = zlib_crc32(data)
        if r.get("crc32") and int(r["crc32"], 16) != got:
            sys.exit(f"{r['file']}: crc32 {got:08x} != {r['crc32']}")
        regions[r["addr"]] = data
    for addr in (L1I_ADDR,):
        if addr not in regions:
            sys.exit(f"{dumpdir}: no L1I region")
    return regions, layout


def zlib_crc32(data):
    import zlib

    return zlib.crc32(data) & 0xFFFFFFFF


def l1data_initial(ldr_data, base, dump):
    """decrypted LDR blocks for one L1 data region; FILL areas stay zero (boot state)"""
    blocks = ldr.parse(ldr_data)
    buf = bytearray(0x8000)
    for b in blocks:
        if b["flags"] & 0x0100 or not b["count"]:  # FILL: zeros at boot
            continue
        a = b["addr"]
        if base <= a and a + b["count"] <= base + len(buf):
            buf[a - base : a - base + b["count"]] = dump[base][
                a - base : a - base + b["count"]
            ]
    return bytes(buf)


def build_elf(op1, dumpdir, out, l1data="initial", scratch="zero"):
    regions, layout = load_dump(dumpdir)
    tmp = tempfile.mkdtemp(prefix="op1sim-")
    ldrpath = stock_ldr(op1, tmp)
    data = open(ldrpath, "rb").read()

    segs = ldr2elf.segments_from_ldr(data, skip_l1=True)
    segs.append((L1I_ADDR, regions[L1I_ADDR]))
    if l1data != "zero":
        for base in (L1A_ADDR, L1B_ADDR):
            segs.append(
                (
                    base,
                    regions[base]
                    if l1data == "dump"
                    else l1data_initial(data, base, regions),
                )
            )
    if scratch == "dump":
        segs.append((SCR_ADDR, regions[SCR_ADDR]))
    else:
        segs.append((SCR_ADDR, bytes(0x1000)))
    ldr2elf.write_elf(out, segs, ENTRY)
    return out


def sim_options(env, cycles):
    opts = ["--model", "bf524", "--environment", env]
    if cycles:
        opts += ["--watch-cycles-breakpoint", str(cycles)]
    return opts


def run(elf, env="operating", cycles=0, fb=None, timeout=300):
    script = [f"target sim {' '.join(sim_options(env, cycles))}", "load"]
    if fb:
        script += [
            "break *0x01aa1944",
            "run",
            f"dump binary memory {fb}.raw 0x02000000 0x02000000+320*256*2",
            'printf "STOP pc=%#x\\n", $pc',
        ]
    else:
        script += ["run", 'printf "STOP pc=%#x\\n", $pc']
    cmd = [BIN + "gdb", "-batch", "-nx", "-q"]
    for line in script:
        cmd += ["-ex", line]
    cmd.append(elf)
    r = subprocess.run(cmd, text=True, capture_output=True, timeout=timeout)
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("op1")
    ap.add_argument("dump")
    ap.add_argument(
        "--elf", help="where to write the merged ELF (default build/sim/te_boot.elf)"
    )
    ap.add_argument(
        "--cycles",
        type=lambda v: int(v, 0),
        default=20000000,
        help="stop after this many cycles (default 20,000,000; 0 = no limit)",
    )
    ap.add_argument(
        "--env", default="operating", choices=["operating", "user", "virtual"]
    )
    ap.add_argument(
        "--fb", help="also dump the TE framebuffer at the stop point to this .png"
    )
    ap.add_argument("--l1data", choices=["initial", "dump", "zero"], default="initial")
    ap.add_argument("--scratch", choices=["zero", "dump"], default="zero")
    ap.add_argument("--timeout", type=int, default=300)
    a = ap.parse_args()

    outdir = os.path.join(SDK, "build", "sim")
    os.makedirs(outdir, exist_ok=True)
    elf = a.elf or os.path.join(outdir, "te_boot.elf")
    build_elf(a.op1, a.dump, elf, a.l1data, a.scratch)
    print(f"elf: {elf}")
    r = run(elf, a.env, a.cycles, a.fb, a.timeout)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    if a.fb and os.path.exists(a.fb + ".raw"):
        sys.path.insert(0, os.path.join(SDK, "sim"))
        import simtest

        simtest.fb_png(open(a.fb + ".raw", "rb").read(), a.fb)
        print(f"framebuffer: {a.fb}")
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
