#!/usr/bin/env python3
"""STATION 1 bank from the original Korg M1 program bank (MPC-00, Int_M1.syx).

    m1_bank.py --syx "<path>/MPC-00/Int_M1.syx" [--out m1_bank.inc]

Reads the M1's internal program dump (SysEx 4C: 100 programs, 143 bytes each) and writes the
STATION 1 bank (m1_bank.inc, included by m1.c) with each sound's original oscillator multisounds,
octave transpositions and layer levels. The sample timbre still comes from build/m1_rom.bin (the
SoundFont); this only replaces the hand-picked wave/layer settings of the 28 bank sounds.

The M1 stores a program's oscillator data at (7-in-8 unpacked) bytes 12/13 (osc 1: multisound,
octave) and 14/15 (osc 2), the oscillator levels as 16-bit values at 19-20 / 23-24 (hi byte first
in the 8-bit pairs: level = data[20] << 8 | data[19]). Octaves are signed (-1 = 0xFF). STATION 1
has a B-pitch of -24..+24 semitones and no A transpose, so a one-oscillator program an octave
down/up is played as B only (MIX = 100 %) at -12/+12; a double program is normalized so A is the
oscillator at octave 0.

Program map: STATION 1 name -> M1 program (see PROGRAM below). Names without a factory program
(BELL PAD, SQUARE) use the multisounds of two factory programs / the SQUARE WAVE multisound.
Filter, envelope and level settings that STATION 1 adds on top keep the values from v0.6.
"""

import argparse
import os
import sys

FMT = 0x4C  # all program parameter dump
OFF = -1
REC = 143

# bank name -> factory program index (None: custom, see CUSTOM)
PROGRAM = {
    "M1 PIANO": 1,  # Piano 16' (the first factory program)
    "PIANO PAD": 21,  # MagicPiano (Piano + Magic Organ)
    "UNIVERSE": 0,  # Universe (Choir + Lore)
    "HOUSE ORGAN": 17,  # Organ 2
    "MAGIC ORGAN": 70,  # MagicOrgan
    "E.PIANO": 11,  # E.Piano 1
    "DWGS EP": 91,  # E.Piano 3 (DWGS E.P.1)
    "CLAV": 61,  # Clav
    "HARPSICHORD": 31,  # Harpsicord
    "FRETLESS": 6,  # Fretless
    "E.BASS": 46,  # Slap Bass (the E.Bass multisound)
    "SYNTH BASS": 36,  # SynthBass1
    "STRINGS": 27,  # Strings
    "CHOIR": 23,  # Choir
    "BRASS": 2,  # Brass 1
    "TENOR SAX": 62,  # Tenor Sax
    "PAN FLUTE": 8,  # Pan Flute
    "KALIMBA": 25,  # Kalimba
    "MALLETS": 28,  # SynMallet
    "BELL PAD": None,  # Bell Ring + Strings
    "KOTO": 34,  # Koto Trem
    "LORE": 30,  # Lore
    "RHYTHM": 95,  # Rhythm
    "WIND BELLS": 65,  # WindBells
    "BOTTLES": 33,  # Bottles
    "SAW LEAD": 38,  # Solo Synth (SAW WAVE)
    "SQUARE": None,  # SQUARE WAVE
    "DIGI BELL": 75,  # Digi-Bells
}

# bank name -> (osc 1, octave 1, osc 2, octave 2), for names without a factory program
CUSTOM = {
    "BELL PAD": (22, 0, 31, 0),  # Bell Ring + Strings
    "SQUARE": (89, 0, OFF, 0),  # SQUARE WAVE
}

