#!/usr/bin/env python3
"""Run the playprobe bench (features/playprobe) in the Blackfin simulator.

    run_pptest.py           (composes playprobe alone; no firmware needed)

The composed probe runs against TE stand-ins (driver.c): the audio callback wrap and the tone
words, the mix test, a FAT16 disk-mode image built here (play/ with A1.WAV fragmented, a mono
AIFF, a 48 kHz AIFF, a hidden "._" file, set.txt), the read test, the heap test, the key / knob
routing. Saves the panel as build/demo/playprobe_screen.png.
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed check bits, see driver.c)."""

import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "pptest")     # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, run_gdb, composed  # noqa: E402
from build import compile_project  # noqa: E402

DRIVER_AT, STACK = 0x01F40000, 0x01F7FFFC
IMG_AT, FB_AT = 0x02200000, 0x03000000
HEAP_AT, HEAP_LEN = 0x03040000, 0x00400000              # free-list nodes + the 3 MB arena
STUBS = {                                               # TE address: (driver function, entry bytes)
    0x01A997F4: ("file_open", 0), 0x01A99908: ("file_close", 0), 0x01A99CA8: ("file_pread", 0),
    0x01AA0318: ("heap_malloc", 0), 0x01AA0040: ("heap_free", 0), 0x01A9F2E2: ("heap_desc", 0),
    0x01A9F3D8: ("mem_set", 0), 0x019B6C74: ("log_key", 1), 0x019B6C0C: ("log_knob", 1),
    0x019B2674: ("nop", 0), 0x0197C870: ("nop", 0), 0x0197A826: ("nop", 0),
    0x01AA185C: ("cache_flush", 0), 0x01A9A69C: ("file_stat", 0),
}


