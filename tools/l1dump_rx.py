#!/usr/bin/env python3
"""Receive an l1dump image (features/l1dump) back from the OP-1.

    l1dump_rx.py mkfile 256 L1DUMP.BIN        carrier file for the USB path (256 KB)
    l1dump_rx.py extract L1DUMP.BIN outdir/   carve + verify the dump (USB path)
    l1dump_rx.py decode recording.wav outdir/ demodulate + verify the dump (audio path)

Both write l1i.bin, l1a.bin, l1b.bin, scrp.bin + layout.json (all CRC-verified) to outdir.

Audio format (see features/l1dump/l1dump.c): one symbol per 64 device samples, 8 data tones
(3 bits) at bins 2,4,...,16 of the symbol window, sync tone at bin 1. A packet is 332 symbols:
sync sync, 4 index symbols (12 bits), 6 CRC-16 symbols (18 bits, top 2 zero), 320 payload
symbols (120 bytes, MSB first); a 64-symbol leader (sync / tone 7) precedes every loop.
Packets with a bad CRC are dropped and caught on the next loop; decoding stops when every
packet of the image is present. Pure stdlib; a 10-minute recording takes about a minute."""

import binascii, json, math, os, statistics, struct, sys, wave, zlib

MAGIC = b"OP1D"
CARRIER = b"OP1DMPY1"
PAYLOAD = 120  # bytes per packet
NPKT_SYMS = 332  # 2 sync + 4 index + 6 crc + 320 payload
FILES = {"L1I0": "l1i.bin", "L1A0": "l1a.bin", "L1B0": "l1b.bin", "SCRP": "scrp.bin"}


# ---------------------------------------------------------------- image


def parse_image(data):
    """-> (regions, total) with regions [{tag, addr, len, crc32, file, data}]; raises ValueError"""
    if len(data) < 16 or data[:4] != MAGIC:
        raise ValueError(
            "not an OP1D image (bad magic - wrong file, or dump not received)"
        )
    ver, nreg = struct.unpack_from("<II", data, 4)
    if ver != 1 or not 0 < nreg <= 16:
        raise ValueError(f"bad header: version {ver}, {nreg} regions")
    regions, pos = [], 12 + 16 * nreg
    for i in range(nreg):
        tag = data[12 + 16 * i : 16 + 16 * i].decode("ascii", "replace")
        addr, ln, crc = struct.unpack_from("<III", data, 16 + 16 * i)
        pay = data[pos : pos + ln]
        if len(pay) < ln:
            raise ValueError(f"truncated: region {tag} has {len(pay)} of {ln} bytes")
        if zlib.crc32(pay) & 0xFFFFFFFF != crc:
            raise ValueError(f"region {tag}: crc32 mismatch (data corrupted)")
        regions.append(
            {
                "tag": tag,
                "addr": addr,
                "len": ln,
                "crc32": f"{crc:08x}",
                "file": FILES.get(tag, tag.lower() + ".bin"),
                "data": pay,
            }
        )
        pos += ln
    (img_crc,) = struct.unpack_from("<I", data, pos)
    if zlib.crc32(data[:pos]) & 0xFFFFFFFF != img_crc:
        raise ValueError("image crc32 mismatch")
    return regions, pos + 4


def write_outputs(regions, outdir):
    os.makedirs(outdir, exist_ok=True)
    layout = []
    for r in regions:
        with open(os.path.join(outdir, r["file"]), "wb") as f:
            f.write(r["data"])
        layout.append({k: r[k] for k in ("tag", "addr", "len", "crc32", "file")})
    with open(os.path.join(outdir, "layout.json"), "w") as f:
        json.dump(layout, f, indent=1)
    return layout


def image_packets(total):
    return (total + PAYLOAD - 1) // PAYLOAD


# ---------------------------------------------------------------- carrier (USB path)


def mkcarrier(kb):
    """carrier file: 512-byte blocks 'OP1DMPY1' + u32 index + 0xA5 fill"""
    out = bytearray()
    for i in range(kb * 2):
        out += CARRIER + struct.pack("<I", i) + b"\xa5" * 500
    return bytes(out)


def extract(data):
    """carve the image from a carrier file the Mac copied back (blocks are in file order)"""
    return parse_image(data)


# ---------------------------------------------------------------- WAV in


def read_wav(path):
    """-> (mono float samples, frame rate); PCM 8/16/24/32-bit, any channel count"""
    w = wave.open(path, "rb")
    fs, nch, sw, nf = (
        w.getframerate(),
        w.getnchannels(),
        w.getsampwidth(),
        w.getnframes(),
    )
    raw = w.readframes(nf)
    w.close()
    n = len(raw) // sw
    if sw == 1:
        vals = [b - 128 for b in raw]
    elif sw == 2:
        vals = struct.unpack(f"<{n}h", raw)
    elif sw == 3:
        vals = [
            int.from_bytes(raw[3 * i : 3 * i + 3], "little", signed=True)
            for i in range(n)
        ]
    elif sw == 4:
        vals = struct.unpack(f"<{n}i", raw)
    else:
        raise ValueError(f"unsupported sample width {sw}")
    x = [float(vals[i]) for i in range(0, n, nch)]
    peak = max(1.0, max(abs(v) for v in x))
    return [v * (0.5 / peak) for v in x], fs


