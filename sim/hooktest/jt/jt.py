import subprocess, sys
sys.path.insert(0, '../../../tools'); import ldr2elf
B = '../../../toolchain/bfin/bin/bfin-elf-'
def build(src, addr, entry, name, tgt=0):
    open(name + '.S', 'w').write(src)
    subprocess.run([B + 'as', '-o', name + '.o', name + '.S'], check=True)
    subprocess.run([B + 'ld', '-e', entry, f'-Ttext={addr:#x}', f'--defsym=T={tgt:#x}', '-o', name + '.elf', name + '.o'], check=True)
    subprocess.run([B + 'objcopy', '-O', 'binary', name + '.elf', name + '.bin'], check=True)
    return open(name + '.bin', 'rb').read()
EXIT = '.text\n.global _x\n_x: HLT;\n'
for name, tgt, insn in [('back_far', 0x01a42344, 'JUMP.L'), ('fwd', 0x01f10000, 'JUMP.L'), ('near', 0x01f00100, 'JUMP.L'),
                        ('back_1MB', 0x01e00000, 'JUMP.L'), ('ind_back_far', 0x01a42344, 'P1')]:
    x = build(EXIT, tgt, '_x', 'x')
    src = f'.text\n.global _s\n_s: JUMP.L T;\n' if insn == 'JUMP.L' else \
          f'.text\n.global _s\n_s: P1.L = {tgt & 0xffff:#x}; P1.H = {tgt >> 16:#x}; JUMP (P1);\n'
    j = build(src, 0x01f00000, '_s', 'j', tgt)
    ldr2elf.write_elf('t.elf', [(tgt, x), (0x01f00000, j), (0x01f70000, bytes(0x10000))], 0x01f00000)
    try:
        r = subprocess.run([B + 'run', '--model', 'bf524', 't.elf'], timeout=5)
        print(f'{name:14} {tgt:#010x} exit {r.returncode}')
    except subprocess.TimeoutExpired:
        print(f'{name:14} {tgt:#010x} HANG')
