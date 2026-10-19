#!/usr/bin/env python3
"""Compose OP-1 features (features/<name>/feature.json) into one installable firmware image.

    compose.py recipes/op1plus.json [stock op1_246.op1]         recipe -> build/apps/op1plus/
    compose.py --features quantize,key_probe --name test [stock] ad-hoc list
    compose.py ... --no-build                                    only generate the project

A recipe: {"name": "op1plus", "boot_text": "op1sdk +", "features": ["quantize", "engine_plus"]}

Each feature brings its sources and two kinds of hooks:
  - exclusive hooks ("hooks": call_sites / entry_hooks / pointers, as in build.py's project.json):
    only one feature may own an address; collisions stop the build and name both features.
  - shared hook points ("shared": [{"point": "on_key", "fn": "q_key", "mode": "filter"}]): TE places
    several features need (key/knob input, frame overlay, sequencer tick, MIDI in, ...). The composer
    owns the hook and generates one dispatcher (_compose.c) that calls every feature's function:
      observe  called with the arguments, result ignored (runs first, sees every event)
      filter   returns int; the first nonzero consumes the event (TE never sees it)
      after    for "after" points: runs after TE, in recipe order
    Handlers run in recipe order; "order": "first" / "last" on a shared entry overrides that.
  - "init": optional one-time function, chained into one init.
Dependencies ("depends", optionally with a version range), declared conflicts ("conflicts"), the
SDK API level ("sdk"), input claims ("claims": two features on the same key/knob = error) and the
target firmware ("firmware": SHA-256 of the stock .op1 each feature was built against) are checked
before anything is generated. Published features: docs/feature-package.md.

Recipe entries are feature names, or {"name": ..., "repository": ..., "ref": ...} for a published
package (install it first: features.py install <repository> --ref <ref>).
"""

import argparse, hashlib, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(HERE)
FEATURES = os.path.join(SDK, "features")
sys.path.insert(0, HERE)
import fpkg  # noqa: E402

