#!/usr/bin/env python3
"""Layering gate: no new vendor headers in AlpacaHTTP or the catalog schema files.

ADR 0004 (device-catalog) moves per-vendor knowledge out of AlpacaHTTP into
vendor descriptors. AlpacaHTTP/src/http/router.cpp still includes
`alpacacore/vendor/...` headers up to this file's
MAX_ALPACAHTTP_VENDOR_INCLUDES baseline; each descriptor slice deletes some and
lowers the baseline. Nothing counted them, so a PR could add one more, or a
slice could forget to delete its includes, with no signal. The catalog schema
files must include none, ever.

Rules (baselines are the constants below, lowered by the PR that deletes the
includes, never raised):
  - AlpacaHTTP/**: at most MAX_ALPACAHTTP_VENDOR_INCLUDES vendor includes.
  - catalog files: at most MAX_CATALOG_SCHEMA_VENDOR_INCLUDES (zero).
  - each region must scan at least one file, or the gate is vacuous and fails.
  - catalog files are the catalog include/source directories plus
    AlpacaCore/src/vendors/*/*_schema.cpp only; a schema header, a nested
    vendor directory or a direct SDK include (not alpacacore/vendor/...) is
    not seen here, and the vendors-OFF build-test job catches the last kind.

Comments and `#if 0` are deliberately NOT stripped: an include inside one (even
mid-line or on a ` * ` continuation line) still counts, so the gate cannot be dodged by disabling a line. Do not "fix" this.

CMake rules (open-astro#710; source: AGENTS.md "Core Architecture", ADR 0004).
`alpacacore` is vendor-neutral, so the vendor libraries and the built-in
descriptor composition (`alpacacore_builtins`) link DOWN to it and it links to
none of them. Read from every CMakeLists.txt and *.cmake under AlpacaCore/ and
AlpacaHTTP/ except below an `external/` or `build*` directory, comments NOT
stripped:
  - L1: no `target_link_libraries(alpacacore ...)` (target name exactly
    `alpacacore`; `alpacacore_tests` or any other prefix match is not it)
    names `alpacacore_<name>` where <name> is a directory under
    AlpacaCore/src/vendors/, bare, as `AlpacaCore::alpacacore_<name>` or inside a
    generator expression (`$<LINK_ONLY:...>`): tokens are matched by fragment,
    split on `$ < > : , ;` and quotes.
  - L2: no `target_compile_definitions(alpacacore ...)` names a macro starting
    ALPACACORE_ENABLE_, and no directory-scoped `add_definitions` /
    `add_compile_definitions` in AlpacaCore/CMakeLists.txt does either (a
    directory-scoped one reaches `alpacacore` as well). The same holds for
    AlpacaHTTP/CMakeLists.txt, but only for a call before its last
    `add_subdirectory(... AlpacaCore ...)` (a later one does not reach
    `alpacacore`; the last, so a commented-out copy above the real call
    cannot hide anything); a file with no such add_subdirectory fails.
  - L3: no source listed in `add_library(alpacacore ...)` (AlpacaCore/CMakeLists.txt)
    or `target_sources(alpacacore ...)` (any scanned file), directly or through
    the `${VAR}` lists it expands (each resolved from a `set(VAR ...)` in the
    same file as the call; one that cannot be resolved is a failure), is under
    src/vendors/ or is a vendor target (`$<TARGET_OBJECTS:alpacacore_<name>>`),
    also inside a generator expression. A `${VAR}` nested inside a generator
    expression is not expanded.
  - each rule fails when it has nothing to check: a missing or unreadable
    AlpacaCore/CMakeLists.txt, no `add_library(alpacacore ...)` call, an empty
    source list (L3) or no vendor directory (L1).
  - call arguments are read up to the first `)`: a `)` inside a quoted CMake
    argument ends the call early. None of the calls these rules read carries
    one today; keep it so.

Run from the repo root: python3 scripts/check_layering.py
Self-test: python3 scripts/check_layering.py --self-test
"""

from __future__ import annotations

import pathlib
import re
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE_GLOBS = ("*.cpp", "*.h", "*.hpp")

# Lowered by each vendor descriptor slice of ADR 0004 in the PR that deletes
# that vendor's includes from AlpacaHTTP/src/http/router.cpp; the last slice
# sets it to 0. Never raise it.
MAX_ALPACAHTTP_VENDOR_INCLUDES = 37
# ADR 0004: <vendor>_schema.cpp compiles in every build and includes no vendor
# header. Never rises.
MAX_CATALOG_SCHEMA_VENDOR_INCLUDES = 0

# Matches the directive anywhere on a line, so `// #include`, ` * #include` and
# `code(); /* #include ... */` all count.
INCLUDE_RE = re.compile(
    r"#[ \t]*include[ \t]*[<\"]alpacacore/vendor/(?P<vendor>[^/>\"]+)/[^\n]*",
)


