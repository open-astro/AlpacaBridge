#!/usr/bin/env python3
"""Layering gate: no new vendor headers in AlpacaHTTP or the catalog schema files.

ADR 0004 (device-catalog) moves per-vendor knowledge out of AlpacaHTTP into
vendor descriptors. AlpacaHTTP/src/http/router.cpp still includes 40
`alpacacore/vendor/...` headers; each descriptor slice deletes some. Nothing
counted them, so a PR could add a 41st, or a slice could forget to delete its
includes, with no signal. The catalog schema files must include none, ever.

Rules (baselines are the constants below, lowered by the PR that deletes the
includes, never raised):
  - AlpacaHTTP/**: at most MAX_ALPACAHTTP_VENDOR_INCLUDES vendor includes.
  - catalog files: at most MAX_CATALOG_SCHEMA_VENDOR_INCLUDES (zero).

Comments and `#if 0` are deliberately NOT stripped: an include inside one still
counts, so the gate cannot be dodged by disabling a line. Do not "fix" this.

Run from the repo root: python3 scripts/check_layering.py
Self-test: python3 scripts/check_layering.py --self-test
"""

from __future__ import annotations

import os
import pathlib
import re
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE_GLOBS = ("*.cpp", "*.h", "*.hpp")

# Lowered by each vendor descriptor slice of ADR 0004 in the PR that deletes
# that vendor's includes from AlpacaHTTP/src/http/router.cpp; the last slice
# sets it to 0. Never raise it.
MAX_ALPACAHTTP_VENDOR_INCLUDES = 40
# ADR 0004: <vendor>_schema.cpp compiles in every build and includes no vendor
# header. Never rises.
MAX_CATALOG_SCHEMA_VENDOR_INCLUDES = 0

INCLUDE_RE = re.compile(
    r"^[ \t]*#[ \t]*include[ \t]*[<\"]alpacacore/vendor/(?P<vendor>[^/>\"]+)/",
    re.MULTILINE,
)


def region_files(root: pathlib.Path, region: str) -> list[pathlib.Path]:
    if region == "AlpacaHTTP":
        dirs = [root / "AlpacaHTTP"]
        files: set[pathlib.Path] = set()
        for d in dirs:
            for g in SOURCE_GLOBS:
                files.update(d.rglob(g))
        return sorted(files)
    files = set()
    for d in (
        root / "AlpacaCore" / "include" / "alpacacore" / "catalog",
        root / "AlpacaCore" / "src" / "catalog",
    ):
        for g in SOURCE_GLOBS:
            files.update(d.rglob(g))
    files.update(root.glob("AlpacaCore/src/vendors/*/*_schema.cpp"))
    return sorted(files)


def scan_region(root: pathlib.Path, region: str):
    """Return (files_visited, [(file, line, text, vendor)], [read errors])."""
    # STUB (red step): scanner not written yet, finds nothing.
    return len(region_files(root, region)), [], []


def main(argv: list[str], baselines: dict[str, int] | None = None) -> int:
    root = ROOT
    if "--root" in argv:
        root = pathlib.Path(argv[argv.index("--root") + 1])
    limits = baselines or {
        "AlpacaHTTP": MAX_ALPACAHTTP_VENDOR_INCLUDES,
        "catalog": MAX_CATALOG_SCHEMA_VENDOR_INCLUDES,
    }
    failed = False
    for region in ("AlpacaHTTP", "catalog"):
        visited, hits, errors = scan_region(root, region)
        print(f"{region}: {len(hits)} vendor includes (baseline {limits[region]})")
        if region == "AlpacaHTTP" and (not (root / "AlpacaHTTP").is_dir() or visited == 0):
            print("AlpacaHTTP region scanned no files -- gate is vacuous", file=sys.stderr)
            failed = True
        for e in errors:
            print(e, file=sys.stderr)
            failed = True
        if len(hits) > limits[region]:
            failed = True
            for f, n, text, _v in hits:
                print(f"{f}:{n}: {text}", file=sys.stderr)
            print(f"{region}: {len(hits)} exceeds baseline {limits[region]}", file=sys.stderr)
    return 1 if failed else 0


def _write(root: pathlib.Path, rel: str, text: str) -> pathlib.Path:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)
    return p


def _run(root: pathlib.Path, baselines: dict[str, int]) -> tuple[int, str]:
    import contextlib
    import io

    err = io.StringIO()
    out = io.StringIO()
    with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
        rc = main(["--root", str(root)], baselines)
    return rc, err.getvalue()


def self_test() -> int:
    inc = "#include <alpacacore/vendor/zwo/x.h>\n"
    base = {"AlpacaHTTP": 2, "catalog": 0}
    failures: list[str] = []

    def case(name: str, ok: bool) -> None:
        print(f"{'PASS' if ok else 'FAIL'}: {name}")
        if not ok:
            failures.append(name)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write(r, "AlpacaHTTP/src/http/router.cpp", inc * 3)
        rc, err = _run(r, base)
        case("one include above baseline fails and names file:line",
             rc == 1 and "router.cpp:3:" in err)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write(r, "AlpacaHTTP/src/http/router.cpp", inc * 2)
        rc, _ = _run(r, base)
        case("exactly baseline passes", rc == 0)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write(r, "AlpacaHTTP/src/a.cpp", inc + '#include "alpacacore/vendor/zwo/y.h"\n')
        _write(r, "AlpacaHTTP/src/b.h", "  #  include <alpacacore/vendor/qhy/z.h>\n")
        rc, err = _run(r, base)
        case("quote form and spaced directive are counted", rc == 1 and "b.h:1:" in err)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write(r, "AlpacaHTTP/src/a.cpp", inc + inc)
        _write(r, "AlpacaHTTP/src/b.cpp", "// #include <alpacacore/vendor/zwo/x.h>\n")
        rc, _ = _run(r, base)
        case("include inside a comment counts", rc == 1)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write(r, "AlpacaHTTP/src/a.cpp", inc)
        p = _write(r, "AlpacaHTTP/src/b.cpp", "int x;\n")
        p.write_bytes(b"\xff\xfe\x00bad utf8 \xc3\x28\n")
        rc, err = _run(r, base)
        case("unreadable (undecodable) file fails, not skipped",
             rc == 1 and "b.cpp" in err)

    with tempfile.TemporaryDirectory() as t:
        rc, _ = _run(pathlib.Path(t), base)
        case("missing AlpacaHTTP dir fails (vacuous)", rc == 1)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write(r, "AlpacaHTTP/src/a.cpp", inc)
        _write(r, "AlpacaCore/src/vendors/foo/foo_schema.cpp", inc)
        rc, err = _run(r, base)
        case("catalog schema include fails against baseline 0",
             rc == 1 and "foo_schema.cpp:1:" in err)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write(r, "AlpacaHTTP/src/a.cpp", inc)
        _write(r, "AlpacaCore/include/alpacacore/catalog/schema.h", inc)
        rc, err = _run(r, base)
        case("catalog header include fails", rc == 1 and "schema.h:1:" in err)

    return 1 if failures else 0


if __name__ == "__main__":
    if "--self-test" in sys.argv[1:]:
        sys.exit(self_test())
    sys.exit(main(sys.argv[1:]))
