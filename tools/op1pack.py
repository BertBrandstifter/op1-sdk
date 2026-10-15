#!/usr/bin/env python3
"""Read / write OP-1 .op1 update files in memory: CRC-32 (LE) + LZMA 'alone' + GNU tar.

Repacking keeps the original member order and metadata, and uses the LZMA settings known to install
(op1repacker's: the OP-1 accepts < 15 MB and has limited decoder RAM).
"""
import binascii, copy, io, lzma, struct, tarfile

LZMA_FILTERS = [{'id': lzma.FILTER_LZMA1, 'preset': 9, 'lc': 3, 'lp': 1, 'pb': 2, 'dict_size': 2 ** 23}]
MAX_SIZE = 15 * 1024 * 1024


def read(path):
    data = open(path, 'rb').read()
    if struct.unpack('<L', data[:4])[0] != binascii.crc32(data[4:]):
        raise ValueError(f'{path}: CRC mismatch')
    with tarfile.open(fileobj=io.BytesIO(lzma.decompress(data[4:], format=lzma.FORMAT_ALONE))) as t:
        return [(m, t.extractfile(m).read() if m.isfile() else None) for m in t.getmembers()]


def write(path, members):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w', format=tarfile.GNU_FORMAT) as t:
        for info, data in members:
            info = copy.copy(info)
            if data is None:
                t.addfile(info)
            else:
                info.size = len(data)
                t.addfile(info, io.BytesIO(data))
    comp = lzma.compress(buf.getvalue(), format=lzma.FORMAT_ALONE, filters=LZMA_FILTERS)
    out = struct.pack('<L', binascii.crc32(comp)) + comp
    if len(out) >= MAX_SIZE:
        raise ValueError(f'{len(out):,} bytes: the OP-1 only accepts update files below 15 MB')
    open(path, 'wb').write(out)
    return out
