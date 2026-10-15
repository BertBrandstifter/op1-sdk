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
