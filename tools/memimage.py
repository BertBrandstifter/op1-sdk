#!/usr/bin/env python3
"""Build flat memory images from an LDR file: one .bin per segment + layout.json.

    memimage.py OP1_vdk.ldr outdir/
"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ldr  # noqa: E402


def build(path, outdir):
    data = open(path, 'rb').read()
    blocks = ldr.parse(data)
    os.makedirs(outdir, exist_ok=True)
    layout = []
    for s in ldr.segments(blocks):
        buf = bytearray(s['size'])
        for addr, chunk in ldr.memory(data, blocks):
            if s['addr'] <= addr < s['addr'] + s['size']:
                buf[addr - s['addr']:addr - s['addr'] + len(chunk)] = chunk
        name = f"{s['addr']:08x}.bin"
        open(os.path.join(outdir, name), 'wb').write(buf)
        layout.append({**s, 'file': name})
    json.dump(layout, open(os.path.join(outdir, 'layout.json'), 'w'), indent=1)
    return layout


def reader(outdir):
    """read(addr, n) over the dumped segments"""
    layout = json.load(open(os.path.join(outdir, 'layout.json')))
    segs = [(s['addr'], open(os.path.join(outdir, s['file']), 'rb').read()) for s in layout]

    def read(addr, n):
        for base, buf in segs:
            if base <= addr and addr + n <= base + len(buf):
                return buf[addr - base:addr - base + n]
        raise KeyError(f'{addr:#x} not mapped')
    return read


if __name__ == '__main__':
    for s in build(sys.argv[1], sys.argv[2]):
        print(f"{s['file']}  {s['size']:>10,}  {s['region']}")
