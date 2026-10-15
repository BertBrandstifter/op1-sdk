#!/usr/bin/env python3
"""Turn an LDR boot stream (+ optional extra raw blobs) into a Blackfin ELF the GDB simulator can load.

    ldr2elf.py OP1_vdk.ldr out.elf [--entry 0xffa00000] [--add 0x01f00000:driver.bin ...] [--skip-l1]

One PT_LOAD per merged memory segment; fill blocks are expanded.
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ldr  # noqa: E402

EM_BLACKFIN = 106


def segments_from_ldr(data, skip_l1=False):
    blocks = ldr.parse(data)
    segs = []
    for s in ldr.segments(blocks):
        if skip_l1 and s['addr'] >= 0xff800000:
            continue
        buf = bytearray(s['size'])
        for addr, chunk in ldr.memory(data, blocks):
            if s['addr'] <= addr < s['addr'] + s['size']:
                buf[addr - s['addr']:addr - s['addr'] + len(chunk)] = chunk
        segs.append((s['addr'], bytes(buf)))
    return segs


def write_elf(path, segs, entry):
    """ELF32 LE EXEC: one PT_LOAD + one PROGBITS section (.segN) per segment, plus .shstrtab"""
    ehsize, phsize, shsize = 52, 32, 40
    names = b'\0.shstrtab\0' + b''.join(f'.seg{i}\0'.encode() for i in range(len(segs)))
    off = ehsize + phsize * len(segs)
    ph, body, sh, offs = b'', b'', [b'\0' * shsize], []
    for addr, buf in segs:
        offs.append(off + len(body))
        ph += struct.pack('<8I', 1, offs[-1], addr, addr, len(buf), len(buf), 7, 4)
        body += buf
    strtab_off = off + len(body)
    body += names
    body += b'\0' * (-len(body) % 4)
    shoff = off + len(body)
    for i, (addr, buf) in enumerate(segs):
        name = names.index(f'.seg{i}\0'.encode())
        sh.append(struct.pack('<10I', name, 1, 7, addr, offs[i], len(buf), 0, 0, 4, 0))   # PROGBITS, WAX
    sh.append(struct.pack('<10I', 1, 3, 0, 0, strtab_off, len(names), 0, 0, 1, 0))
    eh = struct.pack('<16sHHIIIIIHHHHHH', b'\x7fELF\x01\x01\x01' + b'\0' * 9, 2, EM_BLACKFIN, 1, entry,
                     ehsize, shoff, 0, ehsize, phsize, len(segs), shsize, len(sh), len(sh) - 1)
    open(path, 'wb').write(eh + ph + body + b''.join(sh))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ldr'); ap.add_argument('out')
    ap.add_argument('--entry', type=lambda v: int(v, 0), default=0xffa00000)
    ap.add_argument('--add', action='append', default=[], help='ADDR:file.bin')
    ap.add_argument('--skip-l1', action='store_true', help='leave L1 out (its blocks are encrypted in the image)')
    a = ap.parse_args()
    segs = segments_from_ldr(open(a.ldr, 'rb').read(), a.skip_l1)
    for spec in a.add:
        addr, f = spec.split(':', 1)
        segs.append((int(addr, 0), open(f, 'rb').read()))
    write_elf(a.out, segs, a.entry)
    for addr, buf in segs:
        print(f'  {addr:#010x} {len(buf):>10,}')


if __name__ == '__main__':
    main()
