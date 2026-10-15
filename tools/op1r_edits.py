#!/usr/bin/env python3
"""Map op1r's OP1_vdk.ldr byte edits (data/manifest.json) to Blackfin memory addresses."""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ldr  # noqa: E402


def locate(blocks, off):
    for b in blocks:
        if b['offset'] <= off < b['payload']:
            return b, None                         # inside a header
        if b['payload'] <= off < b['payload'] + b['payload_len']:
            return b, b['addr'] + off - b['payload']
    return None, None


def main(manifest, stock_ldr):
    man = json.load(open(manifest))
    data = open(stock_ldr, 'rb').read()
    blocks = ldr.parse(data)
    f = next(p for p in man['patched_files'] if p['path'] == 'OP1_vdk.ldr')
    rows = []
    for op in f['ops']:
        b, addr = locate(blocks, op['offset'])
        if op['op'] == 'insert':
            print(f"INSERT at file {op['offset']:#x}: {op['size']:,} bytes ({op['blob']})")
            continue
        n = len(op['old']) // 2
        where = f"mem {addr:#010x} [{ldr.region(addr)}]" if addr is not None else \
            f"HEADER of block @{b['offset']:#x} (field +{op['offset'] - b['offset']})"
        rows.append((op['offset'], n, where, op['old'], op['new']))
        print(f"file {op['offset']:#09x} {n:3}B  {where:40} {op['old'][:32]:32} -> {op['new'][:32]}")
    print(f'{len(rows)} replace ops')


if __name__ == '__main__':
    main(*sys.argv[1:3])
