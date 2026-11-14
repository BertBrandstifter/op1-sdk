#!/usr/bin/env python3
"""nophysics bench.

    run_nptest.py           (no firmware needed; more checks when it is there, see below)

Always:
  1. tools/ldrpatch.py span helpers on a synthetic LDR stream (data, FILL, data blocks): read across
     blocks (FILL gives its pattern), RTS fill writes only data blocks and reports the FILL bytes,
     a gap is refused, the rebuilt stream parses with valid header checksums.
  2. compose: nophysics with every feature of recipes/op1plus.json -> no hook of any feature lies in
     a blanked range; a made-up hook inside one is refused.
  3. Simulator: the RTS fill reached like TE reaches the redirected pointers (the boot loop over the
     ctor table, a vtable call with this + argument): returns, stack and callee-saved registers kept.
With the stock firmware (OP1_FIRMWARE, or ../additional/org op1 firmware/op1_246.op1 next to the SDK):
  4. the built update: every blanked byte is RTS, the 36 pointers lead to the first one,
     every patch is in, everything else of OP1_vdk.ldr's loaded memory is unchanged, lander.svg and
     tombola.svg are the stub.
  5. prove.py (needs analysis/stock_sdram.dis): nothing can reach the blanked code.
Exit 0 = pass."""

import contextlib, io, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
FEAT = os.path.dirname(HERE)
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(FEAT, "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "nptest")      # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
sys.path.insert(0, os.path.join(SDK, "tools"))
from simtest import c_bin, run_model, composed  # noqa: E402
from build import compile_project, patch_firmware, STOCK_SHA256  # noqa: E402
import compose, ldr, ldrpatch, op1pack  # noqa: E402

DRIVER_AT, STACK = 0x01F00000, 0x01F7FFFC
STUB = 0x0191FE8C                                         # where the redirected pointers lead
RTS = b"\x10\x00"
fails = []


def check(name, ok):
    print(f"  {'ok  ' if ok else 'FAIL'} {name}")
    if not ok:
        fails.append(name)


def hdr(flags, addr, count, arg=0):
    return ldrpatch.header(flags, addr, count, arg)


def test_spans():
    print("1. LDR span helpers")
    a, b = bytes(range(0x40, 0x50)), bytes(range(0x80, 0x90))
    stream = (hdr(0x4006, 0xFFA00000, 0, 0) + hdr(0x0006, 0x1000, 16) + a
              + hdr(0x0106, 0x1010, 8, 0x11223344) + hdr(0x0006, 0x1018, 16) + b
              + hdr(0x0106, 0x1028, 8, 0x55667788) + hdr(0x8006, 0xFFA00000, 0))
    p = ldrpatch.LdrPatcher(stream)
    fill = bytes.fromhex("44332211") * 2
    check("read across data / FILL / data", p.read_span(0x1008, 24) == a[8:] + fill + b[:8])
    check("FILL pattern keeps its alignment", p.read_span(0x1011, 2) == bytes.fromhex("3322"))
    kept = p.fill_span(0x1004, 0x28, RTS)              # ends inside the second FILL block
    check("a FILL block reaching outside is left, and reported", kept == 4)
    want = a[:4] + RTS * 6 + RTS * 4 + RTS * 8 + bytes.fromhex("88776655") * 2
    check("RTS over data blocks and a FILL block inside", p.read_span(0x1000, 0x30) == want)
    try:
        p.read_span(0x1030, 0x10)
        check("gap refused", False)
    except ValueError:
        check("gap refused", True)
    blocks = ldr.parse(p.build())
    check("rebuilt stream: header checksums", all(x["chk_ok"] for x in blocks))
    check("rebuilt stream: same blocks, FILL value RTS", [(x["addr"], x["count"], x["arg"] if x["flags"] & 0x100 else 0)
          for x in blocks] == [(0xFFA00000, 0, 0), (0x1000, 16, 0), (0x1010, 8, 0x00100010), (0x1018, 16, 0),
                               (0x1028, 8, 0x55667788), (0xFFA00000, 0, 0)])


def test_compose():
    print("2. compose")
    names = json.load(open(os.path.join(SDK, "recipes", "op1plus.json")))["features"]
    with contextlib.redirect_stdout(io.StringIO()):       # input-sharing notes
        order, _ = compose.resolve(names + ["nophysics"])  # exits on a collision
    check(f"nophysics + op1plus ({len(order)} features with dependencies): no hook in blanked code",
          {f["name"] for f in order} >= set(names) | {"nophysics"})
    np_ = [f for f in order if f["name"] == "nophysics"]
    check("a hook inside a blanked range is refused",
          len(compose.blank_collisions(np_, {0x01920000: "feature 'other'"})) == 1)
    check("a hook right after one is not", not compose.blank_collisions(np_, {0x0193BFF0: "feature 'other'"}))


def test_sim():
    print("3. simulator")
    rts = RTS * 64                                       # the blanked range as the build writes it
    driver, d = c_bin([os.path.join(HERE, "crt.S"), os.path.join(HERE, "driver.c")], DRIVER_AT,
                      os.path.join(OUT, "driver"), {"STACK": STACK, "_stub_addr": STUB})
    r = run_model(os.path.join(OUT, "nptest_run.elf"), [(STUB, rts), (DRIVER_AT, driver)], d["_start"])
    check(f"RTS at {STUB:#x}: 31 calls, stack, registers kept (exit {r.returncode})", r.returncode == 0)


def firmware():
    for path in (os.environ.get("OP1_FIRMWARE"),
                 os.path.join(SDK, "..", "additional", "org op1 firmware", "op1_246.op1")):
        if path and os.path.exists(path):
            import hashlib
            if hashlib.sha256(open(path, "rb").read()).hexdigest() == STOCK_SHA256:
                return path
    return None


def test_image(fw):
    print("4. built update")
    proj, cfg = composed(["nophysics"], "npbench")
    code, syms, _ = compile_project(proj, cfg)
    members = patch_firmware(fw, cfg, code, syms)
    stock = op1pack.read(fw)
    get = lambda ms, n: next(d for m, d in ms if m.name == n)       # noqa: E731
    new, old = ldrpatch.LdrPatcher(get(members, "OP1_vdk.ldr")), ldrpatch.LdrPatcher(get(stock, "OP1_vdk.ldr"))
    h = json.load(open(os.path.join(FEAT, "feature.json")))["hooks"]
    changed = set()
    ok = True
    for bl in h["blank"]:
        lo, hi = int(bl["addr"], 0), int(bl["end"], 0)
        for a, k, o, _ in new.pieces(lo, hi - lo):
            raw = new.read_span(a, k)
            ok &= raw == RTS * (k // 2)
        changed |= set(range(lo, hi))
    check(f"{len(h['blank'])} blanked ranges are RTS", ok)
    ptrs = [p for p in h["patches"] if p["bytes"] == STUB.to_bytes(4, "little").hex()]
    check(f"{len(ptrs)} pointers lead to an RTS in blanked code",
          len(ptrs) == 36 and new.read(STUB, 2) == RTS and any(int(b["addr"], 0) == STUB for b in h["blank"]))
    ok = all(new.read(int(p["addr"], 0), len(p["bytes"]) // 2).hex() == p["bytes"] for p in h["patches"])
    changed |= {int(p["addr"], 0) + i for p in h["patches"] for i in range(len(p["bytes"]) // 2)}
    check(f"{len(h['patches'])} patches in", ok)
    diff = 0
    for b in ldr.loaded(old.blocks):
        if b["flags"] & 0x0100:
            continue
        x, y = old.read_span(b["addr"], b["count"]), new.read_span(b["addr"], b["count"])
        if x != y:
            diff += sum(1 for i in range(len(x)) if x[i] != y[i] and b["addr"] + i not in changed)
    check("nothing else in TE's image changed", diff == 0)
    svg = open(os.path.join(FEAT, "stub.svg"), "rb").read()
    check("lander.svg and tombola.svg are the stub",
          get(members, "content/display/lander.svg") == svg and get(members, "content/display/tombola.svg") == svg)


def main():
    test_spans()
    test_compose()
    test_sim()
    fw = firmware()
    if fw:
        test_image(fw)
        if os.path.exists(os.path.join(SDK, "analysis", "stock_sdram.dis")):
            print("5. prove.py")
            r = subprocess.run([sys.executable, os.path.join(FEAT, "prove.py"), fw], capture_output=True, text=True)
            print("   " + r.stdout.strip().replace("\n", "\n   "))
            check("nothing reaches the blanked code", r.returncode == 0)
        else:
            print("5. skipped: no analysis/stock_sdram.dis")
    else:
        print("4./5. skipped: no stock firmware (set OP1_FIRMWARE)")
    print("PASS" if not fails else f"FAIL: {', '.join(fails)}")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
