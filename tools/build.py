#!/usr/bin/env python3
"""Build an op1-sdk project (C and assembly sources + project.json) into an installable OP-1 update.

    build.py <project dir> <stock op1_246.op1> [-o out.op1] [--no-pack]

project.json:
{
  "name": "counter_c",
  "sources": ["entry.S", "counter.c"],
  "boot_text": "op1sdk 1",                      optional, at most 8 characters
  "symbols": {"ORIG_SEQIDS": "0x01a42344"},     extra linker symbols (TE addresses)
  "init": "app_init",                           optional C function, run once before the first hook
  "libs": ["gfx"],                              SDK libraries from lib/ (gfx = overlay drawing + font)
  "cflags": ["-O2"],                            extra compiler flags for all sources (after the defaults)
  "call_sites": [                               CALL instructions in TE code to point at our code
    {"site": "0x01a42a2e", "expect": "0x01a42344", "to": "_hook_seqids"}
  ],
  "entry_hooks": [                              function entries to overwrite with JUMP.L to our code;
    {"func": "0x01a85c20",                      "expect" = the exact bytes there in stock (hex, >= 4,
     "expect": "00e80400", "to": "_hook_midi"}  whole instructions); the hook must re-run them and
  ],                                            jump back to func + len(expect)
  "pointers": [                                 32-bit code pointers (vtable slots, callbacks) to redirect;
    {"addr": "0x01910ca8", "expect": "0x019e7934", "c": "my_filter_process"}
  ],                                            "c" = C function called directly with TE's arguments
  "content_sql": [                              SQL run on a content database of the update (e.g. a row
    {"file": "content/op1_factory.db",          in TE's engine list); plain statements on the existing
     "sql": ["INSERT INTO synth_types ..."]}    file, so its SQLite format stays the one TE reads
  ]
}
Instead of "to" (your own assembly), a hook can name a C function with "c"; build.py then generates
the trampoline (build/_sdk_hooks.S): it saves R0-R2/P0-P1/RETS, calls the C function with the same
arguments the TE function gets, restores them and continues into TE. With "filter": true the C
function returns int: nonzero skips the TE function (for void TE functions only). With "after": true
the TE function runs first and the C function is called afterwards with the same arguments (TE's
return value is kept); only for TE functions with at most 3 arguments (no stack arguments).
For entry hooks the generated code also replays the 4 overwritten bytes (refused if they are not
position independent) and exports te_orig_<func> (e.g. te_orig_01a85c20) to call the unhooked original.
The code is linked at 0x01e00000 (free SDRAM after the stock image) and added as one LDR block.
Every call site / entry hook / pointer must match "expect" in the stock image, otherwise the build stops.
Output: <project>/build/<name>.{elf,bin,map} and the .op1 (unless --no-pack).
"""

import argparse, hashlib, json, os, sqlite3, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import bfin_branch, ldrpatch, op1pack  # noqa: E402

BIN = os.path.join(SDK, "toolchain", "bfin", "bin", "bfin-elf-")
LOAD, LOAD_END = (
    0x01E00000,
    0x02000000,
)  # free SDRAM between the stock image and 0x02000000
STOCK_SHA256 = "c5315218f825f143b415ca554516541898abee843d3a236df0b54c04e1fb13a9"
BOOT_TEXT, BOOT_TEXT_STOCK = 0x018F738A, b"R. 00246"
CFLAGS = [
    "-mcpu=bf524",
    "-Os",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-common",
    "-ffunction-sections",
    "-fdata-sections",
    "-Wall",
    "-Wextra",
    "-I",
    os.path.join(SDK, "include"),
]


def run(*cmd):
    subprocess.run(cmd, check=True)


HOOK_SAVE = "\t[--SP] = RETS;\n\t[--SP] = P1;\n\t[--SP] = P0;\n\t[--SP] = R2;\n\t[--SP] = R1;\n\t[--SP] = R0;\n"
HOOK_RESTORE = "\tR0 = [SP++];\n\tR1 = [SP++];\n\tR2 = [SP++];\n\tP0 = [SP++];\n\tP1 = [SP++];\n\tRETS = [SP++];\n"
NOT_RELOCATABLE = ("JUMP", "CALL", "LSETUP", "LOOP", "RTS", "RTI", "RTX", "RTN", "RTE", "ILLEGAL")