# everything STATION 1 adds: cutoff, F.ENV, F.DECAY, RES, KEY TRK, A DECAY, B FADE (percent / steps).
# The original programs' VDF / envelopes are not part of the multisound; these stay the v0.6 values.
EXTRA = {
    "M1 PIANO": (100, 0, 0, 0, 0, 0, 0),
    "PIANO PAD": (100, 0, 0, 0, 0, 0, 34),
    "UNIVERSE": (85, 20, 60, 0, 30, 0, 20),
    "HOUSE ORGAN": (100, 0, 0, 0, 0, 0, 0),
    "MAGIC ORGAN": (100, 0, 0, 0, 0, 0, 0),
    "E.PIANO": (100, 0, 0, 0, 0, 0, 0),
    "DWGS EP": (90, 0, 0, 0, 20, 28, 0),
    "CLAV": (70, 40, 30, 15, 40, 0, 0),
    "HARPSICHORD": (100, 0, 0, 0, 0, 0, 0),
    "FRETLESS": (100, 0, 0, 0, 0, 0, 0),
    "E.BASS": (65, 45, 25, 10, 30, 0, 0),
    "SYNTH BASS": (40, 60, 25, 35, 30, 0, 0),
    "STRINGS": (100, 0, 0, 0, 0, 0, 0),
    "CHOIR": (100, 0, 0, 0, 0, 0, 12),
    "BRASS": (70, 35, 40, 10, 30, 0, 0),
    "TENOR SAX": (100, 0, 0, 0, 0, 0, 0),
    "PAN FLUTE": (100, 0, 0, 0, 0, 0, 0),
    "KALIMBA": (100, 0, 0, 0, 0, 0, 0),
    "MALLETS": (100, 0, 0, 0, 0, 0, 0),
    "BELL PAD": (90, 0, 0, 0, 0, 34, 36),
    "KOTO": (100, 0, 0, 0, 0, 0, 0),
    "LORE": (100, 0, 0, 0, 0, 0, 0),
    "RHYTHM": (100, 0, 0, 0, 0, 0, 0),
    "WIND BELLS": (100, 0, 0, 0, 0, 0, 0),
    "BOTTLES": (100, 0, 0, 0, 0, 0, 0),
    "SAW LEAD": (55, 40, 45, 25, 40, 0, 0),
    "SQUARE": (60, 20, 40, 10, 40, 0, 0),
    "DIGI BELL": (100, 0, 0, 0, 0, 34, 0),
}

ORDER = [k for k in PROGRAM]


def unpack(b):
    """7-in-8: every 8th byte holds the MSBs of the next 7"""
    out = bytearray()
    for i in range(0, len(b) - 7, 8):
        msb = b[i]
        for j in range(1, 8):
            out.append((b[i + j] & 0x7F) | (((msb >> (j - 1)) & 1) << 7))
    return bytes(out)


def programs(path):
    d = open(path, "rb").read()
    i = 0
    while True:
        s = d.find(b"\xf0\x42", i)
        if s < 0:
            break
        e = d.find(b"\xf7", s)
        if d[s + 3] == 0x19 and d[s + 4] == FMT:
            u = unpack(d[s + 6 : e])
            out = []
            for n in range(100):
                r = u[n * REC : (n + 1) * REC]
                out.append(
                    {
                        "name": r[:10].decode("latin1").strip(),
                        "mode": r[10] & 3,
                        "o1": r[12],
                        "t1": r[13] - 256 if r[13] > 127 else r[13],
                        "o2": r[14],
                        "t2": r[15] - 256 if r[15] > 127 else r[15],
                        "l1": (r[20] << 8) | r[19],
                        "l2": (r[24] << 8) | r[23],
                    }
                )
            return out
        i = e + 1
    sys.exit(f"{path}: no internal program dump (SysEx 4C)")


