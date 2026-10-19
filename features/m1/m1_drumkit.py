#!/usr/bin/env python3
"""STATION 1 drum kits for the OP-1's drum sampler (no firmware code).

    m1_drumkit.py "<path>/<sample set>/Drum samples" [--out build/drum]
        -> build/drum/m1 drums.aif   24 kit hits: kicks, snares, side stick, claps, hats, shaker,
                                     tambourine, toms, ride, crash, cowbell, congas, rap
           build/drum/m1 perc.aif    the 17 percussion / FX hits (timbales, whip, pole, block, finger
                                     snap, drop, vibe hit, hammer, metal hit, pluck, flexatone, wind
                                     bells, tubular, bell ring, metronomes) + 7 short kit hits, so it
                                     works on its own
           build/drum/<kit>.raw + <kit>.sql  the same as factory kits: the audio TE loads from
                                     content/audio/factory_drum/<name>.raw and the drum_presets row
                                     (folder "station 1"); feature.json's content_files put both into
                                     the image when they exist
    Not ours: build/ is committed with a NOTICE (not MIT) and never packed.

Built into the image (above), the kits are in the drum preset browser, folder station 1. The .aif
files also work on a stock OP-1: copy them into the drum folder in disk mode (e.g. drum/m1/); after
unplugging, TE imports them as drum kits ("Created drum patch."). Format as the OP-1 writes it: AIFC 'sowt' (16-bit
little-endian, mono, 44.1 kHz), chunks FVER, COMM, APPL ('op-1' + the kit JSON), SSND; key k plays
[start[k], end[k]] in frames x 4058; one kit holds at most 12 s. Each hit: tail cut at -60 dB with a
3 ms fade, levels as in the source, 64 frames of silence between hits. Needs numpy (resampling the
88.2 kHz files)."""

import argparse, glob, json, os, re, struct, sys, wave

HERE = os.path.dirname(os.path.abspath(__file__))
UNIT = 4058                             # OP-1 drum position units per frame
MAX_FRAMES = 12 * 44100
GAP = 64

KITS = {
    "m1 drums": [1, 2, 3, 8, 4, 5, 6, 7, 21, 11, 13, 12, 14, 27, 22, 9, 10, 23, 24, 15, 20, 16, 17, 25],
    "m1 perc": [1, 4, 8, 21, 11, 14, 20, 18, 19, 26, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 42, 43, 44],
}

# kit settings as in a stock OP-1 kit (values the OP-1 itself writes)
DEFAULTS = {
    "drum_version": 2, "dyna_env": [0, 8192, 0, 8192, 0, 0, 0, 0], "fx_active": False,
    "fx_params": [8000, 15216, 4064, 9968, 0, 0, 0, 0], "fx_type": "delay", "lfo_active": False,
    "lfo_params": [11264, 18663, 2080, 14944, 0, 0, 0, 0], "lfo_type": "value", "octave": 0,
}


def load_hits(folder):
    import numpy as np
    sys.path.insert(0, HERE)
    from gen_m1 import resample
    hits = {}
    for path in glob.glob(os.path.join(folder, "*.wav")):
        m = re.match(r"M01 (\d+) (.+)\.wav$", os.path.basename(path))
        if not m:
            continue
        w = wave.open(path)
        if w.getsampwidth() != 2:
            sys.exit(f"{path}: 16-bit expected")
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(float)
        if w.getnchannels() == 2:
            x = x.reshape(-1, 2).mean(axis=1)
        if w.getframerate() != 44100:
            x, _, _ = resample(np, x, 44100 / w.getframerate(), None)
        a = np.abs(x)
        nz = np.nonzero(a > a.max() * 10 ** (-60 / 20))[0]
        x = x[:nz[-1] + 1] if len(nz) else x
        f = min(132, len(x))                    # 3 ms fade
        x[-f:] *= np.linspace(1, 0, f)
        hits[int(m.group(1))] = (m.group(2), np.clip(np.round(x), -32768, 32767).astype("<i2"))
    return hits