def check_replay(func, raw):
    """raw (>= 4 bytes) must be whole, position-independent instructions: the JUMP.L overwrites the
    first 4 bytes, the replay re-runs all of raw and resumes at func + len(raw)"""
    tmp = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"sdk_replay_{os.getpid()}.bin")
    with open(tmp, "wb") as f:
        f.write(raw + bytes(8))
    try:
        txt = subprocess.run(
            [BIN + "objdump", "-z", "-b", "binary", "-m", "bfin", "-D", tmp],
            capture_output=True, text=True, check=True,
        ).stdout
    finally:
        os.remove(tmp)
    starts, insns = [], []
    for line in txt.splitlines():
        f = line.split("\t")
        if len(f) >= 3 and f[0].strip().endswith(":"):
            starts.append(int(f[0].strip()[:-1], 16))
            insns.append(f[2].strip())
    n = len(raw)
    if n < 4 or n not in starts:
        sys.exit(f"entry hook {func:#x}: {raw.hex()} is not >= 4 bytes of whole instructions")
    for a, insn in zip(starts, insns):
        if a < n and any(w in insn.upper() for w in NOT_RELOCATABLE):
            sys.exit(f"entry hook {func:#x}: cannot replay '{insn}' elsewhere (PC-relative/flow change)")
    return [i for a, i in zip(starts, insns) if a < n]


def gen_hooks(cfg, out):
    """write build/_sdk_hooks.S for hooks given with "c"; fills in their "to" symbols.
    Returns (path or None, TE addresses the linker must define)"""
    lines, init, te_syms = [], cfg.get("init"), {}
    hooks = [(h, int(h["site"], 0), "site") for h in cfg.get("call_sites", [])]
    hooks += [(h, int(h["func"], 0), "func") for h in cfg.get("entry_hooks", [])]
    for pt in cfg.get("pointers", []):
        if "c" in pt:
            pt["to"] = "_" + pt["c"]
    for h, addr, kind in hooks:
        if "c" not in h:
            continue
        name = f"__sdk_hook_{addr:08x}"
        h["to"] = name
        if kind == "site":
            cont = f"__te_{int(h['expect'], 0):08x}"
            te_syms[cont] = f"{int(h['expect'], 0):#x}"
        else:
            raw = bytes.fromhex(h["expect"].replace(" ", ""))
            insns = check_replay(addr, raw)
            cont = f"_te_orig_{addr:08x}"
            back = addr + len(raw)
            te_syms[f"__te_{back:08x}"] = f"{back:#x}"
            lines += [
                f"/* {addr:#010x} original entry: {'; '.join(insns)} */",
                f"\t.global {cont}",
                f"{cont}:",
                "\t.byte " + ", ".join(f"{b:#04x}" for b in raw),
                f"\tJUMP.L __te_{back:08x};",
                "",
            ]
        lines += [f"\t.global {name}", f"{name}:", HOOK_SAVE.rstrip("\n")]
        if init:
            lines += [
                "\tP0.L = __sdk_inited;", "\tP0.H = __sdk_inited;", "\tR0 = [P0];",
                "\tCC = R0 == 0;", f"\tIF !CC JUMP 1f;", "\tR0 = 1;", "\t[P0] = R0;",
                "\tSP += -12;", f"\tCALL _{init};", "\tSP += 12;",
                "1:", "\tR0 = [SP];", "\tR1 = [SP + 4];", "\tR2 = [SP + 8];",
            ]
        if h.get("after"):
            if h.get("filter"):
                sys.exit(f"hook {addr:#x}: 'after' and 'filter' cannot be combined")
            lines += [
                "\tR0 = [SP];", "\tR1 = [SP + 4];", "\tR2 = [SP + 8];", "\tP0 = [SP + 12];",
                "\tP1 = [SP + 16];", "\tSP += -12;", f"\tCALL {cont};", "\tSP += 12;",
                "\t[--SP] = R0;", "\tR0 = [SP + 4];", "\tR1 = [SP + 8];", "\tR2 = [SP + 12];",
                "\tSP += -12;", f"\tCALL _{h['c']};", "\tSP += 12;",
                "\tR0 = [SP++];", "\tSP += 20;", "\tRETS = [SP++];", "\tRTS;", "",
            ]
            continue
        lines += ["\tSP += -12;", f"\tCALL _{h['c']};", "\tSP += 12;"]
        if h.get("filter"):
            lines += ["\tCC = R0 == 0;", HOOK_RESTORE.rstrip("\n"), "\tIF CC JUMP 2f;", "\tRTS;", "2:"]
        else:
            lines += [HOOK_RESTORE.rstrip("\n")]
        lines += [f"\tJUMP.L {cont};", ""]
    if not lines:
        return None, te_syms
    if init:
        lines += ["\t.section .bss.sdk_inited", "\t.align 4", "__sdk_inited:", "\t.long 0"]
    path = os.path.join(out, "_sdk_hooks.S")
    with open(path, "w") as f:
        f.write("/* generated by tools/build.py from project.json - do not edit */\n")
        f.write("\t.section .text.entry\n" + "\n".join(lines) + "\n")
    return path, te_syms


