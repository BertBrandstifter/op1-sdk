#!/usr/bin/env python3
"""nodsynth bench: the feature's content_sql, run through build.py's run_sql.

  1. synthetic databases (no firmware needed): the dsynth engine row and its presets go, every other
     row stays with its id; a dsynth slot gets the 'piano of love' patch byte for byte, the other
     slots stay; without that preset the update fails instead of writing an empty slot.
  2. with the stock firmware (OP1_FIRMWARE or ../additional/org op1 firmware/op1_246.op1): the built
     update has no dsynth in the engine list, the presets or the default slots, and the rest of
     op1_factory.db is unchanged.
Exit 0 = pass."""

import contextlib, hashlib, io, json, os, sqlite3, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("OP1_SDK") or os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(SDK, "tools"))
sys.path.insert(0, os.path.join(SDK, "sim"))
import build  # noqa: E402
import op1pack  # noqa: E402

FEAT = json.load(open(os.path.join(HERE, "feature.json")))
fails = []


def check(name, ok):
    print(f"  {'ok  ' if ok else 'FAIL'} {name}")
    if not ok:
        fails.append(name)


def db(statements):
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "x.db")
        con = sqlite3.connect(p)
        for q in statements:
            con.execute(q)
        con.commit()
        con.close()
        return open(p, "rb").read()


def rows(raw, q):
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "x.db")
        open(p, "wb").write(raw)
        con = sqlite3.connect(p)
        try:
            return con.execute(q).fetchall()
        finally:
            con.close()


def apply(members):
    """members: {name: bytes}; runs the feature's content_sql like build.patch_firmware does"""
    for cs in FEAT["content_sql"]:
        att = {a: members[n] for a, n in cs.get("attach", {}).items()}
        members[cs["file"]] = build.run_sql(members[cs["file"]], cs["sql"], att)
    return members


def patch(name, typ):
    return json.dumps({"name": name, "type": typ, "knobs": [1, 2, 3, 4, 5, 6, 7, 8]})


def synthetic():
    print("1. synthetic databases")
    piano = patch("piano of love", "string")
    fac = db(['CREATE TABLE "synth_types" ("id" INTEGER PRIMARY KEY NOT NULL, "type" TEXT, "default_params" TEXT)',
              "INSERT INTO synth_types VALUES (8, 'string', '[]'), (9, 'dsynth', '[]'), (10, 'voltage', '[]'), "
              "(11, 'acid 3', '[]')",
              'CREATE TABLE "synth_presets" ("id" INTEGER PRIMARY KEY NOT NULL, "patch" TEXT NOT NULL, "folder" TEXT)',
              f"INSERT INTO synth_presets VALUES (49, '{patch('back bass', 'dsynth')}', 'dsynth'), "
              f"(56, '{patch('legacy', 'dsynth')}', 'dsynth'), (139, '{patch('mearth', 'string')}', 'string'), "
              f"(140, '{piano}', 'string')"])
    slots = db(['CREATE TABLE "synth_slots" ("id" INTEGER PRIMARY KEY NOT NULL, "patch" TEXT NOT NULL)',
                f"INSERT INTO synth_slots VALUES (3, '{patch('grams organ', 'drwave')}'), "
                f"(4, '{patch('legacy', 'dsynth')}'), (5, '{patch('rez bass', 'fm')}')"])
    m = apply({"content/op1_factory.db": fac, "content/op1.db": slots})
    f, o = m["content/op1_factory.db"], m["content/op1.db"]
    check("engine row gone, the others keep their ids",
          rows(f, "SELECT id, type FROM synth_types ORDER BY id") == [(8, "string"), (10, "voltage"), (11, "acid 3")])
    check("dsynth presets gone, the others stay",
          rows(f, "SELECT id FROM synth_presets ORDER BY id") == [(139,), (140,)])
    check("the dsynth slot holds 'piano of love' byte for byte",
          rows(o, "SELECT patch FROM synth_slots WHERE id = 4") == [(piano,)])
    check("the other slots unchanged",
          [r[0] for r in rows(o, "SELECT id FROM synth_slots WHERE json_extract(patch, '$.type') != 'dsynth'")] == [3, 4, 5]
          and json.loads(rows(o, "SELECT patch FROM synth_slots WHERE id = 5")[0][0])["name"] == "rez bass")
    fac2 = build.run_sql(fac, ["DELETE FROM synth_presets WHERE id = 140"])
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            apply({"content/op1_factory.db": fac2, "content/op1.db": slots})
        check("no 'piano of love': the build fails", False)
    except sqlite3.IntegrityError:
        check("no 'piano of love': the build fails", True)


def firmware():
    for path in (os.environ.get("OP1_FIRMWARE"),
                 os.path.join(SDK, "..", "additional", "org op1 firmware", "op1_246.op1")):
        if path and os.path.exists(path) and hashlib.sha256(open(path, "rb").read()).hexdigest() == build.STOCK_SHA256:
            return path
    return None


def real(fw):
    print("2. stock databases")
    stock = {m.name: d for m, d in op1pack.read(fw) if m.name.endswith(".db")}
    m = apply(dict(stock))
    f, o = m["content/op1_factory.db"], m["content/op1.db"]
    check("no dsynth in the engine list", not rows(f, "SELECT 1 FROM synth_types WHERE type = 'dsynth'"))
    check("no dsynth preset left (any folder)",
          not rows(f, "SELECT 1 FROM synth_presets WHERE folder = 'dsynth' OR json_extract(patch, '$.type') = 'dsynth'"))
    n0, n1 = (rows(x, "SELECT COUNT(*) FROM synth_presets")[0][0] for x in (stock["content/op1_factory.db"], f))
    check(f"presets {n0} -> {n1} (16 fewer)", n0 - n1 == 16)
    check("default slot 5 is 'piano of love' (string)",
          rows(o, "SELECT json_extract(patch, '$.name'), json_extract(patch, '$.type') FROM synth_slots WHERE id = 4")
          == [("piano of love", "string")])
    same = all(rows(stock["content/op1_factory.db"], f"SELECT * FROM {t} ORDER BY 1") == rows(f, f"SELECT * FROM {t} ORDER BY 1")
               for t in ("drum_presets", "drum_types", "fx_types", "lfo_types", "seq_types"))
    check("the other factory tables unchanged", same)
    check("the other slots unchanged",
          rows(stock["content/op1.db"], "SELECT * FROM synth_slots WHERE id != 4") == rows(o, "SELECT * FROM synth_slots WHERE id != 4"))


def main():
    synthetic()
    fw = firmware()
    if fw:
        real(fw)
    else:
        print("2. skipped: no stock firmware (set OP1_FIRMWARE)")
    print("PASS" if not fails else f"FAIL: {', '.join(fails)}")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
