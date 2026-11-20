#!/usr/bin/env python3
"""Run the PLAY bench (features/play) in the Blackfin simulator.

    run_playtest.py           (composes play alone; no firmware needed)

The composed feature runs against TE stand-ins (driver.c): an in-memory yaffs, the disk-sync
cleanup call (import hook), TE's volume setup (entry hook, the mix into TE's master buffers),
keys, knobs, frames; the mix lands in TE's master bus (metronome-mix hook). Three FAT16 disk images are built here: 1 = A1.WAV (2 bars stereo) +
A2.AIF (1 bar mono, big-endian) + SET.TXT + a hidden "._" file, 2 = A1.WAV only (mono),
3 = A2.AIF only (A1 gone). Saves build/demo/play_screen*.png (the list three times, the song view playing, the chain view
during an audition, a help page, live mode with a cue pending, the songs page, its load question, the files page).
HLT = exit 0 = pass, ABORT = exit 1 = fail (R7 = failed check bits, see driver.c)."""

import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "bench", "playtest")     # build output, never in the package
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, os.path.join(SDK, "sim"))
from simtest import asm_bin, c_bin, run_model, run_gdb, composed  # noqa: E402
from build import compile_project  # noqa: E402

DRIVER_AT, STACK = 0x01F40000, 0x01F7FFFC
IMGS = [0x02500000, 0x02800000, 0x02B00000, 0x03200000]   # images 1..4 (the driver copies one
FB = [0x03000000, 0x03030000, 0x03060000, 0x030C0000, 0x03090000, 0x02E00000, 0x02E40000, 0x02E80000, 0x02EC0000, 0x02F00000, 0x02F40000, 0x02F80000]   # the 5th: also the driver's scratch frame                 # into tape_c.raw at 0x02200000)
STUBS = {                                                 # TE address: (driver function, entry bytes)
    0x01A997F4: ("file_open", 0), 0x01A99908: ("file_close", 0), 0x01A99CA8: ("file_pread", 0),
    0x01A99F84: ("file_write", 0), 0x01A9A69C: ("file_stat", 0), 0x01A9A886: ("file_mkdir", 0),
    0x01A9A3FC: ("file_unlink", 0), 0x01A9AAD0: ("file_rmdir", 0), 0x01A9B450: ("dir_open", 0),
    0x01A9B648: ("dir_read", 0), 0x01A9B74C: ("dir_close", 0), 0x01A4CCD8: ("te_snprintf", 0),
    0x01AA0318: ("heap_malloc", 0), 0x01AA0040: ("heap_free", 0), 0x01997DE4: ("te_cleanup", 0),
    0x019D2BF0: ("console_print", 0), 0x019B6C74: ("log_key", 1), 0x019B6C0C: ("log_knob", 1),
    0x019B2674: ("nop", 0), 0x0191211A: ("env_start", 0), 0x019120D0: ("env_release", 0),
    0x01912128: ("env_render", 0), 0x019C2F24: ("key_push", 0), 0x019C2F04: ("key_wake", 0),
    0x0196DD44: ("db_op1", 0), 0x019ED238: ("fs_get", 0), 0x0193EF3C: ("nop", 0),
    0x0199B2A8: ("f_open_stub", 0), 0x0199B8A8: ("f_write_stub", 0), 0x0199BD7C: ("f_close_stub", 0),
    0x0199C620: ("f_unlink_stub", 0), 0x0199C7B8: ("f_mkdir_stub", 0), 0x019964EC: ("fat_free_stub", 0),
    0x01A9B1C0: ("yaffs_free_stub", 0), 0x01998E74: ("f_mount_stub", 0),
    0x01A00D14: ("set_knobs_stub", 0), 0x01A00C98: ("set_adsr_stub", 0), 0x019FF36C: ("set_octave_stub", 0),
    0x01A1B7D4: ("te_engine_new", 0),
}
SONG_CLUSTERS = 400                                       # image 4: play_s/SONG01.PSL, room for 200 KB


