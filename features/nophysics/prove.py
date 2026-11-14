#!/usr/bin/env python3
"""Prove that nothing in R246 can reach the code nophysics blanks, once its pointers / patches are in.

    prove.py <stock op1_246.op1> [analysis dir]      (default: <sdk>/analysis)

Needs the local disassembly (analysis/stock_sdram.dis, optional stock_l1i.dis: see docs/internals.md;
never committed, it is TE code). Reads feature.json for the blank ranges, pointers and patches.

Method (greatest fixpoint): the candidate code (Box2D, the Lander and Tombola regions) is cut into
chunks at instruction boundaries nothing can fall through (previous instruction RTS / RTI / JUMP),
every chunk starts out dead; a chunk becomes live when anything outside the dead set refers to any
byte of it: a CALL / JUMP, a constant loaded with X.H (full value), or a 32-bit word anywhere in the
loaded image (SDRAM data, rodata, L1 data blocks). References removed by feature.json do not count:
redirected pointers (they now lead to the first RTS of the blanked range, checked by the bench),
the switch-table entry, the code a patched JUMP.S skips. Repeat until stable;
every blanked range must be inside dead chunks.

Assumptions, each listed below with its evidence: words that only look like code addresses (number
tables, instruction bytes); Box2D's own vtables and switch tables (only Box2D code loads them); TE's
destructor-at-exit records (run by exit(), which the OP-1 never reaches; with the RTS fill a call
there would just return)."""

import bisect, collections, json, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(SDK, "tools"))
import ldr, ldrpatch, op1pack  # noqa: E402

CODE = (0x0191209E, 0x01AA8000)          # SDRAM code (before: ctor table, rodata, vtables)
CANDIDATES = [(0x019C3000, 0x019C9A00),  # LanderScreen.cpp and its neighbours
              (0x01A63E00, 0x01A668F4)]  # Tombola*.cpp and neighbours
BOX2D = (0x0191FE8C, 0x0193BFF0)         # ext/Box2D (b2BlockAllocator .. b2World), blanked
BOX2D_ALL = (0x0191FE8C, 0x0193C12C)     # + b2WorldCallbacks.cpp, kept (TombolaScreen's boot init uses it)
# words in data that are not pointers although their value falls into blanked code: 16-bit number
# tables (0x0192xxxx = two halfwords), float tables, instruction bytes, unaligned hits
NOT_POINTERS = {0x018B0942, 0x018B66C6, 0x018BA646, 0x018D1604, 0x018D17F8, 0x018DB832, 0x018EB014,
                0x018EB02E, 0x018EB0DA, 0x018EB188, 0x018EB2E6, 0x018ECD42, 0x018ECE10, 0x018ECF2C,
                0x018ECF98, 0x018ECF9C, 0x018ECFAA, 0x019B2206, 0x01A214C4, 0x01A22BB6,
                0xFF90141E, 0xFF905B14}
# Box2D's vtables and switch tables: every code constant pointing at them is inside Box2D (checked)
BOX2D_TABLES = [(0x0190EB00, 0x0190F178), (0x018C58C0, 0x018C5940)]
# destructor-at-exit records {next, object, destructor} registered by 0x01AA2B14 (list head
# 0x018C271C); run only by exit()
EXIT_RECORDS = [(0x018C18F0, 0x018C1980), (0xFF907930, 0xFF907AB0), (0xFF9003D4, 0xFF9003DC)]
# switch tables / vtables that belong to candidate code: dead when only dead code refers to them
TABLES = [(0x018E1078, 0x018E1088),      # switch in 0x019C5E2C (Lander)
          (0x018E1144, 0x018E1164),      # switch in 0x019C8D38 (Lander draw)
          (0x018FDDA4, 0x018FDDB8),      # switch in 0x01A63E50 (Tombola knobs)
          (0x01911E0C, 0x01911E44)]      # TombolaPlayer vtable (only its constructor loads it)
SWITCH_CASES = {0x018F2F00: 0x01A0D150}  # patched switch entry -> end of the case block it led to