def write_wav(path, samples, fs=44100):
    """mono 16-bit PCM (used by the sim bench)"""
    w = wave.open(path, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(fs)
    w.writeframes(
        struct.pack(
            f"<{len(samples)}h", *[max(-32768, min(32767, int(s))) for s in samples]
        )
    )
    w.close()


# ---------------------------------------------------------------- FSK decode

DATA_BINS = (2, 4, 6, 8, 10, 12, 14, 16)
SYNC_BIN = 1
SYM = 64  # samples per symbol after resampling


def _goertzel(x, off, n, coeffs):
    """energies at the given bin coefficients (2cos(2 pi k / n)) of the window x[off:off+n]"""
    out = []
    for c in coeffs:
        s1 = s2 = 0.0
        for i in range(off, off + n):
            s0 = x[i] + c * s1 - s2
            s2 = s1
            s1 = s0
        out.append(s2 * s2 + s1 * s1 - c * s1 * s2)
    return out


def _resample(x, step):
    """linear interpolation, step input samples per output sample"""
    n = int((len(x) - 1) / step)
    out, pos = [], 0.0
    for _ in range(n):
        i = int(pos)
        frac = pos - i
        out.append(x[i] + frac * (x[i + 1] - x[i]))
        pos += step
    return out


def find_clock(x, fs, dev_fs):
    """-> recording samples per symbol when the device runs at dev_fs"""
    return fs * SYM / dev_fs


def _classify(y, pos0=0.0, step=SYM):
    """classify symbols on a grid ('S' sync or 0..7): windows of SYM samples at pos0 + j*step"""
    coeffs = {k: 2 * math.cos(2 * math.pi * k / SYM) for k in (SYNC_BIN,) + DATA_BINS}
    out, j = [], max(0, int(-pos0 / step) + 1) if pos0 < 0 else 0
    while True:
        pos = int(round(pos0 + j * step))
        if pos < 0:
            j += 1
            continue
        if pos + SYM > len(y):
            break
        es = _goertzel(y, pos, SYM, [coeffs[k] for k in (SYNC_BIN,) + DATA_BINS])
        s_e, data = es[0], es[1:]
        peak = max(data)
        if s_e > 3 * peak:
            out.append("S")
        elif peak > 0:
            out.append(data.index(peak))
        else:
            out.append(None)
        j += 1
    return out


def _fit_clock(pts):
    """-> (pos0, samples/symbol) from (true symbol index, sample position) points: median
    positions of the first/last quartiles, so individual mis-detections don't move the fit"""
    pts = sorted(pts)
    q = max(2, len(pts) // 4)
    ja = statistics.median(j for j, _ in pts[:q])
    pa = statistics.median(p for _, p in pts[:q])
    jb = statistics.median(j for j, _ in pts[-q:])
    pb = statistics.median(p for _, p in pts[-q:])
    sp = (pb - pa) / (jb - ja) if jb > ja else float(SYM)
    return pa - sp * ja, sp


def demod(x, fs, sps):
    """samples -> {packet index: payload bytes}, stats

    Two-pass clock recovery: pass 1 classifies on the nominal grid and decodes whatever
    packets it can (each packet re-anchors at its double-sync, so packets decode wherever the
    local drift is small enough). Every decoded packet's index field gives its exact position
    in the true symbol stream (leader 64 + index * 332) against its sample position - a fit
    through those anchors is the true symbol clock, and pass 2 classifies the whole stream on
    it. Handles arbitrary linear clock error (device vs recorder crystal ppm)."""
    y = _resample(x, sps / SYM)
    syms = _classify(y)
    packets, stats, anchors = _packets(syms)
    if len(anchors) >= 2:
        seen = set()
        pts = []  # first loop only: repeats are a loop later (unknown length)
        for i, idx in anchors:
            if idx in seen:
                continue
            seen.add(idx)
            pts.append((64 + NPKT_SYMS * idx, SYM * i))
        pos0, sp = _fit_clock(pts)
        syms = _classify(y, pos0, sp)
        packets2, stats2, _ = _packets(syms)
        for idx, pay in packets.items():  # keep the better pass, merged
            packets2.setdefault(idx, pay)
        if len(packets2) >= len(packets):
            stats2["ok"] += stats["ok"]
            packets, stats = packets2, stats2
    stats["symbols"] = len(syms)
    return packets, stats


def _packets(syms):
    """classified symbols -> ({idx: payload}, stats, anchors [(symbol, packet idx)])"""
    packets, stats, anchors = {}, {"ok": 0, "bad": 0, "resync": 0}, []
    i = 0
    while i + NPKT_SYMS <= len(syms):
        if syms[i] != "S" or syms[i + 1] != "S":
            i += 1
            continue
        hdr = syms[i + 2 : i + 12]
        if None in hdr or "S" in hdr or hdr[0] > 1:  # crc's top 2 bits are zero
            stats["bad"] += 1
            i += 1
            continue
        idx = (hdr[0] << 9) | (hdr[1] << 6) | (hdr[2] << 3) | hdr[3]
        crc = (
            (hdr[4] << 15)
            | (hdr[5] << 12)
            | (hdr[6] << 9)
            | (hdr[7] << 6)
            | (hdr[8] << 3)
            | hdr[9]
        )
        data_syms = syms[i + 12 : i + NPKT_SYMS]
        if None in data_syms or "S" in data_syms:
            stats["bad"] += 1
            i += 2
            stats["resync"] += 1
            continue
        bits = 0
        pay = bytearray()
        for j in range(0, 320, 8):  # 8 symbols = 3 bytes, MSB first
            v = 0
            for s in data_syms[j : j + 8]:
                v = (v << 3) | s
            pay += bytes(((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF))
            bits += 24
        got = binascii.crc_hqx(struct.pack(">H", idx) + bytes(pay), 0xFFFF)
        if got == crc:
            if idx not in packets:
                packets[idx] = bytes(pay)
            stats["ok"] += 1
            anchors.append((i, idx))
        else:
            stats["bad"] += 1
        i += NPKT_SYMS
    return packets, stats, anchors


def assemble(packets):
    """-> (image bytes, missing indices); needs packet 0 for the header"""
    if 0 not in packets:
        raise ValueError(
            "packet 0 (image header) not received - record longer / louder"
        )
    pay0 = packets[0]
    if pay0[:4] != MAGIC:
        raise ValueError("packet 0 has no OP1D magic (bad symbol lock)")
    _, nreg = struct.unpack_from("<II", pay0, 4)
    # total size = header + sum(region lens) + 4; read lens from the table
    lens = [struct.unpack_from("<I", pay0, 12 + 16 * i + 8)[0] for i in range(nreg)]
    total = 12 + 16 * nreg + sum(lens) + 4
    need = image_packets(total)
    img = bytearray()
    missing = []
    for i in range(need):
        if i in packets:
            img += packets[i]
        else:
            img += b"\0" * PAYLOAD
            missing.append(i)
    return bytes(img[:total]), missing


def decode(path, progress=True):
    """full pipeline: WAV -> (regions, stats)"""
    x, fs = read_wav(path)
    return decode_samples(x, fs, progress)


def decode_samples(x, fs, progress=True):
    """samples -> (regions, stats)

    The device rate is not measured from the audio (the sync bursts are too short for a clean
    tone estimate): try the plausible device clocks and let the packet CRCs decide - a wrong
    clock decodes to noise and fails every CRC. Recording-rate differences are absorbed by the
    resample, crystal ppm by the packet scanner's re-anchoring at every double-sync."""
    best = None
    for dev_fs in (44100, 48000):
        sps = find_clock(x, fs, dev_fs)
        packets, stats = demod(x, fs, sps)
        if progress:
            print(
                f"device {dev_fs} Hz: {stats['ok']} packets ok, {stats['bad']} dropped"
            )
        if best is None or stats["ok"] > best[1]["ok"]:
            best = (packets, stats, dev_fs)
        if stats["ok"] and 0 in packets:  # decodes clean: stop early
            try:
                assemble(packets)
                break
            except ValueError:
                pass
    packets, stats, dev_fs = best
    if not stats["ok"]:
        raise ValueError(
            "no packet passed its CRC at any device clock (is this an l1dump recording?)"
        )
    stats["dev_fs"] = dev_fs
    img, missing = assemble(packets)
    stats["missing"] = len(missing)
    if missing:
        raise ValueError(
            f"{len(missing)} packets missing ({missing[:10]}{'...' if len(missing) > 10 else ''}) "
            f"- keep the recording running for another loop"
        )
    regions, total = parse_image(img)
    stats["total"] = total
    return regions, stats


# ---------------------------------------------------------------- cli


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("mkfile", "extract", "decode"):
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == "mkfile":
        kb, out = int(sys.argv[2]), sys.argv[3]
        open(out, "wb").write(mkcarrier(kb))
        print(
            f"{out}: {kb} KB carrier ({kb * 2} blocks); copy it to the OP-1's disk root"
        )
        return
    outdir = sys.argv[3]
    if cmd == "extract":
        regions, total = extract(open(sys.argv[2], "rb").read())
        stats = {"ok": image_packets(total), "bad": 0}
    else:
        regions, stats = decode(sys.argv[2])
    layout = write_outputs(regions, outdir)
    for r in layout:
        print(
            f"{outdir}/{r['file']:<10} {r['len']:>7,} bytes  {r['tag']}  @ {r['addr']:#010x}  crc32 {r['crc32']}"
        )
    print(
        f"OK: {len(layout)} regions, {stats.get('ok', 0)} packets good"
        + (f", {stats.get('bad', 0)} dropped (crc/symbols)" if stats.get("bad") else "")
    )


if __name__ == "__main__":
    main()