# Shared TE hook points (R246). kind/params = what build.py needs; sig = C signature of the handlers.
POINTS = {
    "on_key": dict(kind="entry_hooks", func="0x019b6c74", expect="00e80000", mode="filter",
                   ret="int", args="int key, int down", call="key, down",
                   what="KeyHandler thread: every key event (TE_KEY_* id, down)"),
    "on_knob": dict(kind="entry_hooks", func="0x019b6c0c", expect="00e80000", mode="filter",
                    ret="int", args="int knob, int16_t *diff", call="knob, diff",
                    what="KeyHandler thread: every knob step (0..3 blue/green/white/orange)"),
    "midi_in": dict(kind="entry_hooks", func="0x01a85c20", expect="00e80400", mode="filter",
                    ret="int", args="const void *msg", call="msg",
                    what="every MIDI message (te_bytes *) entering VoiceCtl's dispatcher"),
    "kb_note_on": dict(kind="call_sites", site="0x019ac290", expect="0x01a85750", mode="filter",
                       ret="int", args="void *note, int extra", call="note, extra",
                       what="keyboard note-on into VoiceCtl (te_note *, extra); filter = TE does not play it"),
    "kb_note_off": dict(kind="call_sites", site="0x019ac3a6", expect="0x01a85840", mode="filter",
                        ret="int", args="void *note", call="note",
                        what="keyboard note-off into VoiceCtl (te_note *)"),
    "midi_note_on": dict(kind="call_sites", site="0x01a85bc6", expect="0x01a85750", mode="filter",
                         ret="int", args="void *note, int extra", call="note, extra",
                         what="MIDI-in note-on into VoiceCtl (te_note *, extra)"),
    "midi_note_off": dict(kind="call_sites", site="0x01a8599e", expect="0x01a85840", mode="filter",
                          ret="int", args="void *note", call="note",
                          what="MIDI-in note-off into VoiceCtl (te_note *), incl. velocity-0 note-ons"),
    "frame": dict(kind="call_sites", site="0x019ab8b2", expect="0x019b2674", mode="after",
                  ret="void", args="uint16_t *buf", call="buf",
                  what="GUI thread: after TE rendered a frame, before flush + present (draw here)"),
    "engine_name": dict(kind="call_sites", site="0x01940374", expect="0x0196fbd0", mode="after",
                        ret="void", args="void *out, void *db, const char *sql", call="out, db, sql",
                        what="engine list pick: the picked synth_types row's type name (te_string *out), before TE uses it"),
    "fx_name": dict(kind="call_sites", site="0x019ff3f4", expect="0x0196fbd0", mode="after",
                    ret="void", args="void *out, void *db, const char *sql", call="out, db, sql",
                    what="FX list pick (synth/drum): the picked row's type name (te_string *out), before TE looks it up"),
    "master_fx_name": dict(kind="call_sites", site="0x019da9b0", expect="0x0196fbd0", mode="after",
                           ret="void", args="void *out, void *db, const char *sql", call="out, db, sql",
                           what="FX list pick (master): the picked row's type name (te_string *out), before TE looks it up"),
    "fx_patch": dict(kind="entry_hooks", func="0x01a01954", expect="00e81f00", mode="observe",
                     ret="void", args="void *patch", call="patch",
                     what="FX browser (synth/drum) asks which row is current: the sound's patch object, before fx_current_name"),
    "fx_current_name": dict(kind="call_sites", site="0x01a01970", expect="0x019be00c", mode="after",
                            ret="void", args="void *node, void *out", call="node, out",
                            what="FX browser (synth/drum): the sound's stored fx_type (te_string *out) before TE looks up its row"),
    "master_fx_current_name": dict(kind="call_sites", site="0x019d97f0", expect="0x0196ef20", mode="after",
                                   ret="void", args="void *out", call="out",
                                   what="FX browser (master): the stored master fx_type (te_string *out) before TE looks up its row"),
    "seq_tick": dict(kind="call_sites", site="0x01a431aa", expect="0x01a0d2c0", mode="after",
                     ret="void", args="void", call="",
                     what="Seq thread: after TE's tick, once per 64-frame audio block (~689 Hz)"),
    "boot_done": dict(kind="call_sites", site="0x019f09d6", expect="0x019d2d1e", mode="after",
                      ret="void", args="void", call="",
                      what="main thread: once, right before TE's main loop"),
    "main_tick": dict(kind="call_sites", site="0x019f0a6a", expect="0x019d2d1c", mode="after",
                      ret="void", args="void", call="",
                      what="main thread: every main-loop pass"),
}


def load_feature(name):
    entry = name if isinstance(name, dict) else {"name": name}
    path = os.path.join(FEATURES, entry["name"], "feature.json")
    if not os.path.exists(path):
        hint = (f": features.py install {entry['repository']}" + (f" --ref {entry['ref']}" if entry.get("ref") else "")
                if entry.get("repository") else "")
        sys.exit(f"unknown feature '{entry['name']}' (no {os.path.relpath(path, SDK)}){hint}")
    f = json.load(open(path))
    f["dir"] = os.path.dirname(path)
    if f.get("sdk", 1) > fpkg.SDK_API:
        sys.exit(f"{f['name']}: needs SDK API {f['sdk']}, this SDK has {fpkg.SDK_API}")
    return f


def all_features():
    return sorted(d for d in os.listdir(FEATURES) if os.path.exists(os.path.join(FEATURES, d, "feature.json")))


def hook_addr(kind, h):
    return int(h["site" if kind == "call_sites" else "func" if kind == "entry_hooks" else "addr"], 0)


