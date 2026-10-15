"""Shared helpers for the op1-sdk simulator test benches (sim/*/run_*.py)."""

import os, struct, subprocess, sys, zlib

SDK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(SDK, "tools"))
import ldr2elf  # noqa: E402
from build import BIN  # noqa: E402

FB_BYTES = 320 * 512                            # one panel buffer: 320 lines x 512 bytes (RGB565)
DEMO = os.path.join(SDK, "build", "demo")       # screenshots land here (build output, not in git)


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


def fb_png(buf, out, w=320, h=156, scale=2):
    """panel buffer bytes (RGB565, UI (x, y) at [(319 - x) * 256 + y]) -> PNG of the UI, scaled up"""
    rows = []
    for y in range(h):
        line = bytearray()
        for x in range(w):
            v = struct.unpack_from("<H", buf, ((w - 1 - x) * 256 + y) * 2)[0]
            px = bytes(((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
            line += px * scale
        rows += [b"\0" + bytes(line)] * scale

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))
    open(out, "wb").write(data)


def run_gdb(elf, prints=(), shots=(), timeout=300, regions=()):
    """run elf (written by run_model) to the end under bfin-elf-gdb's simulator, same memory map;
    prints: gdb `p` arguments, their values are returned as strings; shots: [(name, fb address)],
    each panel buffer is saved as build/demo/<name>.png (paths printed)"""
    mem = ["0xff800000,0x8000", "0xff900000,0x8000"] + [f"{a:#x},{n:#x}" for a, n in regions]
    target = "target sim --model bf524 " + " ".join("--memory-region " + m for m in mem)
    os.makedirs(DEMO, exist_ok=True)
    cmds = ["-ex", target, "-ex", "load", "-ex", "run"]
    cmds += sum((["-ex", f"p {e}"] for e in prints), [])
    raws = [(name, os.path.join(DEMO, name + ".raw")) for name, _ in shots]
    cmds += sum((["-ex", f"dump binary memory {raw} {addr:#x} {addr + FB_BYTES:#x}"]
                 for (_, raw), (_, addr) in zip(raws, shots)), [])
    out = subprocess.run([BIN + "gdb", "-batch", *cmds, elf], capture_output=True, text=True,
                         timeout=timeout).stdout.split("\n")
    for name, raw in raws:
        if os.path.exists(raw):
            png = os.path.join(DEMO, name + ".png")
            fb_png(open(raw, "rb").read(), png)
            os.remove(raw)
            print(f"screen: {os.path.relpath(png, SDK)}")
    return [line.split("=")[-1].strip() for line in out if line.startswith("$")]