def region_files(root: pathlib.Path, region: str) -> list[pathlib.Path]:
    if region == "AlpacaHTTP":
        files: set[pathlib.Path] = set()
        for g in SOURCE_GLOBS:
            files.update((root / "AlpacaHTTP").rglob(g))
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
    files = region_files(root, region)
    hits: list[tuple[str, int, str, str]] = []
    errors: list[str] = []
    for path in files:
        rel = path.relative_to(root).as_posix()
        try:
            text = path.read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError) as exc:
            errors.append(f"cannot read {rel}: {exc}")
            continue
        for m in INCLUDE_RE.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            hits.append((rel, line, m.group(0).strip(), m.group("vendor")))
    return len(files), hits, errors


# ---------------------------------------------------------------------------
# CMake rules L1-L3 (open-astro#710)
# ---------------------------------------------------------------------------

CMAKE_CALL_RE = re.compile(
    r"\b(?P<cmd>target_link_libraries|target_compile_definitions|add_library|target_sources|"
    r"add_definitions|add_compile_definitions|add_subdirectory|set)\s*\(\s*(?P<args>[^)]*)\)",
    re.DOTALL,
)
ADD_LIBRARY_KEYWORDS = {"STATIC", "SHARED", "MODULE", "OBJECT", "INTERFACE", "IMPORTED",
                        "GLOBAL", "EXCLUDE_FROM_ALL"}
VAR_RE = re.compile(r"^\$\{(?P<name>[A-Za-z_][A-Za-z0-9_]*)\}$")


def _line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def cmake_calls(text: str):
    """Yield (cmd, [(token, line), ...]) for every call CMAKE_CALL_RE matches.

    Tokens are whitespace-split arguments; each carries the line it sits on so a
    finding inside a multi-line call names the right line."""
    for m in CMAKE_CALL_RE.finditer(text):
        base = m.start("args")
        toks = [(t.group(0), _line_of(text, base + t.start())) for t in re.finditer(r"\S+", m.group("args"))]
        yield m.group("cmd"), toks


GENEX_SPLIT_RE = re.compile(r'[$<>:,;"]+')


def _fragments(tok: str) -> list[str]:
    """Split a token on generator-expression punctuation (and `::`, quotes) after
    dropping the ${CMAKE_CURRENT_SOURCE_DIR}/ and ${CMAKE_SOURCE_DIR}/ prefixes, so a
    name or path wrapped in `$<...>` is matched like a bare one (open-astro#724)."""
    norm = tok.replace("${CMAKE_CURRENT_SOURCE_DIR}/", "").replace("${CMAKE_SOURCE_DIR}/", "")
    return [f for f in GENEX_SPLIT_RE.split(norm) if f]


def _is_vendor_source(tok: str, vendor_targets) -> bool:
    return any(f.startswith("src/vendors/") or "/src/vendors/" in f or f in vendor_targets
               for f in _fragments(tok))


def _target_is(toks: list[tuple[str, int]], name: str) -> bool:
    return bool(toks) and toks[0][0] == name


def vendor_dirs(root: pathlib.Path) -> list[str]:
    base = root / "AlpacaCore" / "src" / "vendors"
    if not base.is_dir():
        return []
    return sorted(p.name for p in base.iterdir() if p.is_dir())


def cmake_files(root: pathlib.Path) -> list[pathlib.Path]:
    """Every CMakeLists.txt and *.cmake under AlpacaCore/ and AlpacaHTTP/, except
    below an `external/` directory or a `build*` directory (matched on whole
    path components under the root, never on the file name)."""
    files: set[pathlib.Path] = set()
    for top in ("AlpacaCore", "AlpacaHTTP"):
        base = root / top
        for pattern in ("CMakeLists.txt", "*.cmake"):
            for path in base.rglob(pattern):
                dirs = path.relative_to(base).parts[:-1]
                if any(d == "external" or d.startswith("build") for d in dirs):
                    continue
                files.add(path)
    return sorted(files)


def _resolve_sources(text: str, toks: list[tuple[str, int]], sets: dict[str, list[tuple[str, int]]],
                     errors: list[str], rel: str, depth: int = 0) -> list[tuple[str, int]]:
    out: list[tuple[str, int]] = []
    for tok, line in toks:
        v = VAR_RE.match(tok)
        if v is None:
            out.append((tok, line))
            continue
        name = v.group("name")
        if depth > 8:
            errors.append(f"L3: {rel}:{line}: ${{{name}}} nests too deep to resolve")
            continue
        if name not in sets:
            errors.append(f"L3: {rel}:{line}: cannot resolve ${{{name}}} (no set({name} ...) in this file)")
            continue
        out.extend(_resolve_sources(text, sets[name], sets, errors, rel, depth + 1))
    return out


