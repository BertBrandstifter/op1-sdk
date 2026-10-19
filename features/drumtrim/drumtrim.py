#!/usr/bin/env python3
"""Factory drum kits without their silent tails (no TE code; content only).

    drumtrim.py <stock op1_246.op1> [--db-dir build]
        -> build/factory_drum/<kit>.raw   each key's hit cut 64 frames after its last sample above
                                          -60 dB of the hit's own peak (those 64 frames fade out),
                                          silent gaps between hits dropped
           build/<kit>.sql                the kit's drum_presets row with the new start / end
    TE's own data: local only (build/ is never committed or packed). feature.json's content_files put
    both into the image when they exist.

Keys with a non-default playmode or reverse (the end point may be a loop point or where reverse
playback starts) are kept whole. Positions are frames x 4058 (TE's unit, sub-frame offsets kept): every key keeps its audio bit for bit
up to the cut, its start, pitch, playmode, volume and everything else; only end points move earlier,
into what was silence. Checks every key against the stock audio. Needs numpy."""

import argparse, json, os, sqlite3, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(SDK, "tools"))
import op1pack  # noqa: E402

UNIT = 4058
PAD = 64
FLOOR_DB = -60


def trim(np, x, kit):
    """x int16 kit audio, kit JSON -> (new audio, new kit JSON)"""
    keys = sorted(range(len(kit["start"])), key=lambda k: kit["start"][k])
    segs, new_start, new_end = [], list(kit["start"]), list(kit["end"])
    out_len = 0
    for k in keys:
        s, e = kit["start"][k], kit["end"][k]
        f0, f1 = s // UNIT, min(len(x) - 1, e // UNIT)
        hit = np.abs(x[f0:f1 + 1].astype(float))
        loud = np.nonzero(hit > hit.max() * 10 ** (FLOOR_DB / 20))[0] if hit.max() > 0 else []
        last = int(f0 + (loud[-1] if len(loud) else 0) + PAD)     # keep PAD frames after the last loud one
        plain = kit["playmode"][k] == 8192 and kit["reverse"][k] == 8192    # TE's defaults
        if last < f1 and plain:                                  # loop / hold / reverse keys: untouched
            e = min(e, last * UNIT + UNIT - 1)
            f1 = last
        if segs and f0 <= segs[-1][1]:                           # touches the previous hit's frames
            f0 = segs[-1][1] + 1
        shift = f0 - out_len                                     # frames removed before this hit
        segs.append((f0, f1, f1 < min(len(x) - 1, kit["end"][k] // UNIT)))
        new_start[k] = s - shift * UNIT
        new_end[k] = e - shift * UNIT
        out_len += f1 - f0 + 1
    parts = []
    for f0, f1, cut in segs:
        seg = x[f0:f1 + 1].copy()
        if cut and len(seg) > PAD:                               # fade the kept near-silence
            seg[-PAD:] = np.round(seg[-PAD:] * np.linspace(1, 0, PAD)).astype(np.int16)
        parts.append(seg)
    y = np.concatenate(parts)
    nk = dict(kit)
    nk["start"], nk["end"] = new_start, new_end
    return y, nk


def check(np, x, kit, y, nk):
    for k in range(len(kit["start"])):
        a, b = kit["start"][k] // UNIT, nk["start"][k] // UNIT
        n = nk["end"][k] // UNIT - b + 1 - PAD                   # up to the faded frames: identical
        assert n > 0 and np.array_equal(x[a:a + n], y[b:b + n]), f"{kit['name']} key {k}"
        assert nk["end"][k] <= kit["end"][k] - (kit["start"][k] - nk["start"][k])
        assert nk["start"][k] % UNIT == kit["start"][k] % UNIT and 0 <= nk["start"][k] <= nk["end"][k]
    assert max(nk["end"]) // UNIT < len(y)


def main():
    import numpy as np
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("firmware", help="stock op1_246.op1")
    ap.add_argument("--out", default=os.path.join(HERE, "build"))
    a = ap.parse_args()
    m = {info.name: data for info, data in op1pack.read(a.firmware) if data is not None}
    tmp = os.path.join(tempfile.mkdtemp(), "f.db")
    open(tmp, "wb").write(m["content/op1_factory.db"])
    os.makedirs(os.path.join(a.out, "factory_drum"), exist_ok=True)
    total = saved = 0
    for pid, patch in sqlite3.connect(tmp).execute("SELECT id, patch FROM drum_presets WHERE folder='drum'"):
        kit = json.loads(patch)
        if kit.get("type") != "drum":
            continue
        x = np.frombuffer(m[f"content/audio/factory_drum/{kit['name']}.raw"], dtype="<i2")
        y, nk = trim(np, x, kit)
        check(np, x, kit, y, nk)
        open(os.path.join(a.out, "factory_drum", kit["name"] + ".raw"), "wb").write(y.astype("<i2").tobytes())
        js = json.dumps(nk, separators=(",", ":"))
        open(os.path.join(a.out, kit["name"] + ".sql"), "w").write(
            "UPDATE drum_presets SET patch='%s' WHERE id=%d AND folder='drum' AND patch LIKE '%%\"name\":\"%s\"%%';\n"
            % (js.replace("'", "''"), pid, kit["name"]))
        total += len(x)
        saved += len(x) - len(y)
        print(f"{kit['name']:11s} {len(x) / 44100:5.2f} s -> {len(y) / 44100:5.2f} s")
    print(f"all: {total * 2 / 1048576:.2f} MB -> {(total - saved) * 2 / 1048576:.2f} MB "
          f"(-{saved * 2 / 1048576:.2f} MB, {100 * saved / total:.0f} %)")


if __name__ == "__main__":
    main()