def resolve(names):
    """feature list in dependency order, with all checks"""
    order, seen = [], set()

    def add(entry, chain=()):
        n = entry["name"] if isinstance(entry, dict) else entry
        if n in chain:
            sys.exit(f"dependency cycle: {' -> '.join(chain + (n,))}")
        if n not in seen:
            f = load_feature(entry)
            for d in fpkg.dep_entries(f):
                add(d, chain + (n,))
            seen.add(n)
            order.append(f)
        f = next(x for x in order if x["name"] == n)
        want = entry.get("version") if isinstance(entry, dict) else None
        if want and not fpkg.version_ok(f.get("version"), want):
            sys.exit(f"{chain[-1] if chain else 'recipe'} needs {n} {want}, installed: {f.get('version')}")

    for n in names:
        add(n)
    names = {f["name"] for f in order}
    for f in order:
        for c in f.get("conflicts", []):
            if c in names:
                sys.exit(f"feature conflict: {f['name']} declares a conflict with {c}")
    shas = [set(f.get("firmware", {}).values()) for f in order]
    common = set.intersection(*shas) if shas else set()
    if not common:
        sys.exit("no stock firmware supported by all of: " + ", ".join(f["name"] for f in order))
    # exclusive addresses must be unique, and must not be a shared point's address
    owner = {}
    for p, d in POINTS.items():
        owner[int(d.get("site", d.get("func", "0")), 0)] = f"shared point '{p}'"
    for f in order:
        for kind, hooks in f.get("hooks", {}).items():
            for h in hooks:
                a = hook_addr(kind, h)
                if a in owner:
                    sys.exit(f"hook collision at {a:#010x}: {f['name']} and {owner[a]}")
                owner[a] = f"feature '{f['name']}'"
        for s in f.get("shared", []):
            if s["point"] not in POINTS:
                sys.exit(f"{f['name']}: unknown shared point '{s['point']}' (known: {', '.join(POINTS)})")
    errors, notes = fpkg.check_claims(order)
    for n in notes:
        print(f"note: input shared while a panel is open: {n}")
    if errors:
        sys.exit("input claim conflict:\n  " + "\n  ".join(errors))
    return order, common


def dispatcher(order):
    """C source chaining every feature's handler per shared point; returns (code, used points, init)"""
    lines = ["/* generated by tools/compose.py - do not edit */", "#include <stdint.h>", ""]
    used = {}
    for f in order:
        for s in f.get("shared", []):
            used.setdefault(s["point"], []).append((f["name"], s["fn"], s.get("mode", POINTS[s["point"]]["mode"]),
                                                   {"first": -1, "last": 1}.get(s.get("order"), 0)))
    # optional "order": "first" / "last" moves a handler to the front / end of its point (stable;
    # e.g. an overlay that must draw over full-screen pages while its input handlers run early)
    used = {p: [h[:3] for h in sorted(hs, key=lambda h: h[3])] for p, hs in used.items()}
    for p, handlers in used.items():
        d = POINTS[p]
        for _, fn, mode in handlers:
            ret = "int" if mode == "filter" else "void"
            lines.append(f"{ret} {fn}({d['args']});")
    inits = [f["init"] for f in order if f.get("init")]
    for fn in inits:
        lines.append(f"void {fn}(void);")
    lines.append("")
    for p, handlers in used.items():
        d = POINTS[p]
        lines.append(f"/* {p}: {d['what']} */")
        lines.append(f"{d['ret']} compose_{p}({d['args']})")
        lines.append("{")
        for name, fn, mode in [h for h in handlers if h[2] == "observe"] + [h for h in handlers if h[2] != "observe"]:
            if mode == "filter":
                lines.append(f"    if ({fn}({d['call']}))  /* {name} */")
                lines.append("        return 1;")
            else:
                lines.append(f"    {fn}({d['call']});  /* {name} */")
        if d["ret"] == "int":
            lines.append("    return 0;")
        lines.append("}")
        lines.append("")
    if inits:
        lines.append("void compose_init(void)")
        lines.append("{")
        lines += [f"    {fn}();" for fn in inits]
        lines.append("}")
    return "\n".join(lines) + "\n", used, bool(inits)


