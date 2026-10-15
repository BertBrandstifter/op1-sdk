#!/usr/bin/env python3
"""Classify every difference between two memory dumps (stock vs patched) as branch hook / pointer / data.

    hooks.py mem_stock mem_op1r [--json hooks.json]
"""
import argparse, json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bfin_branch, memimage  # noqa: E402


def diffs(a, b, base):
    out, i = [], 0
    while i < len(a):
        if a[i] != b[i]:
            j = i
            while j < len(a) and (a[j] != b[j] or (j + 1 < len(a) and a[j + 1] != b[j + 1])):
                j += 1
            out.append((base + i, base + j))
            i = j
        i += 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('stock'); ap.add_argument('patched'); ap.add_argument('--json')
    a = ap.parse_args()
    lay_s = {s['addr']: s for s in json.load(open(os.path.join(a.stock, 'layout.json')))}
    lay_p = json.load(open(os.path.join(a.patched, 'layout.json')))
    new = [s for s in lay_p if s['addr'] not in lay_s]
    new_ranges = [(s['addr'], s['addr'] + s['size']) for s in new]
    in_new = lambda v: any(lo <= v < hi for lo, hi in new_ranges)
    rs, rp = memimage.reader(a.stock), memimage.reader(a.patched)
    out = []
    for s in lay_p:
        if s['addr'] not in lay_s:
            continue
        A = open(os.path.join(a.stock, s['file']), 'rb').read()
        B = open(os.path.join(a.patched, s['file']), 'rb').read()
        for lo, hi in diffs(A, B, s['addr']):
            start = lo & ~1
            kind = None
            for st in (start, start - 2):
                d_new = bfin_branch.decode(st, rp(st, 4))
                if d_new and st + 4 >= hi:
                    d_old = bfin_branch.decode(st, rs(st, 4))
                    kind = {'kind': 'branch', 'addr': st, 'new': d_new[0], 'target': d_new[1],
                            'old': f'{d_old[0]} {d_old[1]:#010x}' if d_old else 'insn ' + rs(st, 4).hex()}
                    break
            if not kind:
                st = lo & ~3
                n = ((hi - st + 3) // 4)
                old = struct.unpack(f'<{n}I', rs(st, n * 4)); newv = struct.unpack(f'<{n}I', rp(st, n * 4))
                if any(in_new(v) for v in newv):
                    kind = {'kind': 'pointers', 'addr': st,
                            'changes': [(st + 4 * k, o, v) for k, (o, v) in enumerate(zip(old, newv)) if o != v]}
                else:
                    kind = {'kind': 'data', 'addr': lo, 'old': rs(lo, hi - lo).hex(), 'new': rp(lo, hi - lo).hex()}
            out.append(kind)
    for h in out:
        if h['kind'] == 'branch':
            print(f"{h['addr']:#010x} BRANCH  {h['old']:24} -> {h['new']:6} {h['target']:#010x}")
        elif h['kind'] == 'pointers':
            for at, o, v in h['changes']:
                print(f"{at:#010x} POINTER {o:#010x}               -> {v:#010x}")
        else:
            print(f"{h['addr']:#010x} DATA    {h['old']} -> {h['new']}  {bytes.fromhex(h['new'])!r}")
    print(f"new segments: {', '.join(f'{lo:#x}-{hi:#x}' for lo, hi in new_ranges)}")
    if a.json:
        json.dump(out, open(a.json, 'w'), indent=1)


if __name__ == '__main__':
    main()
