"""OP-1 feature packages (spec: docs/feature-package.md): validate, pack, install, scaffold.

Used by features.py (check / pack / install / new) and compose.py (claims, versions, SDK level).
Nothing here runs code from a package: install = git clone or zip extract + static checks + a
cross-compile of the sources (symbol check). Building and flashing stay explicit user steps."""

import hashlib, json, os, re, shutil, stat, subprocess, sys, tempfile, urllib.request, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(HERE)
FEATURES = os.path.join(SDK, "features")
LOCK = os.path.join(SDK, "features.lock.json")

SDK_API = 1          # bump when shared points, headers or dispatch semantics change incompatibly
SCHEMA = 1           # feature.json schema of docs/feature-package.md

NAME_RE = re.compile(r"^[a-z][a-z0-9_]{1,31}$")
VERSION_RE = re.compile(r"^(\d+)\.(\d+)(?:\.(\d+))?$")
PREFIX_RE = re.compile(r"^[a-z][a-z0-9]*_$")
SHA_RE = re.compile(r"^[0-9a-f]{64}$")
STATUSES = ("experimental", "sim-verified", "device-verified")

# input claims: <control>_<action>, screens = TE_EDIT_MODE-level screens
KEYS = ("speech", "metronome", "synth", "drum", "tape", "mixer", "t1", "t2", "t3", "t4", "cut",
        "insert", "join", "sound1", "sound2", "sound3", "sound4", "sound5", "sound6", "sound7",
        "sound8", "auto", "rec", "play", "stop", "back", "forward", "mic", "com", "keyboard")
KNOBS = ("blue", "green", "white", "orange")
INPUTS = {f"{k}_press" for k in KEYS} | {f"{k}_{a}" for k in KNOBS for a in ("turn", "push")}
SCREENS = ("any", "synth", "drum", "tape", "mixer", "tempo", "sequencer", "album")
SHIFT = ("no", "yes", "any")
PLAYMODES = ("any", "synth", "drum")

# never in a package: TE firmware, images, dumps, disassembly, build output
FORBIDDEN_EXT = (".op1", ".ldr", ".dis", ".bin", ".elf", ".o", ".dfu")
MAX_FILE = 1 << 20
# zip packages: limits on what install extracts (a package is sources + docs, far below these)
MAX_ZIP = 16 << 20
MAX_ZIP_FILES = 2000
SKIP_DIRS = (".git", "build", "__pycache__", "__MACOSX")
SKIP_FILES = (".DS_Store",)


def version_tuple(v):
    m = VERSION_RE.match(str(v))
    return tuple(int(x or 0) for x in m.groups()) if m else None


def version_ok(v, spec):
    """spec: space-separated constraints, e.g. ">=1.8 <2"; empty = any"""
    have = version_tuple(v)
    if have is None:
        return False
    for c in (spec or "").split():
        m = re.match(r"^(>=|<=|==|>|<|=)?(.+)$", c)
        op, want = m.group(1) or "==", version_tuple(m.group(2))
        if want is None:
            raise ValueError(f"bad version constraint '{c}'")
        ok = {">=": have >= want, "<=": have <= want, ">": have > want, "<": have < want,
              "==": have == want, "=": have == want}[op]
        if not ok:
            return False
    return True


def dep_entries(f):
    """depends as [{"name", "version", "repository"}] (plain strings allowed)"""
    return [d if isinstance(d, dict) else {"name": d} for d in f.get("depends", [])]


def claims(f):
    out = []
    for c in f.get("claims", []):
        c = dict(c)
        c.setdefault("screen", "any")
        c.setdefault("shift", "no")
        c.setdefault("playmode", "any")
        c.setdefault("modal", False)
        out.append(c)
    return out


def screens(c):
    """a claim's screens as a set ("screen" is one name or a list)"""
    s = c.get("screen", "any")
    return set(s) if isinstance(s, list) else {s}


