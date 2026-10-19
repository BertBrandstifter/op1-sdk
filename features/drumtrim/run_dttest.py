#!/usr/bin/env python3
"""drumtrim.py's trim on synthetic kits (no firmware needed): loud and quiet hits with long silent
tails, a reverse key, a silent gap, sub-frame start offsets. Checks: audio identical up to the cut,
ends only earlier, offsets kept, the quiet hit's tail (audible relative to itself) kept, the reverse
key whole, the file shorter. Exit 0 = pass."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np  # noqa: E402
import drumtrim as dt  # noqa: E402

U, rng = dt.UNIT, np.random.default_rng(1)
hits, start, end, x = [], [], [], []
spec = [(30000, 2000, 8000), (300, 3000, 6000), (20000, 1500, 5000), (25000, 1000, 9000)] * 6   # peak, sound, silence
pos = 0
for k, (peak, n, sil) in enumerate(spec):
    t = np.arange(n)
    hit = peak * np.exp(-t / (n / 6)) * np.sin(t * 0.3) + rng.normal(0, 1, n) * (peak > 1000)
    seg = np.concatenate([hit, np.zeros(sil)])
    start.append(pos * U + (k * 37) % U)                # sub-frame offsets as in TE's kits
    end.append((pos + len(seg) - 1) * U)
    x.append(seg)
    pos += len(seg) + (500 if k == 5 else 0)            # one silent gap
    if k == 5:
        x.append(np.zeros(500))
x = np.round(np.concatenate(x)).astype(np.int16)
kit = {"name": "test", "start": start, "end": end, "playmode": [8192] * 24, "reverse": [8192] * 24}
kit["reverse"][3] = 12000
y, nk = dt.trim(np, x, kit)
dt.check(np, x, kit, y, nk)
ok = len(y) < len(x) * 0.6
ok &= nk["end"][3] - nk["start"][3] == kit["end"][3] - kit["start"][3]          # reverse key whole
q = x[kit["start"][1] // U:kit["start"][1] // U + 3000]                         # the quiet hit keeps its decay
ok &= (nk["end"][1] - nk["start"][1]) // U >= np.nonzero(np.abs(q) > 300 * 10 ** (-60 / 20))[0][-1]
ok &= all(a % U == b % U for a, b in zip(kit["start"], nk["start"]))
print(f"{len(x)} -> {len(y)} frames", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