# the composed feature's symbols driver.c uses (load_play.py adds its own)
NEED = ["py_main", "py_mx", "py_blocks", "py_import_files", "py_import_errors", "py_import_runs",
        "dr_pat", "dr_owned", "dr_loaded", "py_drum_hits", "py_song", "nr_pat", "nr_owned",
        "nr_loaded", "py_syn_notes", "py_cpu_trk", "py_cpu_all", "eh_register_multi", "dr_req_open", "dr_open",
        "dr_cur", "dr_via_play", "dr_ext_pat", "dr_ext_clock", "py_view", "py_pat_head", "py_pat_start_row",
        "py_pat_audition_track", "py_help_on", "py_pat_cued", "py_rd_busy", "py_pat_slot",
        "py_pat_starved", "py_update_gains", "py_pat_set_start", "py_boosts", "py_inst_cyc", "py_list_pattern",
        "nr_swing", "dr_swing", "py_pat_slot_now", "py_nrfiles", "py_export_files", "py_export_skipped",
        "py_export_songs", "py_songs_imported", "py_songs_left", "py_snd_why"]
HOOKS = {"_hook_key": "__sdk_hook_019b6c74", "_hook_knob": "__sdk_hook_019b6c0c",
         "_hook_frame": "__sdk_hook_019ab8b2", "_hook_volume": "__sdk_hook_019de548",
         "_hook_cleanup": "__sdk_hook_01997584",
         "_hook_disk_enter": "__sdk_hook_019975fa"}


def images(info=None):
    """the FAT16 disk images the driver imports (see the module doc); info: image 4's layout"""
    return [
        fat16_image([("_A1~1", "WAV", 0x22, bytes(4096)), ("A1", "WAV", 0x20, wav(176400, 2)),
                     ("A2", "AIF", 0x20, aif(88200)),
                     ("SET", "TXT", 0x20, b'bpm 120\naudio A1 "song" A2 "hats"\n')]),
        fat16_image([("A1", "WAV", 0x20, wav(88200, 1))]),
        fat16_image([("A2", "AIF", 0x20, aif(88200))]),
        fat16_image([("SONG01", "PSL", 0x20, bytes(SONG_CLUSTERS * 512))], dirname="PLAY_S", info=info),
    ]


def segments(code, syms, driver_src, out, extra_need=()):
    """the driver (driver_src, linked against the composed code's symbols), the images, frame
    buffers, TE stubs: (segs, driver symbols)"""
    info = {}
    imgs = images(info)
    defs = {"STACK": STACK, **{"_" + n: syms["_" + n] for n in list(NEED) + list(extra_need)},
            **{k: syms[v] for k, v in HOOKS.items()}, **{f"_FB{'' if i == 0 else i + 1}": fb for i, fb in enumerate(FB)}}
    for i, (at, img) in enumerate(zip(IMGS, imgs), 1):
        defs[f"_IMG{i}"] = at
        defs[f"_IMG{i}_END"] = at + len(img)
    for k, v in info.items():                             # image 4: the driver writes the file in
        defs[f"_IMG4_{k}"] = v
    driver, d = c_bin([os.path.join(HERE, "crt.S"), driver_src], DRIVER_AT, os.path.join(out, "driver"), defs)
    segs = [(0x01E00000, code), (DRIVER_AT, driver)] + [(at, img) for at, img in zip(IMGS, imgs)]
    segs += [(fb, bytes(320 * 512)) for fb in FB]
    for addr, (fn, pre) in STUBS.items():
        stub, _ = asm_bin(os.path.join(HERE, "stubs.S"), addr, os.path.join(out, f"stub_{addr:08x}"),
                          {"FN": d["_" + fn], "PRE": pre}, entry="_stub")
        segs.append((addr, stub))
    bus, _ = asm_bin(os.path.join(HERE, "te_busfn.S"), 0x019DE548, os.path.join(out, "te_busfn"), {}, entry="_stub")
    segs.append((0x019DE548, bus))
    return segs, d


def a1s(i, c):
    return ((i * (29 if c else 13)) & 0x3FFF) - 0x2000


def a2s(i):
    return ((i * 7) & 0x1FFF) - 0x1000


