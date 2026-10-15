"""Decode Blackfin 32-bit JUMP.L / CALL (pcrel25m2) instructions."""
import struct


def decode(addr, raw):
    """raw = 4 bytes as stored (two little-endian 16-bit halfwords); returns (mnemonic, target) or None"""
    hi, lo = struct.unpack('<2H', raw)
    if hi >> 8 not in (0xE2, 0xE3):
        return None
    imm = ((hi & 0xFF) << 16) | lo
    if imm & 0x800000:
        imm -= 1 << 24
    return ('JUMP.L' if hi >> 8 == 0xE2 else 'CALL'), (addr + imm * 2) & 0xFFFFFFFF


def encode(addr, target, call=True):
    off = target - addr
    assert off % 2 == 0 and -(1 << 24) <= off < (1 << 24), hex(off)
    imm = (off // 2) & 0xFFFFFF
    return struct.pack('<2H', (0xE3 if call else 0xE2) << 8 | imm >> 16, imm & 0xFFFF)
