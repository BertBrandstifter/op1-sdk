#!/usr/bin/env python3
"""PLAY screens on the host (preview.c): builds it with the host compiler, draws the sample models
and writes build/demo/play_new_<name>.png (2x, like the sim bench's screens) and _1x.png.

    python3 features/play/sim/preview.py"""

import os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(SDK, "build", "demo")
TMP = os.path.join(SDK, "build", "bench", "play_preview")
W, H, STRIDE = 320, 156, 256


def main():
    from PIL import Image
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(TMP, exist_ok=True)
    exe = os.path.join(TMP, "preview")
    subprocess.run(["cc", "-O1", "-Wall", "-Wno-unused-function", "-I", os.path.join(SDK, "include"),
                    "-I", os.path.join(SDK, "features", "play"), os.path.join(HERE, "preview.c"),
                    "-o", exe], check=True)
    names = subprocess.run([exe, TMP], check=True, capture_output=True, text=True).stdout.split()
    for path in names:
        raw = open(path, "rb").read()
        px = struct.unpack("<%dH" % (len(raw) // 2), raw)
        img = Image.new("RGB", (W, H))
        for x in range(W):
            line = (W - 1 - x) * STRIDE
            for y in range(H):
                v = px[line + y]
                img.putpixel((x, y), (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63,
                                      (v & 31) * 255 // 31))
        name = os.path.splitext(os.path.basename(path))[0]
        img.resize((W * 2, H * 2), Image.NEAREST).save(os.path.join(OUT, "play_new_%s.png" % name))
        img.save(os.path.join(OUT, "play_new_%s_1x.png" % name))
        print(os.path.join(OUT, "play_new_%s.png" % name))


if __name__ == "__main__":
    sys.exit(main())