def claim_overlap(a, b):
    """same input, compatible shift state, overlapping screen and playmode"""
    sa, sb = screens(a), screens(b)
    return (a["input"] == b["input"]
            and (a["shift"] == b["shift"] or "any" in (a["shift"], b["shift"]))
            and ("any" in sa or "any" in sb or bool(sa & sb))
            and (a["playmode"] == b["playmode"] or "any" in (a["playmode"], b["playmode"])))


def check_claims(order):
    """errors for two always-on claims on the same input; modal claims (only while a feature's own
    panel is open) are reported as notes"""
    errors, notes = [], []
    owned = [(f["name"], c) for f in order for c in claims(f)]
    for i, (fa, a) in enumerate(owned):
        for fb, b in owned[i + 1:]:
            if fa == fb or not claim_overlap(a, b):
                continue
            what = (f"{'shift+' if a['shift'] == 'yes' else ''}{a['input']} on {a['screen']}"
                    f" ({fa}) vs {b['screen']} ({fb})")
            (notes if a["modal"] or b["modal"] else errors).append(what)
    return errors, notes


# ------------------------------------------------------------------ validation

def _inside(root, path):
    p = os.path.realpath(os.path.join(root, path))
    return p == os.path.realpath(root) or p.startswith(os.path.realpath(root) + os.sep)


