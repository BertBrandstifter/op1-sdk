#!/usr/bin/env python3
"""Unpack an OP-1 .op1 firmware image (CRC-32 + LZMA + tar) into a directory."""
import binascii, io, lzma, os, struct, sys, tarfile

def unpack(src, dst):
    data = open(src, 'rb').read()
    crc = struct.unpack('<L', data[:4])[0]
    if binascii.crc32(data[4:]) != crc:
        sys.exit(f'{src}: CRC mismatch')
    tar = lzma.decompress(data[4:], format=lzma.FORMAT_ALONE)
    os.makedirs(dst, exist_ok=False)
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        for m in t.getmembers():
            if not (m.isfile() or m.isdir()) or m.name.startswith('/') or '..' in m.name.split('/'):
                sys.exit(f'refusing member {m.name!r}')
        t.extractall(dst, filter='data')
    print(f'{src} -> {dst} ({len(tar):,} bytes tar)')

if __name__ == '__main__':
    unpack(sys.argv[1], sys.argv[2])