def compose(names, name, boot_text=None, out=None):
    """generate the project directory; returns (project dir, project.json dict, firmware SHAs)"""
    order, shas = resolve(names)
    out = out or os.path.join(SDK, "build", "apps", name)
    os.makedirs(out, exist_ok=True)
    code, used, has_init = dispatcher(order)
    open(os.path.join(out, "_compose.c"), "w").write(code)
    sources, bases, libs, cflags = ["_compose.c"], {"_compose.c": "composer"}, [], []
    hooks = {"call_sites": [], "entry_hooks": [], "pointers": [], "patches": []}
    content_sql, content_files = [], []
    for f in order:
        content_sql += f.get("content_sql", [])
        for cf in f.get("content_files", []):   # files added to the update (+ their SQL), feature-relative
            src = os.path.join(f["dir"], cf["from"])
            sql = cf.get("sql")
            sql_src = os.path.join(f["dir"], sql["from"]) if sql else None
            if not os.path.exists(src) or (sql_src and not os.path.exists(sql_src)):
                if cf.get("optional"):
                    print(f"note: {f['name']}: {cf['from']} not there, {cf['file']} left out (optional)")
                    continue
                sys.exit(f"{f['name']}: content file {cf['from']} missing")
            content_files.append({"file": cf["file"], "from": os.path.abspath(src)})
            if sql:
                stmts = [q.strip() for q in open(sql_src).read().split(";\n") if q.strip()]
                content_sql.append({"file": sql["file"], "sql": stmts})
        for src in f["sources"]:
            b = os.path.basename(src)
            if b in bases:
                sys.exit(f"source name clash: {b} in {f['name']} and {bases[b]} (object files collide)")
            bases[b] = f["name"]
            sources.append(os.path.relpath(os.path.join(f["dir"], src), out))
        libs += [l for l in f.get("libs", []) if l not in libs]
        cflags += [c for c in f.get("cflags", []) if c not in cflags]
        for kind, hs in f.get("hooks", {}).items():
            hooks[kind] += [{k: v for k, v in h.items() if k != "what"} for h in hs]
    for p in used:
        d = POINTS[p]
        h = {k: d[k] for k in ("site", "func", "expect") if k in d}
        h["c"] = f"compose_{p}"
        if d["mode"] == "filter":
            h["filter"] = True
        elif d["mode"] == "after":
            h["after"] = True                   # "observe": C first, then TE (no flag)
        hooks[d["kind"]].append(h)
    proj = {"name": name, "sources": sources, "libs": libs}
    if cflags:
        proj["cflags"] = cflags
    if has_init:
        proj["init"] = "compose_init"
    if boot_text:
        proj["boot_text"] = boot_text
    proj.update({k: v for k, v in hooks.items() if v})
    if content_sql:
        proj["content_sql"] = content_sql
    if content_files:
        proj["content_files"] = content_files
    proj["features"] = [f"{f['name']} {f.get('version', '')}".strip() for f in order]
    json.dump(proj, open(os.path.join(out, "project.json"), "w"), indent=2)
    return out, proj, shas


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recipe", nargs="?", help="recipes/<name>.json")
    ap.add_argument("firmware", nargs="?", help="stock op1_246.op1 (omit with --no-build)")
    ap.add_argument("--features", help="comma-separated feature names instead of a recipe")
    ap.add_argument("--name", default="custom")
    ap.add_argument("--boot-text")
    ap.add_argument("--no-build", action="store_true")
    a = ap.parse_args()
    if a.features:
        if a.recipe and not a.firmware:
            a.firmware, a.recipe = a.recipe, None
        names, name, boot = a.features.split(","), a.name, a.boot_text
    elif a.recipe:
        r = json.load(open(a.recipe))
        names, name, boot = r["features"], r["name"], a.boot_text or r.get("boot_text")
    else:
        ap.error("give a recipe or --features")
    out, proj, shas = compose(names, name, boot)
    print(f"{name}: {', '.join(proj['features'])} -> {os.path.relpath(out, SDK)}/project.json")
    if a.no_build:
        return
    if not a.firmware:
        sys.exit("firmware path needed to build (or --no-build)")
    sha = hashlib.sha256(open(a.firmware, "rb").read()).hexdigest()
    if sha not in shas:
        sys.exit(f"{a.firmware}: SHA-256 {sha[:12]}... is not a firmware these features were built for")
    subprocess.run([sys.executable, os.path.join(HERE, "build.py"), out, a.firmware], check=True)


if __name__ == "__main__":
    main()