def validate(fdir, compile_check=True, package=None):
    """static checks of one feature directory; returns (errors, warnings). package=True applies the
    publishing rules in full (default: for anything not developed in this repo's features/, and for
    features installed from a package)"""
    from compose import POINTS  # noqa: E402 (late: compose imports this module)
    E, W = [], []
    path = os.path.join(fdir, "feature.json")
    try:
        f = json.load(open(path))
    except (OSError, ValueError) as e:
        return [f"feature.json: {e}"], []
    in_features = os.path.realpath(os.path.dirname(fdir)) == os.path.realpath(FEATURES)
    if package is None:
        package = not in_features or f.get("name") in read_lock()
    in_repo = not package

    if f.get("schema") != SCHEMA:
        E.append(f"schema: must be {SCHEMA}")
    name = f.get("name", "")
    if not NAME_RE.match(name):
        E.append("name: [a-z][a-z0-9_]{1,31}")
    elif in_features and not os.path.basename(fdir).startswith(".") and os.path.basename(os.path.normpath(fdir)) != name:
        E.append(f"name '{name}' does not match its directory features/{os.path.basename(fdir)}")
    if not version_tuple(f.get("version")):
        E.append("version: MAJOR.MINOR[.PATCH]")
    for k in ("title", "summary", "license"):
        if not isinstance(f.get(k), str) or not f[k].strip() or f[k] == "TODO":
            E.append(f"{k}: required")
    if not f.get("authors") or not all(isinstance(a, str) and a for a in f["authors"]):
        E.append("authors: non-empty list of names")
    if f.get("status") not in STATUSES:
        E.append(f"status: one of {', '.join(STATUSES)}")
    sdk = f.get("sdk")
    if not isinstance(sdk, int) or sdk < 1:
        E.append("sdk: SDK API level (integer)")
    elif sdk > SDK_API:
        E.append(f"sdk: needs SDK API {sdk}, this SDK has {SDK_API} (update the SDK)")
    fw = f.get("firmware")
    if not isinstance(fw, dict) or not fw or not all(SHA_RE.match(str(v)) for v in fw.values()):
        E.append('firmware: {"R246": "<sha256 of the stock .op1>"}')
    prefixes = f.get("prefix")
    prefixes = [prefixes] if isinstance(prefixes, str) else prefixes or []
    if not prefixes or not all(PREFIX_RE.match(p) for p in prefixes):
        E.append('prefix: symbol prefix like "rq_" (or a list)')

    srcs = f.get("sources", [])
    if not srcs and not (f.get("content_files") or f.get("content_sql")):
        E.append("sources: at least one .c/.S file (or content_files / content_sql for a content-only feature)")
    for s in srcs:
        if not _inside(fdir, s):
            E.append(f"{s}: outside the package")
        elif not os.path.exists(os.path.join(fdir, s)):
            E.append(f"{s}: missing")
    for kind, paths in (("docs", [f["docs"]] if f.get("docs") else []), ("test", f.get("tests", []))):
        for t in paths:
            if os.path.exists(os.path.join(fdir, t)) and _inside(fdir, t):
                continue
            if in_repo and os.path.exists(os.path.join(SDK, t)):
                W.append(f"{kind} {t}: outside the package (move it in before publishing)")
            else:
                E.append(f"{kind} {t}: missing")
    if not f.get("tests"):
        W.append("tests: none (a host test or sim bench is expected for sim-verified)")
    if f.get("status") == "device-verified" and not f.get("evidence"):
        W.append("status device-verified without evidence entries")
    for e in f.get("evidence", []):
        if not {"firmware", "date", "what"} <= set(e):
            E.append("evidence: entries need firmware, date, what")

    for s in f.get("shared", []):
        p = POINTS.get(s.get("point"))
        if not p:
            E.append(f"shared: unknown point '{s.get('point')}' (known: {', '.join(POINTS)})")
            continue
        mode = s.get("mode", p["mode"])
        allowed = {"filter": ("observe", "filter"), "observe": ("observe",)}.get(p["mode"], ("after",))
        if mode not in allowed:
            E.append(f"shared {s['point']}: mode '{mode}' not allowed")
        if not any(s.get("fn", "").startswith(x) for x in prefixes):
            E.append(f"shared {s['point']}: fn '{s.get('fn')}' lacks the prefix")
    for kind, hs in f.get("hooks", {}).items():
        if kind not in ("call_sites", "entry_hooks", "pointers", "patches"):
            E.append(f"hooks: unknown kind '{kind}'")
            continue
        for h in hs:
            if "expect" not in h:
                E.append(f"hooks {kind}: every hook needs 'expect' (checked against the stock image)")
            if not h.get("what"):
                W.append(f"hooks {kind} {h.get('site') or h.get('func') or h.get('addr')}: add 'what'")
    for c in f.get("claims", []):
        if c.get("input") not in INPUTS:
            E.append(f"claims: unknown input '{c.get('input')}' (see docs/feature-package.md)")
        if not screens(c) or not screens(c) <= set(SCREENS):
            E.append(f"claims: unknown screen '{c.get('screen')}'")
        if c.get("shift", "no") not in SHIFT:
            E.append(f"claims: shift must be one of {', '.join(SHIFT)}")
        if c.get("playmode", "any") not in PLAYMODES:
            E.append(f"claims: playmode must be one of {', '.join(PLAYMODES)}")
    if "claims" not in f:
        E.append("claims: required (empty list if the feature takes no input)")
    for d in dep_entries(f):
        if not NAME_RE.match(d.get("name", "")):
            E.append(f"depends: bad entry {d}")
        elif d.get("version"):
            try:
                version_ok("0.0", d["version"])
            except ValueError as e:
                E.append(f"depends {d['name']}: {e}")
    if not os.path.exists(os.path.join(fdir, "README.md")):
        (W if in_repo else E).append("README.md: missing")
    if not os.path.exists(os.path.join(fdir, "LICENSE")) and not in_repo:
        E.append("LICENSE: missing")
    if not in_repo and not f.get("repository"):
        W.append("repository: add the URL people install from")

    for root, dirs, files in os.walk(fdir):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]      # never packed or installed
        for n in files:
            p = os.path.join(root, n)
            rel = os.path.relpath(p, fdir)
            if n.lower().endswith(FORBIDDEN_EXT):
                (W if in_repo and rel.startswith("build" + os.sep) else E).append(
                    f"{rel}: binaries / TE images / dumps never go into a package")
            elif os.path.getsize(p) > MAX_FILE:
                E.append(f"{rel}: larger than 1 MB")

    if compile_check and not E:
        E += _symbol_check(fdir, f, prefixes)
    return E, W


