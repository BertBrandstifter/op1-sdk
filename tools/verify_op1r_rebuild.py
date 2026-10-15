#!/usr/bin/env python3
"""Rebuild op1r's OP1_vdk.ldr from stock + op1r blob + hooks.json; compare byte for byte."""
import hashlib, json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bfin_branch, ldrpatch  # noqa: E402

stock, blob, hooks, target = sys.argv[1:5]
p = ldrpatch.LdrPatcher(open(stock, 'rb').read())
for h in json.load(open(hooks)):
    if h['kind'] == 'branch':
        p.write(h['addr'], bfin_branch.encode(h['addr'], h['target'], call=h['new'] == 'CALL'))
    elif h['kind'] == 'pointers':
        for at, _, v in h['changes']:
            p.write(at, struct.pack('<I', v))
    else:
        p.write(h['addr'], bytes.fromhex(h['new']))
p.add_block(0x01e00000, open(blob, 'rb').read())
out = p.build()
ref = open(target, 'rb').read()
print('rebuilt', hashlib.sha256(out).hexdigest())
print('op1r   ', hashlib.sha256(ref).hexdigest())
print('IDENTICAL' if out == ref else f'DIFFERENT (first diff at {next(i for i in range(min(len(out), len(ref))) if out[i] != ref[i]):#x})')