def wav(frames, ch):
    data = b"".join(struct.pack("<" + "h" * ch, *[a1s(i, c) for c in range(ch)]) for i in range(frames))
    fmt = struct.pack("<HHIIHH", 1, ch, 44100, 44100 * ch * 2, ch * 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


def aif(frames):                                          # mono, big-endian
    data = b"".join(struct.pack(">h", a2s(i)) for i in range(frames))
    e = 44100 .bit_length() - 1
    ext = struct.pack(">H", 16383 + e) + (44100 << (63 - e)).to_bytes(8, "big")
    comm = struct.pack(">hIh", 1, frames, 16) + ext
    body = (b"AIFF" + b"COMM" + struct.pack(">I", len(comm)) + comm
            + b"SSND" + struct.pack(">III", 8 + len(data), 0, 0) + data)
    return b"FORM" + struct.pack(">I", len(body)) + body


def fat16_image(files, size=2560 << 10, dirname="PLAY", info=None):
    """FAT16, 512-byte clusters; root: dirname/ with files [(name, ext, attr, data)]; info gets the
    first file's layout (FAT offsets, first cluster, cluster count, data and size-field offsets)"""
    tot, rsvd, nfats, rootents = size // 512, 1, 2, 512
    rootsecs, fatsz = rootents * 32 // 512, 1
    while True:
        nclus = tot - rsvd - nfats * fatsz - rootsecs
        if fatsz * 512 >= (nclus + 2) * 2:
            break
        fatsz += 1
    assert nclus >= 4085
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

    def alloc(n):
        cl = list(range(nxt[0], nxt[0] + ((n + 511) // 512 or 1)))
        nxt[0] += len(cl)
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

    lfn = bytes([0x41]) + b"x" * 10 + b"\x0f" + bytes(20)
    play = alloc(2048)
    entries = [ent(".", "", 0x10, play[0], 0), ent("..", "", 0x10, 0, 0)]
    for name, ext, attr, data in files:
        cl = alloc(len(data))
        write(cl, data)
        if info is not None and "CL" not in info:
            info.update(FAT=fat_off, FATSZ=fatsz * 512, CL=cl[0], NCL=len(cl),
                        DATA=data_off + (cl[0] - 2) * 512,
                        SIZE=data_off + (play[0] - 2) * 512 + 32 * (len(entries) + 1) + 28)
        entries += [lfn, ent(name, ext, attr, cl[0], len(data))]
    write(play, b"".join(entries))
    root = lfn + ent(dirname, "", 0x10, play[0], 0) + ent("TAPE", "", 0x10, 0, 0)
    img[root_off:root_off + len(root)] = root
    for f in range(nfats):
        for c, v in enumerate(fat):
            struct.pack_into("<H", img, fat_off + f * fatsz * 512 + 2 * c, v)
    return bytes(img)


def main():
    proj, cfg = composed(["play", "warp"], "pybench")     # warp: a multi-instance engine (keeps eh_register_multi)
    code, syms, elf = compile_project(proj, cfg)
    print(f"project: {len(code)} bytes ({elf})")
    segs, d = segments(code, syms, os.path.join(HERE, "driver.c"), OUT)
    elf_run = os.path.join(OUT, "playtest_run.elf")
    r = run_model(elf_run, segs, d["_start"], timeout=600)
    mx = syms["_py_mx"]
    vals = run_gdb(elf_run, [f"/x *(unsigned*){d['_fails']:#x}"] + [f"*(int*){d['_dbg'] + 4 * i:#x}" for i in range(7)]
                   + [f"*(unsigned*){syms['_py_blocks']:#x}"] + [f"*(int*){d['_tevdbg'] + 4 * i:#x}" for i in range(12)],
                   [("play_screen", FB[0]), ("play_screen_gone", FB[1]), ("play_screen_tracks", FB[2]),
                    ("play_screen_song", FB[3]), ("play_screen_chain", FB[4]),
                    ("play_screen_help", FB[5]), ("play_screen_live", FB[6]),
                    ("play_screen_render", FB[7]), ("play_screen_songs", FB[8]),
                    ("play_screen_songs_ask", FB[9]),
                    ("play_screen_files", FB[10]), ("play_screen_songs_sounds", FB[11])],
                   timeout=600)
    if (r.returncode or os.environ.get("PY_DBG")) and vals:
        print(f"failed check bits: {vals[0]}  first mismatch block/frame/gotL/wantL/gotR/wantR/pos: {vals[1:8]}  blocks {vals[8:9]}  tev {vals[9:]}")
    print("PASS" if r.returncode == 0 else f"FAIL (exit {r.returncode}; R7 = failed check bits)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