def wav(ch, rate, frames):
    data = bytes((i * 7 + (i >> 9)) & 255 for i in range(frames * ch * 2))
    fmt = struct.pack("<HHIIHH", 1, ch, rate, rate * ch * 2, ch * 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


def aif(ch, rate, frames):
    e = rate.bit_length() - 1
    ext = struct.pack(">H", 16383 + e) + (rate << (63 - e)).to_bytes(8, "big")
    comm = struct.pack(">hIh", ch, frames, 16) + ext
    data = bytes(frames * ch * 2)
    body = (b"AIFF" + b"COMM" + struct.pack(">I", len(comm)) + comm
            + b"SSND" + struct.pack(">III", 8 + len(data), 0, 0) + data)
    return b"FORM" + struct.pack(">I", len(body)) + body


def fat16_image(size=4 << 20):
    """FAT16, 512-byte clusters, root: PLAY/; play/: '._A1.WAV' (hidden), A1.WAV (fragmented:
    runs of 3 clusters with a free one between), A2.AIF, A3.AIF, SET.TXT; long-name entries"""
    tot, rsvd, nfats, rootents = size // 512, 1, 2, 512
    rootsecs, fatsz = rootents * 32 // 512, 1
    while True:
        nclus = tot - rsvd - nfats * fatsz - rootsecs
        if fatsz * 512 >= (nclus + 2) * 2:
            break
        fatsz += 1
    img = bytearray(size)
    img[0:3] = b"\xeb\x3c\x90"
    img[3:11] = b"OP1SDK  "
    struct.pack_into("<HBHBHHBH", img, 11, 512, 1, rsvd, nfats, rootents, tot, 0xF8, fatsz)
    img[510:512] = b"\x55\xaa"
    fat_off = rsvd * 512
    root_off = fat_off + nfats * fatsz * 512
    data_off = root_off + rootsecs * 512
    fat = [0] * (nclus + 2)
    fat[0], fat[1] = 0xFFF8, 0xFFFF
    nxt = [2]

    def alloc(n, frag=False):
        cl = []
        for i in range((n + 511) // 512 or 1):
            if frag and i and i % 3 == 0:
                nxt[0] += 1                             # leave a free cluster
            cl.append(nxt[0])
            nxt[0] += 1
        for a, b in zip(cl, cl[1:]):
            fat[a] = b
        fat[cl[-1]] = 0xFFFF
        return cl

    def write(cl, data):
        for i, c in enumerate(cl):
            chunk = data[i * 512:(i + 1) * 512]
            o = data_off + (c - 2) * 512
            img[o:o + len(chunk)] = chunk

    def ent(name, ext, attr, clus, n):
        return name.ljust(8).encode() + ext.ljust(3).encode() + bytes([attr]) + bytes(14) \
            + struct.pack("<HI", clus, n)

    lfn = bytes([0x41]) + b"x" * 10 + b"\x0f" + bytes(20)  # a long-name part (skipped)
    play = alloc(512)
    entries = [ent(".", "", 0x10, play[0], 0), ent("..", "", 0x10, 0, 0)]
    for name, ext, attr, data, frag in [
            ("_A1~1", "WAV", 0x22, bytes(4096), False),
            ("A1", "WAV", 0x20, wav(2, 44100, 50000), True),
            ("A2", "AIF", 0x20, aif(1, 44100, 3000), False),
            ("A3", "AIF", 0x20, aif(2, 48000, 1000), False),
            ("SET", "TXT", 0x20, b"bpm 120\n", False)]:
        cl = alloc(len(data), frag)
        write(cl, data)
        entries += [lfn, ent(name, ext, attr, cl[0], len(data))]
    write(play, b"".join(entries))
    root = ent("OP1", "", 0x08, 0, 0) + lfn + ent("PLAY", "", 0x10, play[0], 0)
    img[root_off:root_off + len(root)] = root
    for f in range(nfats):
        for c, v in enumerate(fat):
            struct.pack_into("<H", img, fat_off + f * fatsz * 512 + 2 * c, v)
    return bytes(img)


def main():
    proj, cfg = composed(["playprobe"], "ppbench")
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    img = fat16_image()
    d_nop = DRIVER_AT + 0x70000                          # no init in this composition: a RTS there
    need = ["pp_main", "pp_te", "pp_pp", "pp_p", "pp_windows", "pp_tx_peak", "pp_rewraps",
            "pp_tone_mode", "pp_mix_n", "pp_nfiles", "pp_scan_state", "pp_speed_on", "pp_scan_err",
            "pp_mix_buf", "pp_files", "pp_a1_index", "pp_sp_reads", "pp_sp_errs", "pp_hp_list_sum",
            "pp_hp_list_big", "pp_hp_list_n", "pp_hp_big", "pp_hp_total", "pp_hp_blocks",
            "pp_hp_state", "pp_hp_list_ok"]
    driver, d = c_bin(
        [os.path.join(HERE, "crt.S"), os.path.join(HERE, "driver.c")],
        DRIVER_AT, os.path.join(OUT, "driver"),
        {"STACK": STACK, **{"_" + n: syms["_" + n] for n in need},
         "_compose_init": syms.get("_compose_init", d_nop), "_hook_key": syms["__sdk_hook_019b6c74"],
         "_hook_knob": syms["__sdk_hook_019b6c0c"], "_hook_frame": syms["__sdk_hook_019ab8b2"],
         "_FB": FB_AT, "_IMG": IMG_AT, "_IMG_END": IMG_AT + len(img)},
    )
    segs = [(0x01E00000, code), (DRIVER_AT, driver), (d_nop, b"\x10\x00"), (FB_AT, bytes(320 * 512)), (IMG_AT, img),
            (HEAP_AT, bytes(HEAP_LEN))]
    for addr, (fn, pre) in STUBS.items():
        stub, _ = asm_bin(os.path.join(HERE, "stubs.S"), addr, os.path.join(OUT, f"stub_{addr:08x}"),
                          {"FN": d["_" + fn], "PRE": pre}, entry="_stub")
        segs.append((addr, stub))
    elf_run = os.path.join(OUT, "pptest_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=300)
    vals = run_gdb(elf_run, [f"*(unsigned*){syms['_' + n]:#x}" for n in ("pp_te", "pp_pp", "pp_p")]
                   + [f"/x *(unsigned*){d['_fails']:#x}"], [("playprobe_screen", FB_AT)], timeout=300)
    if len(vals) >= 4:
        print(f"last window: TE {vals[0]}, probe {vals[1]}, period {vals[2]} cycles per block")
        if r.returncode:
            print(f"failed check bits: {vals[3]}")
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode}; R7 = failed check bits)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