def extended80(rate):
    """IEEE 80-bit extended of an integer sample rate (AIFF COMM)"""
    e = int(rate).bit_length()                  # rate = 1.xxx * 2^(e - 1)
    return struct.pack(">HQ", 16383 + e - 1, int(rate) << (64 - e))


def aifc(name, hits, order):
    pcm, start, end, frames = bytearray(), [], [], 0
    for n in order:
        x = hits[n][1]
        start.append(frames * UNIT)
        end.append((frames + len(x) - 1) * UNIT)
        pcm += x.tobytes() + bytes(2 * GAP)
        frames += len(x) + GAP
    if frames > MAX_FRAMES:
        sys.exit(f"{name}: {frames / 44100:.2f} s, more than the OP-1's 12 s")
    kit = dict(DEFAULTS)
    kit.update({"end": end, "name": name, "pitch": [0] * 24, "playmode": [8192] * 24, "reverse": [8192] * 24,
                "start": start, "type": "drum", "volume": [8192] * 24})
    js = b"op-1" + json.dumps({k: kit[k] for k in sorted(kit)}, separators=(",", ":")).encode() + b"\n"
    if len(js) & 1:
        js += b" "
    name_p = b"Signed integer (little-endian) linear PCM"
    comm = struct.pack(">hIh", 1, frames, 16) + extended80(44100) + b"sowt" + bytes([len(name_p)]) + name_p
    if len(comm) & 1:
        comm += b"\0"

    def chunk(cid, body):
        return cid + struct.pack(">I", len(body)) + body

    body = b"AIFC" + chunk(b"FVER", struct.pack(">I", 0xA2805140)) + chunk(b"COMM", comm) \
        + chunk(b"APPL", js) + chunk(b"SSND", struct.pack(">II", 0, 0) + bytes(pcm))
    return b"FORM" + struct.pack(">I", len(body)) + body, frames, bytes(pcm), js[4:].strip()


def check(data, order, hits):
    """parse the file back: chunk layout, JSON, every key's region = its hit"""
    assert data[:4] == b"FORM" and data[8:12] == b"AIFC"
    i, c = 12, {}
    while i < len(data):
        cid, n = data[i:i + 4], struct.unpack(">I", data[i + 4:i + 8])[0]
        c[cid] = data[i + 8:i + 8 + n]
        i += 8 + n + (n & 1)
    assert list(c) == [b"FVER", b"COMM", b"APPL", b"SSND"]
    kit = json.loads(c[b"APPL"][4:])
    pcm = c[b"SSND"][8:]
    for k, n in enumerate(order):
        a, b = kit["start"][k] // UNIT, kit["end"][k] // UNIT + 1
        assert pcm[2 * a:2 * b] == hits[n][1].tobytes(), f"key {k}"
    assert max(kit["end"]) < 2 ** 31 and len(kit["start"]) == 24


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", help="the sample set's 'Drum samples' folder (M01 NN Name.wav)")
    ap.add_argument("--out", default=os.path.join(HERE, "build", "drum"))
    a = ap.parse_args()
    hits = load_hits(a.folder)
    os.makedirs(a.out, exist_ok=True)
    for name, order in KITS.items():
        missing = [n for n in order if n not in hits]
        if missing:
            sys.exit(f"{name}: missing M01 {missing}")
        data, frames, pcm, patch = aifc(name, hits, order)
        check(data, order, hits)
        path = os.path.join(a.out, name + ".aif")
        open(path, "wb").write(data)
        open(os.path.join(a.out, name + ".raw"), "wb").write(pcm)         # factory kit: the SSND data
        open(os.path.join(a.out, name + ".sql"), "w").write(
            "INSERT INTO drum_presets (patch, folder) VALUES ('%s', 'station 1');\n"
            % patch.decode().replace("'", "''"))
        print(f"{os.path.relpath(path)}: {frames / 44100:.2f} s, {len(data) // 1024} KB, keys: "
              + ", ".join(hits[n][0] for n in order))


if __name__ == "__main__":
    main()