LINE = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$')
BRANCH = re.compile(r'(CALL|JUMP(?:\.[SL])?) 0x0x([0-9a-f]+)')
HALF = re.compile(r'\b([PRIBLM][0-7])\.([LH]) = (0x[0-9a-f]+);')
PAIR = 10
# X.H without an X.L nearby whose 64 KB window touches blanked code. None in SDRAM. In the L1 dump
# (analysis/stock_l1i.dis) some 8-byte blocks did not decrypt, and they hide the low half of
# "P1.L = ?; P1.H = x; CALL (P1)" (the calls from L1 into SDRAM). The real targets of these 10 are
# not in the dump; why each is taken not to be blanked code (a call there would return at once, RTS):
_ASSERT = "0x191: 23 of the 29 decoded L1 loads of P1.H = 0x191 are 0x01919aa8 (assert printer)"
WINDOWS_CHECKED = {
    0xFFA00DD0: _ASSERT + "; between Core.cpp assert messages",
    0xFFA02720: _ASSERT + "; next to assert message R0 = 0x018d2db4",
    0xFFA055D2: _ASSERT,
    0xFFA05724: _ASSERT + "; or P1.L = 0x3fd8 (bytes 09 e1 d8 3f at 0xffa05720): 0x01913fd8, a "
                          "function with no SDRAM caller",
    0xFFA086D8: _ASSERT,
    0xFFA08D50: _ASSERT,
    0xFFA04ADA: "same L1 function as the decoded call to 0x019c2f04 (0xffa04a32), outside",
    0xFFA04B6A: "same L1 function as the decoded call to 0x019c2f04 (0xffa04a32), outside",
    0xFFA02360: "0x1a6: decoded L1 calls there go to 0x01a60512..0x01a69238 (none into Tombola code)",
    0xFFA03ED8: "0x1a6: decoded L1 calls there go to 0x01a60512..0x01a69238 (none into Tombola code)",
}
TERM = re.compile(r'^(RTS|RTI|RTX|RTN|JUMP \(P[0-5]\)|JUMP(\.[SL])? 0x)')


def memory(ldr_bytes):
    """every loaded SDRAM / L1 data byte: {block start: bytes}"""
    p = ldrpatch.LdrPatcher(ldr_bytes)
    out = {}
    for b in ldr.loaded(p.blocks):
        if 0xFFA00000 <= b['addr'] < 0xFFB00000:
            continue                                     # L1 instruction: handled as code below
        out[b['addr']] = p.read_span(b['addr'], b['count'])
    return p, out


def disasm(path, lo=0, hi=1 << 32):
    """(instructions, references). A 32-bit constant is X.H = hi plus an X.L = lo of the same
    register within PAIR instructions either side (every such pairing counts); an X.H with no X.L
    near it is returned in `windows` (it could be any address hi:0000..hi:ffff)"""
    insns, refs, windows = [], [], []
    for l in open(path):
        m = LINE.match(l)
        if m:
            insns.append((int(m.group(1), 16), m.group(3)))
    halves = [HALF.findall(t) for a, t in insns]
    for i, (a, t) in enumerate(insns):
        if not lo <= a < hi:
            continue
        for k, v in BRANCH.findall(t):
            refs.append((int(v, 16), a, k))
        for reg, part, v in halves[i]:
            if part != 'H':
                continue
            lows = {int(v2, 16) for j in range(max(0, i - PAIR), min(len(insns), i + PAIR + 1))
                    for r2, p2, v2 in halves[j] if r2 == reg and p2 == 'L'}
            h = int(v, 16) << 16
            refs += [(h | x, a, 'const') for x in lows]
            if not lows:
                windows.append((h, a))
    return insns, refs, windows


