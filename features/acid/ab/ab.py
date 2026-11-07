"""A/B: Open303 (ref) vs ACID's ac_core (ours): per 10 ms frame level (dB) and spectral centroid.
Run via run.sh (builds both into build/acid_ab). Last result (v0.2): level 0.2 dB, centroid 0.03 oct."""
import subprocess, sys, os, numpy as np
H = os.path.dirname(os.path.abspath(__file__))
SC = [
    ("plain",      "1000 50 50 1000 0 0",  ["45:64:400:0"]),
    ("lowcut-res", "400 85 80 500 0 0",    ["45:64:400:0"]),
    ("hicut",      "2000 10 20 300 0 0",   ["45:64:400:0"]),
    ("accent",     "800 70 60 800 80 0",   ["45:127:300:0", "45:64:300:0", "45:127:300:0"]),
    ("slide",      "700 60 50 800 0 0",    ["45:64:200:0", "57:64:300:1"]),
    ("square",     "900 40 50 600 0 1",    ["45:64:400:0"]),
    ("envmax",     "314 100 100 2000 0 0", ["40:64:500:0"]),
    ("wow",        "600 90 70 400 100 0",  ["40:127:120:0"] * 5),
    ("high note",  "1200 60 50 600 0 0",   ["69:64:300:0"]),
]


def run(exe, sc, notes):
    raw = subprocess.run([os.path.join(H, exe)] + sc.split() + notes, capture_output=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(float)


def feats(x, sr):
    hop, win = sr // 100, sr // 50
    w = np.hanning(win)
    lv, ce = [], []
    for i in range(0, len(x) - win, hop):
        f = x[i:i + win]
        r = np.sqrt(np.mean(f * f)) + 1e-9
        sp = np.abs(np.fft.rfft(f * w))
        fr = np.fft.rfftfreq(win, 1 / sr)
        m = fr < 10000
        lv.append(20 * np.log10(r))
        ce.append(np.sum(fr[m] * sp[m]) / (np.sum(sp[m]) + 1e-12))
    return np.array(lv), np.array(ce)


allr, allo = [], []
res = []
for name, sc, notes in SC:
    lr, cr = feats(run("ref", sc, notes), 44100)
    lo, co = feats(run("ours", sc, notes), 88200)
    n = min(len(lr), len(lo))
    res.append((name, lr[:n], cr[:n], lo[:n], co[:n]))
off = np.median(np.concatenate([r[1] - r[3] for r in res]))      # one global level offset
print(f"global level offset ours->ref {off:+.1f} dB")
tot_l, tot_c = [], []
for name, lr, cr, lo, co in res:
    ok = lr > lr.max() - 40
    dl = np.abs(lr - (lo + off))[ok]
    dc = np.abs(np.log2((co + 1) / (cr + 1)))[ok]
    tot_l += list(dl); tot_c += list(dc)
    print(f"{name:11s} level |d| median {np.median(dl):4.1f} dB (90% {np.percentile(dl, 90):4.1f})  "
          f"centroid |d| median {np.median(dc):.2f} oct (90% {np.percentile(dc, 90):.2f})  "
          f"ref c {np.median(cr[ok]):5.0f} Hz ours {np.median(co[ok]):5.0f} Hz")
print(f"ALL: level median {np.median(tot_l):.1f} dB, centroid median {np.median(tot_c):.2f} oct")
if "-v" in sys.argv:
    for name, lr, cr, lo, co in res:
        print(name, "ref lv", np.round(lr[::3], 0)[:20], "\n      ours", np.round(lo[::3] + off, 0)[:20])
        print("   ref c", np.round(cr[::3])[:20], "\n    ours", np.round(co[::3])[:20])