def check_cmake_rules(root: pathlib.Path) -> tuple[list[str], list[str]]:
    """Run L1-L3. Return (failure lines, summary lines)."""
    failures: list[str] = []
    core = root / "AlpacaCore" / "CMakeLists.txt"
    core_rel = "AlpacaCore/CMakeLists.txt"
    texts: dict[str, str] = {}
    for path in cmake_files(root):
        rel = path.relative_to(root).as_posix()
        try:
            texts[rel] = path.read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError) as exc:
            failures.append(f"cannot read {rel}: {exc}")
    if core_rel not in texts:
        failures.append(f"L1/L2/L3: {core_rel} missing or unreadable -- rules are vacuous")
        return failures, []

    vendors = vendor_dirs(root)
    if not vendors:
        failures.append("L1: no directory under AlpacaCore/src/vendors/ -- rule is vacuous")
    vendor_targets = {f"alpacacore_{v}" for v in vendors}

    http_rel = "AlpacaHTTP/CMakeLists.txt"
    # Line of the LAST add_subdirectory(... AlpacaCore ...) in http_rel. Comments are not
    # stripped, so a commented-out call above the real one must not move the cutoff up and
    # hide the definitions between them; a stray later match only makes the check stricter.
    http_cutoff = None
    if http_rel in texts:
        for cmd, toks in cmake_calls(texts[http_rel]):
            if cmd == "add_subdirectory" and any("AlpacaCore" in t for t, _ in toks):
                http_cutoff = toks[0][1]
        if http_cutoff is None:
            failures.append(f"L2: {http_rel}: no add_subdirectory(... AlpacaCore ...) call -- rule is vacuous")

    l1 = l2 = l3 = 0
    add_library_seen = False
    for rel, text in texts.items():
        sets: dict[str, list[tuple[str, int]]] = {}
        calls = list(cmake_calls(text))
        for cmd, toks in calls:
            if cmd == "set" and toks:
                sets[toks[0][0]] = toks[1:]
        for cmd, toks in calls:
            if cmd == "target_link_libraries" and _target_is(toks, "alpacacore"):
                for tok, line in toks[1:]:
                    if any(f in vendor_targets for f in _fragments(tok)):
                        l1 += 1
                        failures.append(f"L1: {rel}:{line}: target_link_libraries(alpacacore ...) links vendor library {tok}")
            elif cmd == "target_compile_definitions" and _target_is(toks, "alpacacore"):
                for tok, line in toks[1:]:
                    if "ALPACACORE_ENABLE_" in tok:
                        l2 += 1
                        failures.append(f"L2: {rel}:{line}: target_compile_definitions(alpacacore ...) defines {tok}")
            elif cmd in ("add_definitions", "add_compile_definitions") and rel in (core_rel, http_rel):
                for tok, line in toks:
                    # In AlpacaHTTP only a definition made before add_subdirectory(AlpacaCore)
                    # is inherited by alpacacore; later ones reach only AlpacaHTTP targets.
                    if rel == http_rel and (http_cutoff is None or line >= http_cutoff):
                        continue
                    if "ALPACACORE_ENABLE_" in tok:
                        l2 += 1
                        failures.append(f"L2: {rel}:{line}: directory-scoped {cmd}() defines {tok} (reaches alpacacore)")
            elif cmd == "target_sources" and _target_is(toks, "alpacacore"):
                body = [(t, n) for t, n in toks[1:] if t not in ("PUBLIC", "PRIVATE", "INTERFACE")]
                for tok, line in _resolve_sources(text, body, sets, failures, rel):
                    if _is_vendor_source(tok, vendor_targets):
                        l3 += 1
                        failures.append(f"L3: {rel}:{line}: target_sources(alpacacore ...) compiles vendor source {tok}")
            elif cmd == "add_library" and _target_is(toks, "alpacacore") and rel == core_rel:
                add_library_seen = True
                body = [(t, n) for t, n in toks[1:] if t not in ADD_LIBRARY_KEYWORDS]
                sources = _resolve_sources(text, body, sets, failures, rel)
                if not sources:
                    failures.append(f"L3: {rel}: add_library(alpacacore ...) lists no source -- rule is vacuous")
                for tok, line in sources:
                    if _is_vendor_source(tok, vendor_targets):
                        l3 += 1
                        failures.append(f"L3: {rel}:{line}: add_library(alpacacore ...) compiles vendor source {tok}")
    if not add_library_seen:
        failures.append(f"L1/L2/L3: no add_library(alpacacore ...) call in {core_rel} -- rules are vacuous")
    summary = [
        f"cmake L1: {l1} vendor links on alpacacore ({len(vendors)} vendor dirs, {len(texts)} CMake files)",
        f"cmake L2: {l2} ALPACACORE_ENABLE_ definitions reaching alpacacore",
        f"cmake L3: {l3} vendor sources compiled into alpacacore",
    ]
    return failures, summary


def main(argv: list[str], baselines: dict[str, int] | None = None) -> int:
    root = ROOT
    if "--root" in argv:
        i = argv.index("--root")
        if i + 1 >= len(argv):
            print("usage: check_layering.py [--self-test] [--root DIR]", file=sys.stderr)
            return 1
        root = pathlib.Path(argv[i + 1])
    limits = baselines if baselines is not None else {
        "AlpacaHTTP": MAX_ALPACAHTTP_VENDOR_INCLUDES,
        "catalog": MAX_CATALOG_SCHEMA_VENDOR_INCLUDES,
    }
    failed = False
    for region in ("AlpacaHTTP", "catalog"):
        visited, hits, errors = scan_region(root, region)
        print(f"{region}: {len(hits)} vendor includes (baseline {limits[region]})")
        per: dict[str, int] = {}
        for h in hits:
            per[h[3]] = per.get(h[3], 0) + 1
        for v, n in sorted(per.items(), key=lambda kv: (-kv[1], kv[0])):
            print(f"  {v}: {n}")
        if region == "AlpacaHTTP" and (not (root / "AlpacaHTTP").is_dir() or visited == 0):
            print("AlpacaHTTP region scanned no files -- gate is vacuous", file=sys.stderr)
            failed = True
        if region == "catalog" and visited == 0:
            print("catalog region scanned no files -- gate is vacuous", file=sys.stderr)
            failed = True
        for e in errors:
            print(e, file=sys.stderr)
            failed = True
        if len(hits) > limits[region]:
            failed = True
            for f, n, text, _v in hits:
                print(f"{f}:{n}: {text}", file=sys.stderr)
            print(f"{region}: {len(hits)} exceeds baseline {limits[region]}", file=sys.stderr)
    cmake_failures, cmake_summary = check_cmake_rules(root)
    for line in cmake_summary:
        print(line)
    for line in cmake_failures:
        print(line, file=sys.stderr)
        failed = True
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


