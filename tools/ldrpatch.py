#!/usr/bin/env python3
"""Patch a Blackfin LDR boot stream at the memory level.

    p = LdrPatcher(open('OP1_vdk.ldr','rb').read())
    p.write(0x018f738a, b' op1r 10')        # in-place write into existing (non-fill) data blocks
    p.add_block(0x01e00000, code)           # new data block, inserted before the FINAL block
    out = p.build()                         # first-block DXE length + header checksums fixed
"""
import struct
import ldr


def header(code_flags, addr, count, arg):
    hdr = bytearray(struct.pack('<4I', 0xAD000000 | code_flags, addr, count, arg))
    hdr[2] = ldr.header_checksum(hdr)
    return bytes(hdr)


class LdrPatcher:
    def __init__(self, data):
        self.data = bytearray(data)
        self.blocks = ldr.parse(bytes(data))
        self.new_blocks = []

    def file_offset(self, addr, n):
        for b in self.blocks:
            if b['flags'] & 0x1100 or not b['count']:      # skip IGNORE / FILL
                continue
            if b['addr'] <= addr and addr + n <= b['addr'] + b['count']:
                return b['payload'] + addr - b['addr']
        raise ValueError(f'{addr:#x}+{n} is not inside a single loaded data block')

    def read(self, addr, n):
        o = self.file_offset(addr, n)
        return bytes(self.data[o:o + n])

    def write(self, addr, raw):
        o = self.file_offset(addr, len(raw))
        self.data[o:o + len(raw)] = raw

    def pieces(self, addr, n):
        """(addr, length, file offset or None for a FILL block) covering addr..addr+n, in order"""
        out, at, end = [], addr, addr + n
        for b in sorted((b for b in self.blocks if not b['flags'] & 0x1000 and b['count']),
                        key=lambda b: b['addr']):
            lo, hi = max(at, b['addr']), min(end, b['addr'] + b['count'])
            if lo >= hi:
                continue
            if lo != at:
                raise ValueError(f'{at:#x}..{lo:#x} is not loaded by any block')
            fill = b['flags'] & 0x0100
            out.append((lo, hi - lo, None if fill else b['payload'] + lo - b['addr'], b['arg'] if fill else None))
            at = hi
        if at != end:
            raise ValueError(f'{at:#x}..{end:#x} is not loaded by any block')
        return out

    def read_span(self, addr, n):
        """memory content addr..addr+n across blocks (FILL blocks give their 32-bit pattern)"""
        raw = b''
        for a, k, o, arg in self.pieces(addr, n):
            if o is None:
                pat = arg.to_bytes(4, 'little')
                raw += bytes(pat[(a + i) % 4] for i in range(k))
            else:
                raw += bytes(self.data[o:o + k])
        return raw

    def fill_span(self, addr, n, pattern):
        """write pattern (repeated, aligned to addr; its length must divide 4) over addr..addr+n. A
        FILL block wholly inside gets the pattern as its fill value; one sticking out is left as it
        is. Returns the bytes left that way."""
        kept = 0
        for a, k, o, _ in self.pieces(addr, n):
            if o is not None:
                self.data[o:o + k] = bytes(pattern[(a - addr + i) % len(pattern)] for i in range(k))
                continue
            b = next((b for b in self.blocks if b['addr'] == a and b['flags'] & 0x0100 and b['count'] == k
                      and not b['flags'] & 0x1000), None) if 4 % len(pattern) == 0 else None
            if b is None:
                kept += k
                continue
            fill = bytes(pattern[(a - addr + i) % len(pattern)] for i in range(4))
            h = bytearray(self.data[b['offset']:b['offset'] + 16])
            h[12:16] = fill
            h[2] = ldr.header_checksum(h)
            self.data[b['offset']:b['offset'] + 16] = h
            b['arg'] = int.from_bytes(fill, 'little')
        return kept

    def add_block(self, addr, payload, flags=0x0006):
        self.new_blocks.append((addr, bytes(payload), flags))

    def build(self):
        final = next(b for b in self.blocks if b['flags'] & 0x8000)
        first = self.blocks[0]
        assert first['flags'] & 0x4000, 'first block is not FIRST'
        ins = b''.join(header(f, a, len(p), 0) + p for a, p, f in self.new_blocks)
        out = bytearray(self.data[:final['offset']] + ins + self.data[final['offset']:])
        f, a, c, _ = struct.unpack_from('<4I', out, 0)
        out[0:16] = header(f & 0xFFFF, a, c, len(out) - 16)
        return bytes(out)
