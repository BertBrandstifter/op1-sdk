#!/usr/bin/env python3
"""Feature library catalog: the manifests in features/*/feature.json (source of truth) plus a SQLite
index (features.db, regenerated from them) for querying.

    features.py list                         all features: status, version, title
    features.py show <name>                  one feature: summary, controls, hooks, tests, docs
    features.py points                       shared TE hook points and who uses them
    features.py index                        rebuild features.db
    features.py query "SELECT ..."           run SQL against features.db (rebuilt first)
    features.py check [<name|dir> ...]       validate against the package standard (default: all)
    features.py pack <name|dir> [--out <dir>]   publishable zip (default build/packages/<name>-<version>.zip)
    features.py install <git url|.zip|zip url|dir> [--ref <tag>] [--force]   add a published feature
    features.py new <name> [--prefix p_] [--author "Name"]      scaffold a new feature

Published features: docs/feature-package.md (features.lock.json records what was installed).

Tables: features, firmware, hooks (exclusive), shared (shared hook points used), points,
controls, sources, tests, deps (depends/conflicts), recipes, recipe_features.
Example: features.py query "SELECT feature, address, what FROM hooks ORDER BY address"
"""

import glob, json, os, sqlite3, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from compose import POINTS, all_features, load_feature, hook_addr  # noqa: E402
import fpkg  # noqa: E402

DB = os.path.join(SDK, "features.db")

SCHEMA = """
CREATE TABLE features (name TEXT PRIMARY KEY, title TEXT, version TEXT, status TEXT, status_note TEXT,
                       summary TEXT, docs TEXT, dir TEXT, init TEXT);
CREATE TABLE firmware (feature TEXT, label TEXT, sha256 TEXT);
CREATE TABLE hooks (feature TEXT, kind TEXT, address TEXT, expect TEXT, symbol TEXT, mode TEXT, what TEXT);
CREATE TABLE points (name TEXT PRIMARY KEY, kind TEXT, address TEXT, expect TEXT, mode TEXT, signature TEXT, what TEXT);
CREATE TABLE shared (feature TEXT, point TEXT, fn TEXT, mode TEXT);
CREATE TABLE controls (feature TEXT, control TEXT);
CREATE TABLE sources (feature TEXT, path TEXT);
CREATE TABLE tests (feature TEXT, path TEXT);
CREATE TABLE deps (feature TEXT, other TEXT, kind TEXT, version TEXT);
CREATE TABLE claims (feature TEXT, input TEXT, shift TEXT, screen TEXT, modal INTEGER, playmode TEXT);
CREATE TABLE recipes (name TEXT PRIMARY KEY, boot_text TEXT, file TEXT);
CREATE TABLE recipe_features (recipe TEXT, feature TEXT, position INTEGER);
"""


def index():
    if os.path.exists(DB):
        os.remove(DB)
    db = sqlite3.connect(DB)
    db.executescript(SCHEMA)
    for name, d in POINTS.items():
        db.execute("INSERT INTO points VALUES (?,?,?,?,?,?,?)",
                   (name, d["kind"], d.get("site", d.get("func")), d["expect"], d["mode"],
                    f"{d['ret']} f({d['args']})", d["what"]))
    for n in all_features():
        f = load_feature(n)
        rel = os.path.relpath(f["dir"], SDK)
        db.execute("INSERT INTO features VALUES (?,?,?,?,?,?,?,?,?)",
                   (n, f.get("title"), f.get("version"), f.get("status"), f.get("status_note"),
                    f.get("summary"), f.get("docs"), rel, f.get("init")))
        for label, sha in f.get("firmware", {}).items():
            db.execute("INSERT INTO firmware VALUES (?,?,?)", (n, label, sha))
        for kind, hs in f.get("hooks", {}).items():
            for h in hs:
                mode = "filter" if h.get("filter") else "after" if h.get("after") else "replace" if kind == "pointers" else "before"
                db.execute("INSERT INTO hooks VALUES (?,?,?,?,?,?,?)",
                           (n, kind, f"{hook_addr(kind, h):#010x}", h.get("expect"), h.get("c") or h.get("to"),
                            mode, h.get("what")))
        for s in f.get("shared", []):
            db.execute("INSERT INTO shared VALUES (?,?,?,?)",
                       (n, s["point"], s["fn"], s.get("mode", POINTS[s["point"]]["mode"])))
        for c in f.get("controls", []):
            db.execute("INSERT INTO controls VALUES (?,?)", (n, c))
        for s in f.get("sources", []):
            db.execute("INSERT INTO sources VALUES (?,?)", (n, os.path.join(rel, s)))
        for t in f.get("tests", []):
            db.execute("INSERT INTO tests VALUES (?,?)", (n, t))
        for d in fpkg.dep_entries(f):
            db.execute("INSERT INTO deps VALUES (?,?,?,?)", (n, d["name"], "depends", d.get("version")))
        for o in f.get("conflicts", []):
            db.execute("INSERT INTO deps VALUES (?,?,?,?)", (n, o, "conflicts", None))
        for c in fpkg.claims(f):
            db.execute("INSERT INTO claims VALUES (?,?,?,?,?,?)",
                       (n, c["input"], c["shift"], ",".join(sorted(fpkg.screens(c))), int(bool(c["modal"])),
                        c["playmode"]))
    for path in sorted(glob.glob(os.path.join(SDK, "recipes", "*.json"))):
        r = json.load(open(path))
        db.execute("INSERT INTO recipes VALUES (?,?,?)", (r["name"], r.get("boot_text"), os.path.relpath(path, SDK)))
        for i, n in enumerate(r["features"]):
            db.execute("INSERT INTO recipe_features VALUES (?,?,?)",
                       (r["name"], n["name"] if isinstance(n, dict) else n, i))
    db.commit()
    return db


