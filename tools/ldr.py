#!/usr/bin/env python3
"""Parse Blackfin BF52x/BF54x LDR boot streams (as used by OP1_vdk.ldr / te-boot.ldr).

Each block: 16-byte header (block code, target address, byte count, argument) + payload.
Block code: [31:24] HDRSIGN 0xAD, [23:16] HDRCHK (XOR of the header bytes), [15:0] flags.
"""
import argparse, json, struct, sys

FLAGS = {0x0010: 'SAFE', 0x0020: 'AUX', 0x0100: 'FILL', 0x0200: 'QUICKBOOT', 0x0400: 'CALLBACK',
         0x0800: 'INIT', 0x1000: 'IGNORE', 0x2000: 'INDIRECT', 0x4000: 'FIRST', 0x8000: 'FINAL'}


def header_checksum(hdr):
    """HDRCHK makes the XOR of all 16 header bytes zero"""
    x = 0
    for i, b in enumerate(hdr):
        if i != 2:
            x ^= b
    return x


def parse(data):
    blocks, off = [], 0
    while off < len(data):
        hdr = data[off:off + 16]
        if len(hdr) < 16:
            raise ValueError(f'truncated header at {off:#x}')
        code, addr, count, arg = struct.unpack('<4I', hdr)
        if code >> 24 != 0xAD:
            raise ValueError(f'bad HDRSIGN at {off:#x}: {code:#010x}')
        flags = code & 0xFFFF
        chk_ok = (code >> 16) & 0xFF == header_checksum(hdr)
        payload_len = 0 if flags & 0x0100 else count
        blocks.append({'offset': off, 'flags': flags, 'dma': flags & 0xF,
                       'names': [n for f, n in FLAGS.items() if flags & f],
                       'addr': addr, 'count': count, 'arg': arg, 'chk_ok': chk_ok,
                       'payload': off + 16, 'payload_len': payload_len})
        off += 16 + payload_len
    return blocks


REGIONS = [  # ADSP-BF524 memory map
    (0x00000000, 0x08000000, 'SDRAM'),
    (0x20000000, 0x20400000, 'async (flash)'),
    (0xEF000000, 0xEF008000, 'boot ROM'),
    (0xFF800000, 0xFF808000, 'L1 data A'),
    (0xFF900000, 0xFF908000, 'L1 data B'),
    (0xFFA00000, 0xFFA10000, 'L1 instruction SRAM'),
    (0xFFA10000, 0xFFA14000, 'L1 instruction SRAM/cache'),
    (0xFFB00000, 0xFFB01000, 'L1 scratchpad'),
]


def region(addr):
    return next((n for lo, hi, n in REGIONS if lo <= addr < hi), '?')


def loaded(blocks):
    return [b for b in blocks if not b['flags'] & 0x1000 and b['count']]


def segments(blocks):
    """merge contiguous loaded blocks (data + fill) into memory segments"""
    segs = []
    for b in loaded(blocks):
        fill = bool(b['flags'] & 0x0100)
        s = segs[-1] if segs else None
        if s and s['addr'] + s['size'] == b['addr'] and not (b['flags'] & 0x0800) and not s['init']:
            s['size'] += b['count']
        else:
            s = {'addr': b['addr'], 'size': b['count'], 'init': bool(b['flags'] & 0x0800),
                 'region': region(b['addr']), 'data': 0, 'fill': 0, 'blocks': 0}
            segs.append(s)
        s['fill' if fill else 'data'] += b['count']
        s['blocks'] += 1
    return segs


def memory(data, blocks):
    """[(addr, bytes)] for every loaded block, fill blocks expanded, in boot order"""
    out = []
    for b in loaded(blocks):
        if b['flags'] & 0x0100:
            chunk = (struct.pack('<I', b['arg']) * (b['count'] // 4 + 1))[:b['count']]
        else:
            chunk = data[b['payload']:b['payload'] + b['count']]
        out.append((b['addr'], chunk))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('ldr')
    ap.add_argument('--blocks', action='store_true', help='print every block')
    ap.add_argument('--json', help='write blocks + segments as JSON')
    a = ap.parse_args()
    data = open(a.ldr, 'rb').read()
    blocks = parse(data)
    bad = [b for b in blocks if not b['chk_ok']]
    print(f'{a.ldr}: {len(data):,} bytes, {len(blocks)} blocks, {len(bad)} bad header checksums')
    if a.blocks:
        for b in blocks:
            print(f"  @{b['offset']:#09x} addr {b['addr']:#010x} count {b['count']:#09x} arg {b['arg']:#010x}"
                  f" dma {b['dma']} {' '.join(b['names'])}{'' if b['chk_ok'] else ' BADCHK'}")
    print('segments:')
    for s in segments(blocks):
        print(f"  {s['addr']:#010x}-{s['addr'] + s['size']:#010x} {s['size']:>10,}  data {s['data']:>9,}"
              f"  fill {s['fill']:>10,}  blocks {s['blocks']:>4}  {s['region']}{'  INIT' if s['init'] else ''}")
    if a.json:
        json.dump({'blocks': blocks, 'segments': segments(blocks)}, open(a.json, 'w'), indent=1)


if __name__ == '__main__':
    main()