def _symbol_check(fdir, f, prefixes):
    """every global symbol the sources define starts with the prefix (all features link into one
    image: a plain 'grid' in two features is a link error)"""
    sys.path.insert(0, HERE)
    from build import BIN, CFLAGS  # noqa: E402
    E = []
    tmp = tempfile.mkdtemp()
    try:
        for s in f.get("sources", []):
            obj = os.path.join(tmp, "x.o")
            r = subprocess.run([BIN + "gcc", *CFLAGS, *f.get("cflags", []), "-I", os.path.dirname(fdir),
                                "-c", os.path.join(fdir, s), "-o", obj], capture_output=True, text=True)
            if r.returncode:
                E.append(f"{s}: does not compile\n{r.stderr[-1500:]}")
                continue
            out = subprocess.run([BIN + "nm", "-g", "--defined-only", obj], capture_output=True,
                                 text=True).stdout
            for row in out.splitlines():
                sym = row.split()[-1].lstrip("_") if row.split() else ""
                if sym and not any(sym.startswith(p) for p in prefixes):
                    E.append(f"{s}: global symbol '{sym}' lacks the prefix {'/'.join(prefixes)} (make it static or rename)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return E


# ------------------------------------------------------------------ install

def read_lock():
    return json.load(open(LOCK)) if os.path.exists(LOCK) else {}


def _git(*args, cwd=None):
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout.strip()


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fp:
        for chunk in iter(lambda: fp.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def _is_zip_source(source):
    return source.lower().split("?")[0].endswith(".zip")


def _fetch_zip(source, tmp):
    """local path or http(s) URL of a .zip -> local path (downloads are capped at MAX_ZIP)"""
    if not re.match(r"^https?://", source):
        if not os.path.isfile(source):
            sys.exit(f"{source}: no such file")
        return source
    path = os.path.join(tmp, "download.zip")
    with urllib.request.urlopen(source, timeout=60) as r, open(path, "wb") as out:
        n = 0
        while chunk := r.read(1 << 16):
            n += len(chunk)
            if n > MAX_ZIP:
                sys.exit(f"{source}: larger than {MAX_ZIP >> 20} MB")
            out.write(chunk)
    return path


def _extract_zip(path, dst):
    """safe extract: no absolute paths, no '..', no links, size / count limits; skips macOS junk.
    Returns the package root: dst itself, or its single top directory (GitHub 'Download ZIP',
    features.py pack) when feature.json is there."""
    try:
        z = zipfile.ZipFile(path)
    except zipfile.BadZipFile as e:
        sys.exit(f"{path}: {e}")
    with z:
        infos = z.infolist()
        if len(infos) > MAX_ZIP_FILES:
            sys.exit(f"{path}: more than {MAX_ZIP_FILES} entries")
        if sum(i.file_size for i in infos) > MAX_ZIP:
            sys.exit(f"{path}: unpacks to more than {MAX_ZIP >> 20} MB")
        os.makedirs(dst)
        for i in infos:
            name = i.filename.replace("\\", "/")
            parts = [p for p in name.split("/") if p not in ("", ".")]
            if name.startswith("/") or re.match(r"^[A-Za-z]:", name) or ".." in parts:
                sys.exit(f"{path}: unsafe path '{i.filename}'")
            mode = i.external_attr >> 16
            if stat.S_ISLNK(mode):
                sys.exit(f"{path}: '{i.filename}' is a link (not allowed in a package)")
            if not parts or any(p in SKIP_DIRS for p in parts[:-1]) or parts[-1] in SKIP_FILES \
                    or parts[0] in SKIP_DIRS:
                continue
            out = os.path.join(dst, *parts)
            if i.is_dir():
                os.makedirs(out, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with z.open(i) as src, open(out, "wb") as fp:
                shutil.copyfileobj(src, fp)
    if os.path.exists(os.path.join(dst, "feature.json")):
        return dst
    top = os.listdir(dst)
    if len(top) == 1 and os.path.exists(os.path.join(dst, top[0], "feature.json")):
        return os.path.join(dst, top[0])
    sys.exit(f"{path}: no feature.json at the top of the zip (or in its single top directory)")


def install(source, ref=None, force=False, _seen=None):
    """git URL, .zip (path or http(s) URL) or local directory [ref = tag/branch/commit, git only]
    -> features/<name>, recorded in features.lock.json. Dependencies with a "repository" are
    installed too."""
    _seen = _seen if _seen is not None else set()
    tmp = tempfile.mkdtemp()
    try:
        dst = os.path.join(tmp, "pkg")
        zip_sha = None
        if _is_zip_source(source):
            if ref:
                sys.exit("--ref applies to git repositories, not to a .zip")
            zpath = _fetch_zip(source, tmp)
            zip_sha = _sha256(zpath)
            dst = _extract_zip(zpath, os.path.join(tmp, "zip"))
            commit = None
        elif os.path.isdir(source) and not os.path.isdir(os.path.join(source, ".git")):
            shutil.copytree(source, dst, ignore=shutil.ignore_patterns(*SKIP_DIRS, *SKIP_FILES))
            commit = None
        else:
            _git("clone", "--quiet", source, dst)
            if ref:
                _git("checkout", "--quiet", ref, cwd=dst)
            commit = _git("rev-parse", "HEAD", cwd=dst)
            shutil.rmtree(os.path.join(dst, ".git"))
        f = json.load(open(os.path.join(dst, "feature.json")))
        name = f.get("name", "")
        if not NAME_RE.match(name):
            sys.exit(f"{source}: bad feature name '{name}'")
        lock = read_lock()
        target = os.path.join(FEATURES, name)
        if os.path.exists(target) and name not in lock and not force:
            sys.exit(f"features/{name} exists and was not installed from a package (local feature); "
                     "rename one of them or pass --force")
        # validate in place under features/ (dependency headers resolve side by side)
        stage = os.path.join(FEATURES, f".{name}.install")
        shutil.rmtree(stage, ignore_errors=True)
        shutil.copytree(dst, stage)
        try:
            errors, warnings = validate(stage, package=True)
            if errors:
                sys.exit(f"{name}: not a valid package:\n  " + "\n  ".join(errors))
            shutil.rmtree(target, ignore_errors=True)
            os.rename(stage, target)
        finally:
            shutil.rmtree(stage, ignore_errors=True)
        lock[name] = {"source": source, "ref": ref, "commit": commit, "version": f["version"]}
        if zip_sha:
            lock[name]["sha256"] = zip_sha
        json.dump(lock, open(LOCK, "w"), indent=2, sort_keys=True)
        print(f"installed {name} {f['version']} ({f.get('status')}) -> features/{name}"
              + (f" @ {commit[:12]}" if commit else "") + (f" (zip sha256 {zip_sha[:16]}...)" if zip_sha else ""))
        for w in warnings:
            print(f"  warning: {w}")
        _seen.add(name)
        for d in dep_entries(f):
            if d["name"] in _seen:
                continue
            have = os.path.join(FEATURES, d["name"], "feature.json")
            if os.path.exists(have) and version_ok(json.load(open(have)).get("version"), d.get("version")):
                continue
            if d.get("repository"):
                install(d["repository"], d.get("ref"), force, _seen)
            else:
                print(f"  missing dependency {d['name']} {d.get('version', '')} (no repository given)")
        return name
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ------------------------------------------------------------------ pack

def pack(target, out_dir=None):
    """features/<name> (or a feature dir) -> <out_dir>/<name>-<version>.zip, after the full
    publishing checks. Reproducible: sorted entries, fixed timestamps and modes, one top directory
    <name>-<version>/, so the same sources give the same sha256."""
    fdir = target if os.path.exists(os.path.join(target, "feature.json")) else os.path.join(FEATURES, target)
    fdir = os.path.abspath(fdir)
    errors, warnings = validate(fdir, package=True)
    if errors:
        sys.exit(f"{target}: not publishable yet:\n  " + "\n  ".join(errors))
    f = json.load(open(os.path.join(fdir, "feature.json")))
    base = f"{f['name']}-{f['version']}"
    out_dir = out_dir or os.path.join(SDK, "build", "packages")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, base + ".zip")
    files = []
    for root, dirs, names in os.walk(fdir):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS and not d.startswith("."))
        for n in sorted(names):
            if n in SKIP_FILES or n.startswith("."):
                continue
            p = os.path.join(root, n)
            if os.path.islink(p):
                sys.exit(f"{os.path.relpath(p, fdir)}: links are not allowed in a package")
            files.append(p)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for p in files:
            rel = os.path.relpath(p, fdir).replace(os.sep, "/")
            info = zipfile.ZipInfo(f"{base}/{rel}", date_time=(1980, 1, 1, 0, 0, 0))
            info.external_attr = (0o100644 if not os.access(p, os.X_OK) else 0o100755) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(p, "rb") as fp:
                z.writestr(info, fp.read())
    sha = _sha256(out)
    print(f"{os.path.relpath(out, SDK) if out.startswith(SDK) else out}: {len(files)} files, "
          f"{os.path.getsize(out)} bytes\nsha256 {sha}")
    for w in warnings:
        print(f"  warning: {w}")
    print(f"install: python3 tools/features.py install {base}.zip")
    return out, sha


# ------------------------------------------------------------------ scaffold

def scaffold(name, prefix=None, author=None):
    if not NAME_RE.match(name):
        sys.exit("name: [a-z][a-z0-9_]{1,31}")
    prefix = prefix or name.split("_")[0][:4] + "_"
    d = os.path.join(FEATURES, name)
    if os.path.exists(d):
        sys.exit(f"features/{name} exists")
    os.makedirs(d)
    sys.path.insert(0, HERE)
    from build import STOCK_SHA256  # noqa: E402
    manifest = {
        "schema": SCHEMA, "name": name, "title": "TODO", "version": "0.1.0", "status": "experimental",
        "summary": "TODO: one paragraph, what the user gets", "license": "TODO",
        "authors": [author] if author else [], "repository": "", "sdk": SDK_API,
        "firmware": {"R246": STOCK_SHA256}, "prefix": prefix,
        "sources": [f"{name}.c"], "libs": ["gfx"],
        "shared": [{"point": "on_key", "fn": f"{prefix}key", "mode": "filter"},
                   {"point": "frame", "fn": f"{prefix}frame"}],
        "hooks": {},
        "claims": [{"input": "insert_press", "shift": "yes", "screen": "any"}],
        "controls": ["shift + insert: TODO"],
        "tests": [], "depends": [], "conflicts": [], "evidence": [],
    }
    json.dump(manifest, open(os.path.join(d, "feature.json"), "w"), indent=2)
    open(os.path.join(d, f"{name}.c"), "w").write(f"""/* {name}: TODO what it does (feature.json, README.md). */
#include <op1/te246.h>
#include <op1/gfx.h>

static volatile int on;                         /* static: only {prefix}* symbols are global */

/* on_key filter (KeyHandler thread): nonzero = consumed, TE never sees the key */
int {prefix}key(int key, int down)
{{
    if (key != TE_KEY_INSERT || !*TE_SHIFT_HELD)
        return 0;
    if (down)
        on = !on;
    return 1;
}}

/* frame (GUI thread): draw over TE's frame */
void {prefix}frame(uint16_t *buf)
{{
    if (!on)
        return;
    gfx_begin(buf);
    gfx_text(2, 2, "{name.upper()}", GFX_WHITE, 2);
}}
""")
    open(os.path.join(d, "README.md"), "w").write(f"""# {name}

TODO: what it does for the player.

## Controls

- shift + insert: TODO

## Install (op1-sdk)

```sh
python3 tools/features.py install <this repository URL> --ref v0.1.0
python3 tools/compose.py --features {name} --name {name} ~/path/to/op1_246.op1
python3 tools/flash.py build/apps/{name}/build/{name}.op1 --eject     # te-boot upload mode
```

Firmware: OP-1 R246 (stock SHA-256 in feature.json). Status: experimental.
""")
    print(f"features/{name}: feature.json, {name}.c, README.md (add a LICENSE, fill the TODOs; "
          f"then features.py check {name})")