def compile_project(proj, cfg):
    out = os.path.join(proj, "build")
    os.makedirs(out, exist_ok=True)
    objs = []
    srcs = [os.path.join(proj, src) for src in cfg["sources"]]
    srcs += [os.path.join(SDK, "lib", lib + ".c") for lib in ["rt"] + cfg.get("libs", [])]
    generated, te_syms = gen_hooks(cfg, out)
    if generated:
        srcs.insert(0, generated)
    for src in srcs:
        prefix = "lib_" if os.path.dirname(src) == os.path.join(SDK, "lib") else ""
        obj = os.path.join(out, prefix + os.path.splitext(os.path.basename(src))[0] + ".o")
        run(BIN + "gcc", *CFLAGS, *cfg.get("cflags", []), "-c", src, "-o", obj)
        objs.append(obj)
    name = cfg["name"]
    elf, binf = os.path.join(out, name + ".elf"), os.path.join(out, name + ".bin")
    allsyms = {**cfg.get("symbols", {}), **te_syms}
    defsyms = [f"-Wl,--defsym={k}={v}" for k, v in allsyms.items()]
    first = None
    for key in ("call_sites", "entry_hooks", "pointers"):
        if cfg.get(key):
            first = cfg[key][0]["to"]
            break
    if first:
        defsyms.append(f"-Wl,--defsym=_sdk_entry={first}")
    for key in ("call_sites", "entry_hooks", "pointers"):
        defsyms += [f"-Wl,-u,{h['to']}" for h in cfg.get(key, [])]  # keep targets alive under --gc-sections
    run(
        BIN + "gcc",
        "-mcpu=bf524",
        "-nostdlib",
        "-T",
        os.path.join(HERE, "sdk.ld"),
        f"-Wl,--defsym=SDK_LOAD={LOAD:#x}",
        *defsyms,
        "-Wl,--gc-sections",
        "-Wl,--no-warn-rwx-segments",
        "-Wl,-Map," + os.path.join(out, name + ".map"),
        "-o",
        elf,
        *objs,
        "-lgcc",
    )
    run(BIN + "objcopy", "-O", "binary", elf, binf)
    syms = {}
    for line in subprocess.run(
        [BIN + "nm", elf], capture_output=True, text=True, check=True
    ).stdout.splitlines():
        f = line.split()
        if len(f) == 3:
            syms[f[2]] = int(f[0], 16)
    code = open(binf, "rb").read()
    if syms["_sdk_end"] - LOAD != len(code):
        sys.exit("image is not contiguous (check sdk.ld sections)")
    if LOAD + len(code) > LOAD_END:
        sys.exit(f"code too large: {len(code):,} bytes, {LOAD_END - LOAD:,} available")
    return code, syms, elf