def jump_s(addr, raw):
    """target of a JUMP.S encoding, or None"""
    w = int.from_bytes(raw[:2], 'little')
    if w >> 12 != 2:
        return None
    off = w & 0xFFF
    off -= 0x1000 if off & 0x800 else 0
    return addr + 2 * off


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    ana = sys.argv[2] if len(sys.argv) > 2 else os.path.join(SDK, "analysis")
    feat = json.load(open(os.path.join(HERE, "feature.json")))
    hooks = feat["hooks"]
    blanks = [(int(b["addr"], 0), int(b["end"], 0)) for b in hooks["blank"]]
    neut_words = {int(p["addr"], 0) for p in hooks.get("pointers", []) + hooks["patches"]}
    neut_code = []
    for pa in hooks["patches"]:
        a = int(pa["addr"], 0)
        if a in SWITCH_CASES:
            neut_words.add(a)
            neut_code.append((int.from_bytes(bytes.fromhex(pa["expect"]), 'little'), SWITCH_CASES[a]))
        t = jump_s(a, bytes.fromhex(pa["bytes"]))
        if t:
            neut_code.append((a + 2, t))
    members = op1pack.read(sys.argv[1])
    p, mem = memory(next(d for m, d in members if m.name == "OP1_vdk.ldr"))
    insns, xrefs, windows = disasm(os.path.join(ana, "stock_sdram.dis"), *CODE)
    idx = {a: i for i, (a, _) in enumerate(insns)}
    refs = [(t, a, k) for t, a, k in xrefs if not any(lo <= a < hi for lo, hi in neut_code)]

    def in_code(v):
        return CODE[0] <= v < CODE[1]
    for base, raw in mem.items():                        # every 32-bit word of the loaded image
        for o in range(0, len(raw) - 3, 2):
            v = struct.unpack_from('<I', raw, o)[0]
            at = base + o
            if not in_code(v) or at in neut_words or at in NOT_POINTERS:
                continue
            if any(lo <= at < hi for lo, hi in EXIT_RECORDS + BOX2D_TABLES):
                continue
            refs.append((v, at, 'data'))
    l1 = os.path.join(ana, "stock_l1i.dis")
    if os.path.exists(l1):
        _, r1, w1 = disasm(l1)
        refs += r1
        windows += w1
    else:
        print("note: no stock_l1i.dis, L1 code not checked")
    for h, a in windows:                                 # X.H with no X.L: could be any hi:xxxx
        if any(lo < h + 0x10000 and h < hi for lo, hi in blanks) and a not in WINDOWS_CHECKED:
            if not any(lo <= a < hi for lo, hi in blanks):
                sys.exit(f"FAIL: {a:#x} loads {h >> 16:#x}xxxx with no low half nearby; check by hand")
    for lo, hi in BOX2D_TABLES:                          # those tables: only Box2D code loads them
        users = {a for t, a, k in xrefs if lo <= t < hi and a >= CODE[0]}
        if not all(BOX2D_ALL[0] <= a < BOX2D_ALL[1] for a in users):
            sys.exit(f"FAIL: table {lo:#x} used outside Box2D: {sorted(map(hex, users))}")

    calls = {t for t, a, k in xrefs if k == 'CALL'} | {a for a, t in insns if t.startswith('LINK')}
    targets = {t for t, a, k in refs}

    def splittable(s):                                   # nothing can fall through into s
        i = idx[s] - 1
        while insns[i][1].startswith('NOP'):
            i -= 1
        return bool(TERM.match(insns[i][1]))
    chunks = [BOX2D] + TABLES
    for lo, hi in CANDIDATES:
        cut = sorted(s for s in (calls | targets) if lo < s < hi and s in idx and splittable(s))
        chunks += list(zip([lo] + cut, cut + [hi]))
    starts = sorted(c[0] for c in chunks)
    by_start = dict(chunks)
    dead = set(chunks)

    def owner(a):
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0:
            c = (starts[i], by_start[starts[i]])
            if c[0] <= a < c[1] and c in dead:
                return c
        return None
    by_target = collections.defaultdict(list)
    for t, a, k in refs:
        by_target[t].append((a, k))
    tlist = sorted(by_target)
    why, changed = {}, True
    while changed:
        changed = False
        for c in sorted(dead):
            i, j = bisect.bisect_left(tlist, c[0]), bisect.bisect_left(tlist, c[1])
            for t in tlist[i:j]:
                ext = [(a, k) for a, k in by_target[t] if not c[0] <= a < c[1] and owner(a) is None]
                if ext:
                    why[c] = (t, *ext[0])
                    dead.discard(c)
                    changed = True
                    break
    dead_ranges = sorted(c for c in dead if c[0] >= CODE[0])
    bad = 0
    for lo, hi in blanks:
        at = lo
        for s, e in dead_ranges:
            if s <= at < e:
                at = e
        if at < hi:
            bad += 1
            c = next((c for c in chunks if c[0] <= at < c[1]), None)
            t, a, k = why.get(c, (0, 0, "?"))
            print(f"FAIL: blank {lo:#x}..{hi:#x}: {at:#x} is reachable ({k} at {a:#x} -> {t:#x})")
        elif lo not in (CANDIDATES[0][0], CANDIDATES[1][0]) and lo in idx and not splittable(lo):
            bad += 1
            print(f"FAIL: blank {lo:#x}: code before it can fall through")
    total = sum(e - s for s, e in blanks)
    print(f"{len(blanks)} blank ranges, {total:,} bytes; dead code found: {sum(e - s for s, e in dead_ranges):,} bytes")
    print("PASS" if not bad else "FAIL")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