def sound(name, p):
    """(wave A, wave B, mix %, B pitch, source) for a bank name"""
    src = p[PROGRAM[name]] if PROGRAM[name] is not None else None
    if name in CUSTOM:
        o1, t1, o2, t2 = CUSTOM[name]
        l1 = l2 = 1
    else:
        q = src
        if q["mode"] == 2:
            sys.exit(f"{name}: M1 program {q['name']} is a drum program")
        o1, t1, o2, t2 = q["o1"], q["t1"], q["o2"] if q["mode"] == 1 else OFF, q["t2"]
        l1, l2 = q["l1"], q["l2"]

    if o2 == OFF:  # one oscillator
        if t1 == 0:
            return o1, OFF, 0, 0, "A only"
        return (
            o1,
            o1,
            100,
            12 * t1,
            "B only (M1 plays it an octave %s)" % ("up" if t1 > 0 else "down"),
        )

    # two layers: A is the oscillator at octave 0, B the other one
    if t1 == 0:
        a, b, ta, tb, la, lb = o1, o2, 0, t2, l1, l2
    elif t2 == 0:
        a, b, ta, tb, la, lb = o2, o1, 0, t1, l2, l1
    else:
        a, b, ta, tb, la, lb = o1, o2, t1, t2, l1, l2
    mix = round(lb / (la + lb) * 100) if la + lb else 50
    return (
        a,
        b,
        mix,
        12 * (tb - ta),
        "layered (%s)" % (src["name"] if src else "custom"),
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--syx", required=True, help="MPC-00/Int_M1.syx")
    ap.add_argument(
        "--out",
        default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "m1_bank.inc"),
    )
    a = ap.parse_args()
    p = programs(a.syx)

    # known-good checks: these multisound numbers are fixed by the M1
    for idx, ms in (
        (1, 0),
        (2, 32),
        (27, 31),
        (23, 30),
        (17, 6),
        (70, 7),
        (11, 1),
        (91, 77),
        (61, 3),
        (31, 4),
        (6, 16),
        (46, 15),
        (36, 17),
        (95, 41),
        (65, 45),
        (33, 28),
        (38, 91),
    ):
        if p[idx]["o1"] != ms:
            sys.exit(
                f"{a.syx}: program {idx} ({p[idx]['name']}) is not multisound {ms}"
            )

    rows = []
    for name in ORDER:
        wave_a, wave_b, mix, bpitch, note = sound(name, p)
        cut, fenv, fdec, res, ktrk, adec, bfade = EXTRA[name]
        if wave_b == OFF:  # A alone: no detune of A, no B fade
            det, bfade = 0, 0
        elif mix >= 100:  # B alone: start at full level
            det, bfade = 0, 0
        elif bpitch == 0:
            det = 0
        else:
            det = 8  # a little width between the layers
        if bpitch < -24 or bpitch > 24:
            sys.exit(f"{name}: B pitch {bpitch} outside -24..+24")
        rows.append(
            (
                name,
                wave_a,
                wave_b,
                mix,
                cut,
                bpitch,
                det,
                fenv,
                fdec,
                res,
                ktrk,
                adec,
                bfade,
            )
        )
        src = p[PROGRAM[name]]["name"] if PROGRAM[name] is not None else "custom"
        print(
            f"{name:<12} {src:<11} A {wave_a:3} B {wave_b:3} mix {mix:3} B pitch {bpitch:+3}  {note}"
        )

    with open(a.out, "w") as f:
        f.write(
            "/* STATION 1 bank, generated by m1_bank.py from the Korg M1 internal program bank\n"
            " * (MPC-00/Int_M1.syx): the original oscillator multisounds, octaves and layer\n"
            " * levels. Do not edit; run the generator instead. */\n"
        )
        f.write("static const bank_entry bank_rom[] = {\n")
        for r in rows:
            n, wa, wb, mix, cut, bp, det, fe, fd, res, kt, ad, bf = r
            f.write(
                '    { "%s", %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d },\n'
                % (n, wa, wb, mix, cut, bp, det, fe, fd, res, kt, ad, bf)
            )
        f.write("};\n")
    print(f"wrote {a.out} ({len(rows)} sounds)")


if __name__ == "__main__":
    main()
