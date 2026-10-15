"""Shared helpers for the op1-sdk simulator test benches (sim/*/run_*.py)."""

import os, subprocess, sys

SDK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(SDK, "tools"))
import ldr2elf  # noqa: E402
from build import BIN  # noqa: E402


def nm(elf):
    syms = {}
    txt = subprocess.run(
        [BIN + "nm", elf], capture_output=True, text=True, check=True
    ).stdout
    for line in txt.splitlines():
        f = line.split()
        if len(f) == 3:
            syms[f[2]] = int(f[0], 16)
    return syms


def asm_bin(src, addr, out, defsyms, entry="_start"):
    """assemble + link at addr + objcopy to raw binary; return (bytes, symbols)"""
    obj, elf = out + ".o", out + ".elf"
    d = sum((["--defsym", f"{k}={v:#x}"] for k, v in defsyms.items()), [])
    subprocess.run([BIN + "as", "-o", obj, *d, src], check=True)
    subprocess.run(
        [BIN + "ld", "-o", elf, f"-Ttext={addr:#x}", "-e", entry, obj], check=True
    )
    subprocess.run([BIN + "objcopy", "-O", "binary", elf, out + ".bin"], check=True)
    return open(out + ".bin", "rb").read(), nm(elf)


def c_bin(srcs, addr, out, defsyms, entry="_start"):
    """compile C/asm test code (bench drivers) + link at addr + raw binary; return (bytes, symbols).
    defsyms become linker symbols, so C code can declare them extern (e.g. hook entry points)."""
    objs = []
    for src in srcs:
        obj = out + "_" + os.path.splitext(os.path.basename(src))[0] + ".o"
        subprocess.run([BIN + "gcc", "-mcpu=bf524", "-Os", "-ffreestanding", "-fno-builtin",
                        "-Wall", "-Wextra", "-I", os.path.join(SDK, "include"), "-c", src,
                        "-o", obj], check=True)
        objs.append(obj)
    d = [f"-Wl,--defsym={k}={v:#x}" for k, v in defsyms.items()]
    elf = out + ".elf"
    subprocess.run([BIN + "gcc", "-mcpu=bf524", "-nostdlib", f"-Wl,-Ttext={addr:#x}",
                    f"-Wl,-e,{entry}", "-Wl,--no-warn-rwx-segments", *d, "-o", elf, *objs,
                    "-lgcc"], check=True)
    subprocess.run([BIN + "objcopy", "-O", "binary", elf, out + ".bin"], check=True)
    return open(out + ".bin", "rb").read(), nm(elf)


def composed(features, name):
    """compose features (tools/compose.py) into build/apps/<name>; returns (project dir, project.json)"""
    from compose import compose
    out, cfg, _ = compose(features, name)
    return out, cfg


def run_model(out_elf, segs, entry_addr, timeout=30, regions=()):
    """write segs [(addr, bytes)] as one ELF and run it on the bf524 model; regions: extra
    (addr, size) memory to attach (e.g. L1 instruction SRAM for a stub of a TE function there)"""
    ldr2elf.write_elf(out_elf, segs, entry_addr)
    # the default (non-OS) environment only attaches SDRAM + scratchpad; map L1 data A/B too so
    # SDK code that reads TE state there (e.g. gfx's canvas size) runs like on the device
    l1 = ["--memory-region", "0xff800000,0x8000", "--memory-region", "0xff900000,0x8000"]
    for addr, size in regions:
        l1 += ["--memory-region", f"{addr:#x},{size:#x}"]
    return subprocess.run([BIN + "run", "--model", "bf524", *l1, out_elf], timeout=timeout)