CLEAN_CORE_CMAKE = """\
add_compile_definitions(ALPACACORE_VERSION="1.0")
set(ALPACACORE_CORE_SOURCES
    src/core/a.cpp
    src/catalog/device_catalog.cpp
)
set(ALPACACORE_DRIVER_SOURCES src/drivers/d.cpp)
set(ALPACACORE_MANAGEMENT_SOURCES ${ALPACACORE_DRIVER_SOURCES} src/management/m.cpp)
add_library(alpacacore STATIC
    ${ALPACACORE_CORE_SOURCES}
    ${ALPACACORE_DRIVER_SOURCES}
    ${ALPACACORE_MANAGEMENT_SOURCES}
)
add_library(AlpacaCore::alpacacore ALIAS alpacacore)
add_library(alpacacore_builtins STATIC src/catalog/builtin_catalog.cpp src/vendors/zwo/zwo_schema.cpp)
target_link_libraries(alpacacore_builtins PUBLIC alpacacore PRIVATE alpacacore_zwo)
target_compile_definitions(alpacacore_builtins PRIVATE ALPACACORE_ENABLE_ZWO)
target_link_libraries(alpacacore_tests PRIVATE alpacacore alpacacore_zwo)
target_compile_definitions(alpacacore_tests PRIVATE ALPACACORE_ENABLE_ZWO)
target_link_libraries(alpacacore PUBLIC Threads::Threads)
"""

CLEAN_VENDOR_CMAKE = """\
add_library(alpacacore_zwo STATIC zwo_driver.cpp)
target_link_libraries(alpacacore_zwo PRIVATE alpacacore)
"""

CLEAN_HTTP_CMAKE = """\
project(alpacahttp)
add_subdirectory(../AlpacaCore AlpacaCore)
add_definitions(-DALPACACORE_ENABLE_ZWO)
"""

CLEAN_TESTS_CMAKE = """\
target_link_libraries(alpacacore_tests PRIVATE alpacacore_zwo)
"""


