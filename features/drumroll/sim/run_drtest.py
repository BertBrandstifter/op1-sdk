#!/usr/bin/env python3
"""Run DRUM ROLL (recipe op1plus) in the Blackfin simulator against an emulated VoiceCtl + drum voice.

    run_drtest.py           (composes recipes/op1plus.json first; no firmware needed)

Checks: shift + green press opens on the drum screen (TE never sees it), green press flips the focus,
other screens keep TE's knobs; step entry; one bar at 120 BPM (four on the floor + a 3-step lane, block
exact); TONE -> note-on extra; VEL + LEN -> voice gain and gate with fade; the note-off half a step
later; LP filter per sample (sequenced, previewed and played by hand), unfiltered sample untouched;
synth playmode: no drum notes, note-offs held; tape sync + rewind; stop: every note-on got its
note-off; screen + badge draw. Prints cycles (filtered voice block, frame, tick).
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed check bits, see driver.c)."""

import json, os, struct, subprocess, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "drtest")     # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, BIN, composed  # noqa: E402
from build import compile_project  # noqa: E402

RECIPE = json.load(open(os.path.join(SDK, "recipes", "op1plus.json")))
DRIVER_AT, STACK = 0x01F00000, 0x01F7FFFC
STUBS = {0x01A85750: 0, 0x01A85840: 1, 0x01A0D2C0: 2, 0x019B2674: 2, 0x019B6C74: 3,
         0x019B6C0C: 3, 0x019789EC: 4, 0x01978926: 5}


def shot(i):
    os.makedirs(os.path.join(SDK, "build", "demo"), exist_ok=True)
    return os.path.join(SDK, "build", "demo", ("", "drumroll_step", "drumroll_sound")[i])


def png(raw, out, w=320, h=156, scale=2):
    """panel buffer (RGB565, UI (x, y) at [(319 - x) * 256 + y]) -> PNG of the UI, scaled up"""
    buf = open(raw, "rb").read()
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


def main():
    proj, cfg = composed(RECIPE["features"], RECIPE["name"])
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    hooks = {"hook_tick": "01a431aa", "hook_key": "019b6c74", "hook_knob": "019b6c0c",
             "hook_frame": "019ab8b2"}
    driver, d = c_bin(
        [os.path.join(HERE, "crt.S"), os.path.join(HERE, "driver.c")],
        DRIVER_AT, os.path.join(OUT, "driver"),
        {"STACK": STACK, "_FB": 0x03000000,
         **{s: syms[s] for s in ("_compose_init", "_dr_te_note_on", "_dr_te_render", "_dr_pat", "_dr_run",
                                 "_dr_open", "_dr_page", "_dr_sel", "_dr_cursor", "_dr_param", "_dr_cur", "_rq_state", "_dr_te_drum_pick", "_dr_roll")},
         **{"_" + k: syms[f"__sdk_hook_{v}"] for k, v in hooks.items()}},
    )
    segs = [(0x01E00000, code), (DRIVER_AT, driver), (0x03000000, bytes(3 * 320 * 512))]
    for addr, which in STUBS.items():
        stub, _ = asm_bin(
            os.path.join(HERE, "te_stubs.S"), addr, os.path.join(OUT, f"stub{addr:08x}"),
            {"WHICH": which, "EMU_ON": d["_emu_on"], "EMU_OFF": d["_emu_off"], "LOG_INPUT": d["_log_input"],
             "EMU_RENDER": d["_emu_render"], "EMU_VOICE_ON": d["_emu_voice_on"]},
            entry="_stub",
        )
        segs.append((addr, stub))
    elf_run = os.path.join(OUT, "drtest_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=600)
    out = subprocess.run(
        [BIN + "gdb", "-batch", "-ex", "target sim --model bf524 --memory-region 0xff800000,0x8000 "
         "--memory-region 0xff900000,0x8000", "-ex", "load", "-ex", "run",
         *sum((["-ex", f"p *(unsigned*){d[s]:#x}"] for s in ("_cycles_render", "_cycles_frame", "_cycles_tick", "_cycles_lp", "_cycles_plain")), []),
         "-ex", f"p/x *(unsigned*){d['_fails']:#x}",
         *sum((["-ex", f"dump binary memory {shot(i)}.raw {0x03000000 + i * 320 * 512:#x} "
                       f"{0x03000000 + (i + 1) * 320 * 512:#x}"] for i in (1, 2)), []), elf_run],
        capture_output=True, text=True, timeout=600).stdout.split("\n")
    vals = [l.split("=")[-1].strip() for l in out if l.startswith("$")]
    if len(vals) == 6:
        print(f"cycles per voice block (128 samples, incl. TE's render stub {vals[4]}): LP {vals[3]}, "
              f"LP + gain + gate {vals[0]}; DRUM ROLL screen per frame {vals[1]}; seq tick chain {vals[2]}")
        if r.returncode:
            print(f"failed check bits: {vals[5]}")
    for i in (1, 2):
        if os.path.exists(shot(i) + ".raw"):
            png(shot(i) + ".raw", shot(i) + ".png")
            os.remove(shot(i) + ".raw")
            print(f"screen: {os.path.relpath(shot(i) + '.png', SDK)}")
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode}; R7 = failed check bits)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