def patch_firmware(stock, cfg, code, syms):
    if hashlib.sha256(open(stock, "rb").read()).hexdigest() != STOCK_SHA256:
        sys.exit("not the original R246 image")
    members = op1pack.read(stock)
    i = next(k for k, (m, _) in enumerate(members) if m.name == "OP1_vdk.ldr")
    p = ldrpatch.LdrPatcher(members[i][1])
    if "boot_text" in cfg:
        text = cfg["boot_text"].encode("ascii")
        if len(text) > 8:
            sys.exit("boot_text: at most 8 characters")
        assert p.read(BOOT_TEXT, 8) == BOOT_TEXT_STOCK
        p.write(BOOT_TEXT, text.ljust(8))
    for cs in cfg.get("call_sites", []):
        site, expect, to = int(cs["site"], 0), int(cs["expect"], 0), syms[cs["to"]]
        found = bfin_branch.decode(site, p.read(site, 4))
        if found != ("CALL", expect):
            sys.exit(f"call site {site:#x}: expected CALL {expect:#x}, found {found}")
        p.write(site, bfin_branch.encode(site, to))
        print(f"  call site {site:#010x}: CALL {expect:#010x} -> {cs['to']} {to:#010x}")
    for eh in cfg.get("entry_hooks", []):
        func, to = int(eh["func"], 0), syms[eh["to"]]
        expect = bytes.fromhex(eh["expect"].replace(" ", ""))
        found = p.read(func, max(4, len(expect)))
        if len(expect) < 4 or found != expect:
            sys.exit(
                f"entry hook {func:#x}: expected bytes {expect.hex()}, found {found.hex()}"
            )
        p.write(func, bfin_branch.encode(func, to, call=False))
        print(
            f"  entry hook {func:#010x}: {found.hex()} -> JUMP.L {eh['to']} {to:#010x}"
        )
    for pt in cfg.get("pointers", []):
        addr, expect, to = int(pt["addr"], 0), int(pt["expect"], 0), syms[pt["to"]]
        found = int.from_bytes(p.read(addr, 4), "little")
        if found != expect:
            sys.exit(f"pointer {addr:#x}: expected {expect:#010x}, found {found:#010x}")
        p.write(addr, to.to_bytes(4, "little"))
        print(f"  pointer {addr:#010x}: {expect:#010x} -> {pt['to']} {to:#010x}")
    p.add_block(LOAD, code)
    members[i] = (members[i][0], p.build())
    for cs in cfg.get("content_sql", []):
        j = next((k for k, (m, _) in enumerate(members) if m.name == cs["file"]), None)
        if j is None:
            sys.exit(f"content_sql: {cs['file']} is not in the update")
        members[j] = (members[j][0], run_sql(members[j][1], cs["sql"]))
        for q in cs["sql"]:
            print(f"  {cs['file']}: {q}")
    return members


def run_sql(db, statements):
    """run SQL statements on a copy of an SQLite file; returns the new file bytes. No VACUUM, no
    journal mode change: the file keeps the format TE's SQLite reads."""
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "x.db")
        open(path, "wb").write(db)
        con = sqlite3.connect(path)
        try:
            for q in statements:
                con.execute(q)
            con.commit()
            if con.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
                sys.exit("content_sql: integrity check failed")
        finally:
            con.close()
        return open(path, "rb").read()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("project")
    ap.add_argument("firmware")
    ap.add_argument("-o", "--output")
    ap.add_argument("--no-pack", action="store_true")
    a = ap.parse_args()
    cfg = json.load(open(os.path.join(a.project, "project.json")))
    code, syms, elf = compile_project(a.project, cfg)
    print(f"{cfg['name']}: {len(code):,} bytes at {LOAD:#x} ({elf})")
    members = patch_firmware(a.firmware, cfg, code, syms)
    if a.no_pack:
        return
    out = a.output or os.path.join(a.project, "build", cfg["name"] + ".op1")
    res = op1pack.write(out, members)
    print(f"{out}: {len(res):,} bytes, sha256 {hashlib.sha256(res).hexdigest()}")


if __name__ == "__main__":
    main()