def _write_clean_cmake(root: pathlib.Path) -> None:
    """A CMake tree that passes L1-L3, with the near-misses each rule must ignore:
    a prefix-match target (alpacacore_tests, alpacacore_builtins) linking a vendor
    and defining ALPACACORE_ENABLE_*, a directory-scoped non-ENABLE macro, a
    non-vendor library linked on alpacacore, and the vendor's own one-way link."""
    _write(root, "AlpacaCore/CMakeLists.txt", CLEAN_CORE_CMAKE)
    _write(root, "AlpacaCore/src/vendors/zwo/CMakeLists.txt", CLEAN_VENDOR_CMAKE)
    _write(root, "AlpacaHTTP/CMakeLists.txt", CLEAN_HTTP_CMAKE)
    _write(root, "AlpacaCore/tests/CMakeLists.txt", CLEAN_TESTS_CMAKE)


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
        _write_clean_cmake(r)
        _write(r, "AlpacaHTTP/src/http/router.cpp", inc * 3)
        rc, err = _run(r, base)
        case("one include above baseline fails and names file:line",
             rc == 1 and "router.cpp:3:" in err)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write_clean_cmake(r)
        _write(r, "AlpacaHTTP/src/http/router.cpp", inc * 2)
        _write(r, "AlpacaCore/src/catalog/c.cpp", "int x;\n")
        rc, _ = _run(r, base)
        case("exactly baseline passes", rc == 0)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write_clean_cmake(r)
        _write(r, "AlpacaHTTP/src/a.cpp", inc + '#include "alpacacore/vendor/zwo/y.h"\n')
        _write(r, "AlpacaHTTP/src/b.h", "  #  include <alpacacore/vendor/qhy/z.h>\n")
        rc, err = _run(r, base)
        case("quote form and spaced directive are counted", rc == 1 and "b.h:1:" in err)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write_clean_cmake(r)
        _write(r, "AlpacaHTTP/src/a.cpp", inc + inc)
        _write(r, "AlpacaHTTP/src/b.cpp", "// #include <alpacacore/vendor/zwo/x.h>\n")
        _write(r, "AlpacaCore/src/catalog/c.cpp", "int x;\n")
        rc, err = _run(r, base)
        case("include inside a comment counts", rc == 1 and "b.cpp:1:" in err)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write_clean_cmake(r)
        _write(r, "AlpacaHTTP/src/a.cpp", inc)
        p = _write(r, "AlpacaHTTP/src/b.cpp", "int x;\n")
        p.write_bytes(b"\xff\xfe\x00bad utf8 \xc3\x28\n")
        rc, err = _run(r, base)
        case("unreadable (undecodable) file fails, not skipped",
             rc == 1 and "b.cpp" in err)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write_clean_cmake(r)
        _write(r, "AlpacaCore/src/catalog/c.cpp", "int x;\n")
        rc, err = _run(r, base)
        case("missing AlpacaHTTP dir fails (vacuous)",
             rc == 1 and "AlpacaHTTP region scanned no files" in err)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write_clean_cmake(r)
        _write(r, "AlpacaHTTP/src/a.cpp", inc)
        _write(r, "AlpacaCore/src/vendors/foo/foo_schema.cpp", inc)
        rc, err = _run(r, base)
        case("catalog schema include fails against baseline 0",
             rc == 1 and "foo_schema.cpp:1:" in err)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write_clean_cmake(r)
        _write(r, "AlpacaHTTP/src/a.cpp", inc)
        _write(r, "AlpacaCore/include/alpacacore/catalog/schema.h", inc)
        rc, err = _run(r, base)
        case("catalog header include fails", rc == 1 and "schema.h:1:" in err)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write_clean_cmake(r)
        _write(r, "AlpacaHTTP/src/a.cpp", inc + inc)
        _write(r, "AlpacaHTTP/src/b.cpp", " * #include <alpacacore/vendor/zwo/x.h>\n")
        rc, err = _run(r, base)
        case("block-comment continuation line counts", rc == 1 and "b.cpp:1:" in err)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write_clean_cmake(r)
        _write(r, "AlpacaHTTP/src/a.cpp", inc + inc)
        _write(r, "AlpacaHTTP/src/b.cpp", "f(); /* #include <alpacacore/vendor/zwo/x.h> */\n")
        rc, err = _run(r, base)
        case("include after code on the same line counts", rc == 1 and "b.cpp:1:" in err)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write_clean_cmake(r)
        _write(r, "AlpacaHTTP/src/a.cpp", inc)
        _write(r, "AlpacaCore/src/catalog/c.cpp", inc)
        rc, err = _run(r, base)
        case("AlpacaCore/src/catalog include fails", rc == 1 and "c.cpp:1:" in err)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write_clean_cmake(r)
        _write(r, "AlpacaHTTP/src/a.cpp", inc)
        rc, err = _run(r, base)
        case("catalog region with no files fails (vacuous)",
             rc == 1 and "catalog region scanned no files" in err)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write_clean_cmake(r)
        (r / "AlpacaHTTP").mkdir(exist_ok=True)
        _write(r, "AlpacaCore/src/catalog/c.cpp", "int x;\n")
        rc, err = _run(r, base)
        case("empty AlpacaHTTP dir fails (vacuous)",
             rc == 1 and "AlpacaHTTP region scanned no files" in err)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write_clean_cmake(r)
        _write(r, "AlpacaHTTP/src/a.cpp", inc)
        _write(r, "AlpacaCore/src/catalog/c.cpp", "int x;\n")
        rc, err = _run(r, {"AlpacaHTTP": 0, "catalog": 0})
        case("explicit zero baseline is honoured, not replaced by defaults",
             rc == 1 and "exceeds baseline 0" in err)

    # --- CMake rules L1-L3 (open-astro#710) --------------------------------
    clean_http = inc  # one include, under the AlpacaHTTP baseline of 2

    def cmake_fixture(core: str | None = None, vendor: str | None = None):
        t = tempfile.TemporaryDirectory()
        r = pathlib.Path(t.name)
        _write(r, "AlpacaHTTP/src/a.cpp", clean_http)
        _write(r, "AlpacaCore/src/catalog/c.cpp", "int x;\n")
        _write_clean_cmake(r)
        if core is not None:
            _write(r, "AlpacaCore/CMakeLists.txt", core)
        if vendor is not None:
            _write(r, "AlpacaCore/src/vendors/zwo/CMakeLists.txt", vendor)
        return t, r

    t, r = cmake_fixture()
    with t:
        rc, err = _run(r, base)
        case("L1-L3: clean tree with prefix-match targets, builtins library and one-way vendor link passes",
             rc == 0 and err == "")

    t, r = cmake_fixture(core=CLEAN_CORE_CMAKE + "target_link_libraries(alpacacore PRIVATE alpacacore_zwo)\n")
    with t:
        rc, err = _run(r, base)
        case("L1: alpacacore linking a vendor library fails and names file:line",
             rc == 1 and "L1: AlpacaCore/CMakeLists.txt:20:" in err and "alpacacore_zwo" in err)

    t, r = cmake_fixture(core=CLEAN_CORE_CMAKE + "if(TARGET alpacacore_zwo)\n    target_link_libraries(alpacacore\n        PRIVATE\n        alpacacore_zwo)\nendif()\n")
    with t:
        rc, err = _run(r, base)
        case("L1: multi-line call names the line of the vendor token",
             rc == 1 and "L1: AlpacaCore/CMakeLists.txt:23:" in err)

    t, r = cmake_fixture(core=CLEAN_CORE_CMAKE + "# target_link_libraries(alpacacore PRIVATE alpacacore_zwo)\n")
    with t:
        rc, err = _run(r, base)
        case("L1: a commented-out vendor link still counts (comments not stripped)",
             rc == 1 and "L1: AlpacaCore/CMakeLists.txt:20:" in err)

    t, r = cmake_fixture(vendor=CLEAN_VENDOR_CMAKE + "target_link_libraries(alpacacore PRIVATE alpacacore_zwo)\n")
    with t:
        rc, err = _run(r, base)
        case("L1: a vendor CMakeLists.txt linking a vendor onto alpacacore fails",
             rc == 1 and "L1: AlpacaCore/src/vendors/zwo/CMakeLists.txt:3:" in err)

    t, r = cmake_fixture(core=CLEAN_CORE_CMAKE + "target_link_libraries(alpacacore PRIVATE alpacacore_notavendor)\n")
    with t:
        rc, err = _run(r, base)
        case("L1: an alpacacore_<name> with no vendor directory is not a vendor link", rc == 0)

    t, r = cmake_fixture(core=CLEAN_CORE_CMAKE + "target_compile_definitions(alpacacore PRIVATE ALPACACORE_ENABLE_ZWO)\n")
    with t:
        rc, err = _run(r, base)
        case("L2: target_compile_definitions(alpacacore ALPACACORE_ENABLE_*) fails and names file:line",
             rc == 1 and "L2: AlpacaCore/CMakeLists.txt:20:" in err and "ALPACACORE_ENABLE_ZWO" in err)

    t, r = cmake_fixture(core="add_definitions(-DALPACACORE_ENABLE_ZWO)\n" + CLEAN_CORE_CMAKE)
    with t:
        rc, err = _run(r, base)
        case("L2: directory-scoped add_definitions(-DALPACACORE_ENABLE_*) fails",
             rc == 1 and "L2: AlpacaCore/CMakeLists.txt:1:" in err)

    t, r = cmake_fixture(core="add_compile_definitions(ALPACACORE_ENABLE_ZWO=1)\n" + CLEAN_CORE_CMAKE)
    with t:
        rc, err = _run(r, base)
        case("L2: directory-scoped add_compile_definitions(ALPACACORE_ENABLE_*) fails",
             rc == 1 and "L2: AlpacaCore/CMakeLists.txt:1:" in err)

    t, r = cmake_fixture(vendor=CLEAN_VENDOR_CMAKE + "add_compile_definitions(ALPACACORE_ENABLE_ZWO)\n")
    with t:
        rc, err = _run(r, base)
        case("L2: a vendor subdirectory's add_compile_definitions does not reach alpacacore", rc == 0)

    t, r = cmake_fixture(core=CLEAN_CORE_CMAKE.replace(
        "    src/catalog/device_catalog.cpp\n", "    src/catalog/device_catalog.cpp\n    src/vendors/zwo/zwo_schema.cpp\n"))
    with t:
        rc, err = _run(r, base)
        case("L3: a src/vendors/ source reached through ${ALPACACORE_CORE_SOURCES} fails at the set() line",
             rc == 1 and "L3: AlpacaCore/CMakeLists.txt:5:" in err and "zwo_schema.cpp" in err)

    t, r = cmake_fixture(core=CLEAN_CORE_CMAKE.replace(
        "    ${ALPACACORE_MANAGEMENT_SOURCES}\n", "    ${ALPACACORE_MANAGEMENT_SOURCES}\n    src/vendors/zwo/zwo_schema.cpp\n"))
    with t:
        rc, err = _run(r, base)
        case("L3: a src/vendors/ source listed directly in add_library(alpacacore) fails",
             rc == 1 and "L3: AlpacaCore/CMakeLists.txt:12:" in err)

    t, r = cmake_fixture(core=CLEAN_CORE_CMAKE.replace(
        "set(ALPACACORE_MANAGEMENT_SOURCES ${ALPACACORE_DRIVER_SOURCES} src/management/m.cpp)",
        "set(ALPACACORE_MANAGEMENT_SOURCES ${ALPACACORE_DRIVER_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/zwo/z.cpp)"))
    with t:
        rc, err = _run(r, base)
        case("L3: a nested variable and a ${CMAKE_CURRENT_SOURCE_DIR}/ prefix are both followed",
             rc == 1 and "L3: AlpacaCore/CMakeLists.txt:7:" in err)

    t, r = cmake_fixture(core=CLEAN_CORE_CMAKE.replace("${ALPACACORE_DRIVER_SOURCES}\n", "${UNDEFINED_LIST}\n", 1))
    with t:
        rc, err = _run(r, base)
        case("L3: an unresolvable ${VAR} in add_library(alpacacore) fails, not skipped",
             rc == 1 and "cannot resolve ${UNDEFINED_LIST}" in err)

    t, r = cmake_fixture(core="add_library(alpacacore STATIC)\n")
    with t:
        rc, err = _run(r, base)
        case("L3: an empty alpacacore source list fails (vacuous)",
             rc == 1 and "lists no source" in err)

    t, r = cmake_fixture(core="add_library(alpacacore_other STATIC a.cpp)\n")
    with t:
        rc, err = _run(r, base)
        case("L1-L3: no add_library(alpacacore ...) call fails (vacuous)",
             rc == 1 and "no add_library(alpacacore" in err)

    with tempfile.TemporaryDirectory() as t:
        r = pathlib.Path(t)
        _write(r, "AlpacaHTTP/src/a.cpp", clean_http)
        _write(r, "AlpacaCore/src/catalog/c.cpp", "int x;\n")
        rc, err = _run(r, base)
        case("L1-L3: missing AlpacaCore/CMakeLists.txt fails (vacuous)",
             rc == 1 and "missing or unreadable" in err)

    t, r = cmake_fixture()
    with t:
        import shutil
        shutil.rmtree(r / "AlpacaCore" / "src" / "vendors")
        rc, err = _run(r, base)
        case("L1: no vendor directory fails (vacuous)",
             rc == 1 and "no directory under AlpacaCore/src/vendors/" in err)

    bad_link = "target_link_libraries(alpacacore PRIVATE alpacacore_zwo)\n"
    for rel, body, want in (
        ("AlpacaHTTP/CMakeLists.txt", CLEAN_HTTP_CMAKE + bad_link, "L1: AlpacaHTTP/CMakeLists.txt:4:"),
        ("AlpacaCore/tests/CMakeLists.txt", CLEAN_TESTS_CMAKE + bad_link, "L1: AlpacaCore/tests/CMakeLists.txt:2:"),
        ("AlpacaCore/foo/bar/CMakeLists.txt", bad_link, "L1: AlpacaCore/foo/bar/CMakeLists.txt:1:"),
        ("AlpacaCore/cmake/x.cmake", bad_link, "L1: AlpacaCore/cmake/x.cmake:1:"),
    ):
        t, r = cmake_fixture()
        with t:
            _write(r, rel, body)
            rc, err = _run(r, base)
            case(f"scope: a vendor link on alpacacore in {rel} fails", rc == 1 and want in err)

    t, r = cmake_fixture()
    with t:
        _write(r, "AlpacaCore/external/x/CMakeLists.txt", bad_link)
        _write(r, "AlpacaHTTP/build-x/CMakeLists.txt", bad_link)
        rc, err = _run(r, base)
        case("scope: external/ and build* directories are not scanned", rc == 0 and err == "")

    t, r = cmake_fixture()
    with t:
        _write(r, "AlpacaHTTP/buildings/CMakeLists.txt", bad_link)
        rc, err = _run(r, base)
        case("scope: only build* directory names are excluded by prefix (buildings/ is excluded too)", rc == 0)

    t, r = cmake_fixture()
    with t:
        _write(r, "AlpacaCore/tests/CMakeLists.txt",
               "target_link_libraries(alpacacore_tests PRIVATE alpacacore_zwo)\n"
               "target_compile_definitions(alpacacore_tests PRIVATE ALPACACORE_ENABLE_ZWO)\n")
        rc, err = _run(r, base)
        case("scope: prefix-match targets in tests/CMakeLists.txt still pass", rc == 0 and err == "")

    t, r = cmake_fixture(core=CLEAN_CORE_CMAKE + "target_sources(alpacacore PRIVATE src/vendors/zwo/x.cpp)\n")
    with t:
        rc, err = _run(r, base)
        case("L3: target_sources(alpacacore) with a src/vendors/ source fails and names file:line",
             rc == 1 and "L3: AlpacaCore/CMakeLists.txt:20:" in err and "target_sources" in err)

    t, r = cmake_fixture(core=CLEAN_CORE_CMAKE + "set(EXTRA src/vendors/zwo/x.cpp)\ntarget_sources(alpacacore PRIVATE ${EXTRA})\n")
    with t:
        rc, err = _run(r, base)
        case("L3: target_sources(alpacacore) resolves ${VAR} lists",
             rc == 1 and "L3: AlpacaCore/CMakeLists.txt:20:" in err)

    t, r = cmake_fixture()
    with t:
        _write(r, "AlpacaCore/tests/CMakeLists.txt",
               "target_sources(alpacacore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/vendors/zwo/x.cpp)\n")
        rc, err = _run(r, base)
        case("L3: target_sources(alpacacore) is checked in any scanned file",
             rc == 1 and "L3: AlpacaCore/tests/CMakeLists.txt:1:" in err)

    t, r = cmake_fixture()
    with t:
        _write(r, "AlpacaCore/tests/CMakeLists.txt",
               "target_sources(alpacacore_tests PRIVATE src/vendors/zwo/x.cpp)\n"
               "target_sources(alpacacore PRIVATE src/core/a.cpp)\n")
        rc, err = _run(r, base)
        case("L3: target_sources on another target or a non-vendor source passes", rc == 0 and err == "")

    # --- tokens inside generator expressions (open-astro#724) ---------------
    genex_fail = [
        ("L1: $<LINK_ONLY:vendor target> fails", "L1", "target_link_libraries(alpacacore PRIVATE $<LINK_ONLY:alpacacore_zwo>)\n"),
        ("L1: nested $<$<BOOL:ON>:vendor target> fails", "L1", "target_link_libraries(alpacacore PRIVATE $<$<BOOL:ON>:alpacacore_zwo>)\n"),
        ("L1: quoted LINK_ONLY and BUILD_INTERFACE:AlpacaCore:: vendor target fail", "L1",
         'target_link_libraries(alpacacore PRIVATE "$<LINK_ONLY:alpacacore_zwo>" $<BUILD_INTERFACE:AlpacaCore::alpacacore_zwo>)\n'),
        ("L3: add_library source in $<$<BOOL:ON>:src/vendors/...> fails", "L3",
         "add_library(alpacacore STATIC src/a.cpp $<$<BOOL:ON>:src/vendors/zwo/x.cpp>)\n"),
        ("L3: add_library BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/ source fails", "L3",
         'add_library(alpacacore STATIC src/a.cpp "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/zwo/y.cpp>")\n'),
        ("L3: add_library $<TARGET_OBJECTS:vendor target> fails", "L3",
         "add_library(alpacacore STATIC src/a.cpp $<TARGET_OBJECTS:alpacacore_zwo>)\n"),
        ("L3: target_sources source in a generator expression fails", "L3",
         "target_sources(alpacacore PRIVATE $<$<BOOL:ON>:src/vendors/zwo/x.cpp>)\n"),
        ("L3: target_sources $<TARGET_OBJECTS:vendor target> fails", "L3",
         "target_sources(alpacacore PRIVATE $<TARGET_OBJECTS:alpacacore_zwo>)\n"),
    ]
    for name, rule, line in genex_fail:
        t, r = cmake_fixture(core=CLEAN_CORE_CMAKE + line)
        with t:
            rc, err = _run(r, base)
            case(name, rc == 1 and f"{rule}: AlpacaCore/CMakeLists.txt:20:" in err)

    genex_pass = [
        ("generator expression around a longer target name (alpacacore_zwo_extra) passes",
         "target_link_libraries(alpacacore PRIVATE $<LINK_ONLY:alpacacore_zwo_extra>)\n"),
        ("generator expression around a non-vendor include path passes",
         "target_include_directories(alpacacore PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)\n"
         "target_sources(alpacacore PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/a.h>)\n"),
        ("generator-expression vendor link on another target (alpacacore_tests) passes",
         "target_link_libraries(alpacacore_tests PRIVATE $<LINK_ONLY:alpacacore_zwo>)\n"),
        ("source under src/vendorsx/ passes",
         "target_sources(alpacacore PRIVATE src/vendorsx/a.cpp)\n"),
    ]
    for name, line in genex_pass:
        t, r = cmake_fixture(core=CLEAN_CORE_CMAKE + line)
        with t:
            rc, err = _run(r, base)
            case("genex: " + name, rc == 0 and err == "")

    t, r = cmake_fixture()
    with t:
        _write(r, "AlpacaHTTP/CMakeLists.txt",
               "add_definitions(-DALPACACORE_ENABLE_ZWO)\nadd_subdirectory(../AlpacaCore AlpacaCore)\n")
        rc, err = _run(r, base)
        case("L2: AlpacaHTTP directory-scoped ALPACACORE_ENABLE_ before add_subdirectory(AlpacaCore) fails",
             rc == 1 and "L2: AlpacaHTTP/CMakeLists.txt:1:" in err)

    t, r = cmake_fixture()
    with t:
        _write(r, "AlpacaHTTP/CMakeLists.txt",
               "add_compile_definitions(ALPACACORE_ENABLE_ZWO)\nadd_subdirectory(../AlpacaCore AlpacaCore)\n")
        rc, err = _run(r, base)
        case("L2: AlpacaHTTP add_compile_definitions before add_subdirectory(AlpacaCore) fails",
             rc == 1 and "L2: AlpacaHTTP/CMakeLists.txt:1:" in err)

    t, r = cmake_fixture()
    with t:
        _write(r, "AlpacaHTTP/CMakeLists.txt",
               "# add_subdirectory(../AlpacaCore AlpacaCore)\n"
               "add_definitions(-DALPACACORE_ENABLE_ZWO)\n"
               "add_subdirectory(../AlpacaCore AlpacaCore)\n")
        rc, err = _run(r, base)
        case("L2: a commented-out add_subdirectory(AlpacaCore) above the real one does not move the cutoff",
             rc == 1 and "L2: AlpacaHTTP/CMakeLists.txt:2:" in err)

    t, r = cmake_fixture()
    with t:
        _write(r, "AlpacaHTTP/CMakeLists.txt", "project(alpacahttp)\nadd_definitions(-DALPACACORE_ENABLE_ZWO)\n")
        rc, err = _run(r, base)
        case("L2: AlpacaHTTP/CMakeLists.txt with no add_subdirectory(AlpacaCore) fails (vacuous)",
             rc == 1 and "no add_subdirectory(" in err)

    return 1 if failures else 0


if __name__ == "__main__":
    if "--self-test" in sys.argv[1:]:
        sys.exit(self_test())
    sys.exit(main(sys.argv[1:]))