def opt(flag, default=None):
    if flag in sys.argv:
        i = sys.argv.index(flag)
        v = sys.argv[i + 1] if i + 1 < len(sys.argv) else default
        del sys.argv[i:i + 2]
        return v
    return default


def check(args):
    targets = args or all_features()
    bad = 0
    for t in targets:
        d = t if os.path.isdir(t) and os.path.exists(os.path.join(t, "feature.json")) else os.path.join(fpkg.FEATURES, t)
        errors, warnings = fpkg.validate(os.path.abspath(d))
        label = os.path.relpath(os.path.abspath(d), SDK) if os.path.abspath(d).startswith(SDK) else d
        print(f"{'OK  ' if not errors else 'FAIL'}  {label}")
        for e in errors:
            print(f"      error:   {e}")
        for w in warnings:
            print(f"      warning: {w}")
        bad += bool(errors)
    sys.exit(1 if bad else 0)


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "list"
    if cmd == "check":
        return check(sys.argv[2:])
    if cmd == "install":
        force = "--force" in sys.argv
        if force:
            sys.argv.remove("--force")
        ref = opt("--ref")
        if len(sys.argv) != 3:
            sys.exit(__doc__)
        fpkg.install(sys.argv[2], ref, force)
        index()
        return
    if cmd == "pack":
        out = opt("--out")
        if len(sys.argv) != 3:
            sys.exit(__doc__)
        fpkg.pack(sys.argv[2], out)
        return
    if cmd == "new":
        prefix, author = opt("--prefix"), opt("--author")
        if len(sys.argv) != 3:
            sys.exit(__doc__)
        return fpkg.scaffold(sys.argv[2], prefix, author)
    db = index()
    if cmd == "index":
        print(f"{os.path.relpath(DB, SDK)}: {db.execute('SELECT count(*) FROM features').fetchone()[0]} features")
    elif cmd == "list":
        for row in db.execute("SELECT name, status, version, title FROM features ORDER BY name"):
            print(f"{row[0]:14s} {row[1]:16s} {row[2] or '':5s} {row[3]}")
    elif cmd == "show":
        n = sys.argv[2]
        f = db.execute("SELECT title, version, status, status_note, summary, docs, dir FROM features WHERE name=?", (n,)).fetchone()
        if not f:
            sys.exit(f"unknown feature {n}")
        print(f"{n} {f[1]} — {f[0]}\nstatus: {f[2]}{' (' + f[3] + ')' if f[3] else ''}\n\n{f[4]}\n")
        for (c,) in db.execute("SELECT control FROM controls WHERE feature=?", (n,)):
            print(f"  control: {c}")
        for row in db.execute("SELECT point, fn, mode FROM shared WHERE feature=?", (n,)):
            print(f"  shared:  {row[0]:10s} {row[1]} ({row[2]})")
        for row in db.execute("SELECT kind, address, symbol, mode, what FROM hooks WHERE feature=?", (n,)):
            print(f"  hook:    {row[1]} {row[0][:-1]:10s} -> {row[2]} ({row[3]}) {row[4] or ''}")
        for (t,) in db.execute("SELECT path FROM tests WHERE feature=?", (n,)):
            print(f"  test:    {t}")
        for row in db.execute("SELECT label, sha256 FROM firmware WHERE feature=?", (n,)):
            print(f"  firmware: {row[0]} {row[1][:16]}...")
        print(f"  docs: {f[5] or '-'}   code: {f[6]}/")
    elif cmd == "points":
        for row in db.execute("SELECT name, address, mode, signature, what FROM points"):
            users = [f"{u}:{fn}" for u, fn in db.execute("SELECT feature, fn FROM shared WHERE point=?", (row[0],))]
            print(f"{row[0]:10s} {row[1]} {row[2]:7s} {row[3]}\n           {row[4]}\n           used by: {', '.join(users) or '-'}")
    elif cmd == "query":
        cur = db.execute(sys.argv[2])
        print(" | ".join(d[0] for d in cur.description))
        for row in cur:
            print(" | ".join("" if v is None else str(v) for v in row))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
