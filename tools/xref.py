#!/usr/bin/env python3
"""Recover functions, call graph and string references from a bfin-elf-objdump listing.

    xref.py stock_sdram.dis stock_strings.txt out.json

Functions = CALL targets (+ LINK sites). A constant built with X.L/X.H (objdump annotates "X=0x...")
that points at a known string is recorded as a string reference of the enclosing function.
Functions referencing assertion strings ("..\\src\\dir\\File.cpp:123 cond") get a source-file name.
"""
import bisect, json, re, sys
from collections import Counter, defaultdict

LINE = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$')
CALL = re.compile(r'\bCALL 0x0x([0-9a-f]+)')
CONST = re.compile(r'\b([PR][0-7])=0x([0-9a-f]+)')


def main(dis, strings, out):
    strs = {}
    for l in open(strings):
        a, s = l.split(' ', 1)
        strs[int(a, 16)] = eval(s)                 # repr written by us
    insns, calls, links = [], Counter(), set()
    for l in open(dis):
        m = LINE.match(l)
        if not m:
            continue
        addr, text = int(m.group(1), 16), m.group(3)
        insns.append((addr, text))
        c = CALL.search(text)
        if c:
            calls[int(c.group(1), 16)] += 1
        if text.startswith('LINK'):
            links.add(addr)
    lo, hi = insns[0][0], insns[-1][0]
    starts = sorted({a for a in calls if lo <= a <= hi} | links)
    funcs = {a: {'addr': a, 'callers': calls.get(a, 0), 'calls': set(), 'strings': []} for a in starts}

    def owner(a):
        i = bisect.bisect_right(starts, a) - 1
        return starts[i] if i >= 0 else None

    for addr, text in insns:
        f = owner(addr)
        if f is None:
            continue
        c = CALL.search(text)
        if c:
            funcs[f]['calls'].add(int(c.group(1), 16))
        if '.H = ' in text or '.L = ' in text:
            for _, v in CONST.findall(text):
                v = int(v, 16)
                if v in strs and strs[v] not in funcs[f]['strings']:
                    funcs[f]['strings'].append(strs[v])
    for f in funcs.values():
        srcs = Counter()
        for s in f['strings']:
            m = re.match(r'\.\.\\src\\(.+?\.(?:cpp|c|h)):(\d+)', s)
            if m:
                srcs[m.group(1).replace('\\', '/')] += 1
        f['source'] = srcs.most_common(1)[0][0] if srcs else None
        f['asserts'] = [s for s in f['strings'] if s.startswith('..\\src')]
        f['calls'] = sorted(f['calls'])
        base = f['source'].rsplit('/', 1)[-1].split('.')[0] if f['source'] else 'sub'
        f['name'] = f"{base}_{f['addr']:08x}"
    json.dump(sorted(funcs.values(), key=lambda f: f['addr']), open(out, 'w'), indent=0)
    named = sum(1 for f in funcs.values() if f['source'])
    print(f'{len(funcs)} functions, {named} with a source file, {sum(len(f["strings"]) for f in funcs.values())} string refs')


if __name__ == '__main__':
    main(*sys.argv[1:4])
