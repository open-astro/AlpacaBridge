#!/usr/bin/env python3
"""Contract-sweep registration gate (issue #571).

The cross-driver contract sweep (AlpacaCore/tests/contract_sweep.h +
test_contract_sweep.cpp) runs the ASCOM contract cases over one registry entry
per (vendor, device type) pair the router can construct. Nothing else makes a
new driver join it: without this gate an 8th telescope driver inherits no
contract coverage and CI stays green, which is the failure the sweep exists to
end.

Rules:
  1. Every (vendor, deviceType) pair Router::register_device_from_config()
     constructs has an `X(<vendor>_<devicetype>)` registry entry, or is in
     ALLOWLIST with a reason.
  2. A stale ALLOWLIST entry fails: the pair is now registered, or the router no
     longer constructs it.
  3. A registry entry whose pair the router does not construct fails (orphan),
     and one registered under a guard that does not name its vendor fails (the
     vendors-off build would sweep the wrong set). GUARD_OVERRIDE names the
     pairs the router itself builds under another vendor's flag.
  4. tests/CMakeLists.txt must define every ALPACACORE_* guard the registry uses
     for alpacacore_tests. Those macros are NOT inherited from the vendor
     targets (only AlpacaHTTP/CMakeLists.txt defines them, for the router), so a
     missing definition compiles the registry to zero entries and the sweep
     passes vacuously. Each ALPACACORE_ENABLE_<V> must sit inside
     `if(TARGET alpacacore_<v>)`.
  5. test_contract_sweep.cpp must be in the unconditional TEST_SOURCES list and
     must expand CONTRACT_SWEEP_ENTRIES.

Run from the repo root:  python3 scripts/check_contract_sweep.py
Self-test (literal fixtures, no repo state):  python3 scripts/check_contract_sweep.py --self-test
"""

from __future__ import annotations

import pathlib
import re
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent

ROUTER = "AlpacaHTTP/src/http/router.cpp"
REGISTRY = "AlpacaCore/tests/contract_sweep.h"
SWEEP_TEST = "AlpacaCore/tests/test_contract_sweep.cpp"
TESTS_CMAKE = "AlpacaCore/tests/CMakeLists.txt"
THIS_SCRIPT = "scripts/check_contract_sweep.py"

ROUTER_FUNCTION = "Router::register_device_from_config("

# (vendor, device_type) -> reason it has no registry entry yet. Empty: every pair
# the router constructs is swept. Adding one needs a reason a reviewer can weigh.
ALLOWLIST: dict[tuple[str, str], str] = {}

# Pairs the router builds under another vendor's build flag.
GUARD_OVERRIDE = {
    ("ioptron", "camera"): "PLAYERONE",  # iCAM cameras are rebadged Player One (router.cpp)
}

ARM_RE = re.compile(r'vendor\s*==\s*"(?P<v>[a-z0-9]+)"\s*&&\s*device_type_str\s*==\s*"(?P<t>[a-z0-9]+)"')
ENTRY_RE = re.compile(r"\bX\(\s*(?P<id>[a-z0-9]+_[a-z0-9]+)\s*\)")
GUARD_TOKEN_RE = re.compile(r"ALPACACORE_[A-Z0-9_]+")


def read_text(path: pathlib.Path) -> str:
    """A seam the self-test does not need: it points ROOT at a temp tree."""
    return path.read_text(encoding="utf-8", errors="replace")


def matching_brace(text: str, open_idx: int) -> int:
    """Index of the `}` closing the `{` at open_idx, skipping string and char
    literals and comments (the router body holds braces in both)."""
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c in "\"'":
            quote = c
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def strip_comments(text: str) -> str:
    """text with // and /* */ comments blanked (string/char literals kept, since
    the router arms are string comparisons). Length-preserving."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def router_pairs(router_text: str) -> set[tuple[str, str]]:
    """The (vendor, deviceType) arms inside register_device_from_config()."""
    router_text = strip_comments(router_text)
    start = router_text.find(ROUTER_FUNCTION)
    if start < 0:
        return set()
    open_idx = router_text.find("{", router_text.find(")", start))
    close_idx = matching_brace(router_text, open_idx)
    if open_idx < 0 or close_idx < 0:
        return set()
    body = router_text[open_idx:close_idx]
    return {(m.group("v"), m.group("t")) for m in ARM_RE.finditer(body)}


def registry_entries(header_text: str) -> dict[str, set[str]]:
    """entry id -> the ALPACACORE_* guard tokens of the `#if` conditions that
    enclose its `#define CS_...(X)` list. Only the active (non-#else) branch of a
    condition counts; the #else twin is the empty list."""
    lines: list[str] = []
    buf = ""
    for raw in header_text.splitlines():
        if raw.rstrip().endswith("\\"):
            buf += raw.rstrip()[:-1] + " "
            continue
        lines.append(buf + raw)
        buf = ""
    if buf:
        lines.append(buf)

    entries: dict[str, set[str]] = {}
    stack: list[list] = []  # [tokens, in_else]
    for line in lines:
        s = line.strip()
        m = re.match(r"#\s*(if|ifdef|ifndef|else|endif|define)\b(.*)", s)
        if not m:
            continue
        kind, rest = m.group(1), m.group(2)
        if kind in ("if", "ifdef", "ifndef"):
            stack.append([set(GUARD_TOKEN_RE.findall(rest)), False])
        elif kind == "else" and stack:
            stack[-1][1] = True
        elif kind == "endif" and stack:
            stack.pop()
        elif kind == "define":
            dm = re.match(r"\s*CS_[A-Z0-9_]+\(X\)(.*)", rest)
            if not dm or any(in_else for _, in_else in stack):
                continue
            tokens: set[str] = set()
            for toks, _ in stack:
                tokens |= toks
            for em in ENTRY_RE.finditer(dm.group(1)):
                entries.setdefault(em.group("id"), set()).update(tokens)
    return entries


def cmake_defined_guards(cmake_text: str) -> dict[str, set[str]]:
    """guard token -> the set of `if(TARGET alpacacore_<x>)` block names it is
    defined in for alpacacore_tests ('' when defined outside any such block)."""
    defs: dict[str, set[str]] = {}
    pat = re.compile(r"target_compile_definitions\(\s*alpacacore_tests\s+PRIVATE\s+(ALPACACORE_[A-Z0-9_]+)\s*\)")
    blocks = [(m.start(), m.end(), m.group(1)) for m in re.finditer(
        r"if\(\s*TARGET\s+alpacacore_([a-z0-9]+)[^)]*\)(?P<body>.*?)\nendif\(\)", cmake_text, re.S)]
    for m in pat.finditer(cmake_text):
        owners = {name for start, end, name in blocks if start <= m.start() < end}
        defs.setdefault(m.group(1), set()).update(owners or {""})
    return defs


def check(root: pathlib.Path) -> list[str]:
    failures: list[str] = []

    def load(rel: str) -> str | None:
        p = root / rel
        if not p.exists():
            failures.append("MISSING FILE: %s" % rel)
            return None
        return read_text(p)

    router_text = load(ROUTER)
    header_text = load(REGISTRY)
    cmake_text = load(TESTS_CMAKE)
    sweep_text = load(SWEEP_TEST)
    if router_text is None or header_text is None or cmake_text is None or sweep_text is None:
        return failures

    pairs = router_pairs(router_text)
    if not pairs:
        failures.append("NO ROUTER ARMS FOUND in %s (%s): the parser is broken or the function moved"
                        % (ROUTER, ROUTER_FUNCTION))
    entries = registry_entries(header_text)
    if not entries:
        failures.append("EMPTY REGISTRY: no X(<vendor>_<type>) entries parsed from %s" % REGISTRY)
    entry_pairs: dict[tuple[str, str], str] = {}
    for eid in entries:
        vendor, _, dtype = eid.partition("_")
        entry_pairs[(vendor, dtype)] = eid

    # 1 + 2: coverage and stale allow-list.
    for pair in sorted(pairs):
        covered = pair in entry_pairs
        allowed = pair in ALLOWLIST
        if not covered and not allowed:
            failures.append(
                "UNSWEPT PAIR: the router constructs %s/%s but %s has no X(%s_%s) entry and it is not in "
                "ALLOWLIST. Add a registry entry (contract_entry_%s_%s + the CS_ list) or allow-list it "
                "with a reason in %s." % (pair[0], pair[1], REGISTRY, pair[0], pair[1], pair[0], pair[1], THIS_SCRIPT))
        if covered and allowed:
            failures.append(
                "STALE ALLOWLIST ENTRY: %s/%s is now in the registry -- remove ('%s', '%s') from ALLOWLIST in %s"
                % (pair[0], pair[1], pair[0], pair[1], THIS_SCRIPT))
    for pair in sorted(set(ALLOWLIST) - pairs):
        failures.append(
            "STALE ALLOWLIST ENTRY: %s/%s is no longer constructed by the router -- remove ('%s', '%s') from "
            "ALLOWLIST in %s" % (pair[0], pair[1], pair[0], pair[1], THIS_SCRIPT))

    # 3: orphan entries and guard/vendor mismatch.
    for pair, eid in sorted(entry_pairs.items()):
        if pairs and pair not in pairs:
            failures.append("ORPHAN ENTRY: %s is in the registry but the router does not construct %s/%s"
                            % (eid, pair[0], pair[1]))
        want = "ALPACACORE_ENABLE_" + GUARD_OVERRIDE.get(pair, pair[0].upper())
        if want not in entries[eid]:
            failures.append(
                "GUARD MISMATCH: %s must sit under a condition naming %s (found %s), so a vendors-off build "
                "sweeps only the drivers it has" % (eid, want, sorted(entries[eid]) or "no guard"))

    # 4: the guards must be defined for alpacacore_tests.
    defs = cmake_defined_guards(cmake_text)
    used = set()
    for toks in entries.values():
        used |= toks
    for tok in sorted(used):
        if tok not in defs:
            failures.append(
                "GUARD NOT DEFINED FOR TESTS: %s guards registry entries but %s never runs "
                "target_compile_definitions(alpacacore_tests PRIVATE %s), so the registry compiles to nothing "
                "and the sweep passes vacuously" % (tok, TESTS_CMAKE, tok))
        elif tok.startswith("ALPACACORE_ENABLE_"):
            owner = tok[len("ALPACACORE_ENABLE_"):].lower()
            if owner not in defs[tok]:
                failures.append(
                    "GUARD DEFINED OUTSIDE ITS TARGET: %s must be defined inside if(TARGET alpacacore_%s) in %s "
                    "(found in %s)" % (tok, owner, TESTS_CMAKE, sorted(defs[tok])))

    # 5: the sweep source is compiled unconditionally and expands the registry.
    m = re.search(r"set\(\s*TEST_SOURCES(.*?)\n\)", cmake_text, re.S)
    if not m or "test_contract_sweep.cpp" not in m.group(1):
        failures.append("SWEEP NOT COMPILED: test_contract_sweep.cpp must be listed in the unconditional "
                        "TEST_SOURCES block of %s" % TESTS_CMAKE)
    if "CONTRACT_SWEEP_ENTRIES(" not in sweep_text:
        failures.append("SWEEP DOES NOT EXPAND THE REGISTRY: %s never uses CONTRACT_SWEEP_ENTRIES(...)" % SWEEP_TEST)

    return failures


def main() -> int:
    failures = check(ROOT)
    if failures:
        print("Contract-sweep registration check failed:\n")
        for f in failures:
            print("  " + f)
        print("\n%d finding(s)." % len(failures))
        return 1
    pairs = router_pairs(read_text(ROOT / ROUTER))
    print("Contract-sweep registration OK -- %d router pairs checked, %d allow-listed as not-yet-swept."
          % (len(pairs), len(ALLOWLIST)))
    return 0


# ---------------------------------------------------------------------------
# Self-test (ADR 0003): literal fixtures, one per rule, plus a main-shaped run.
# ---------------------------------------------------------------------------

FIX_ROUTER = '''
bool Router::register_device_from_config(const nlohmann::json& config, std::string& error) {
    if (vendor == "bisque" && device_type_str == "telescope") {
        std::string s = "{ not a brace }";
        return true;
    }
    if (vendor == "gemini" && device_type_str == "switch") {
        // vendor == "ghost" && device_type_str == "comment"  { unbalanced in a comment
        return true;
    }
    if (vendor == "ioptron" && device_type_str == "camera") {
        return true;
    }
    return false;
}
bool Router::other() {
    if (vendor == "zwo" && device_type_str == "camera") { return true; }
}
'''

FIX_HEADER = '''
#ifdef ALPACACORE_ENABLE_BISQUE
#define CS_BISQUE(X) X(bisque_telescope)
#else
#define CS_BISQUE(X)
#endif
#ifdef ALPACACORE_ENABLE_GEMINI
#define CS_GEMINI(X) \\
    X(gemini_switch)
#else
#define CS_GEMINI(X)
#endif
#ifdef ALPACACORE_ENABLE_PLAYERONE
#define CS_PLAYERONE(X) X(ioptron_camera)
#else
#define CS_PLAYERONE(X)
#endif
#define CONTRACT_SWEEP_ENTRIES(X) CS_BISQUE(X) CS_GEMINI(X) CS_PLAYERONE(X)
'''

FIX_CMAKE = '''
set(TEST_SOURCES
    test_a.cpp
    test_contract_sweep.cpp
)
if(TARGET alpacacore_bisque)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_bisque)
    target_compile_definitions(alpacacore_tests PRIVATE ALPACACORE_ENABLE_BISQUE)
endif()
if(TARGET alpacacore_gemini)
    target_link_libraries(alpacacore_tests PRIVATE alpacacore_gemini)
    target_compile_definitions(alpacacore_tests PRIVATE ALPACACORE_ENABLE_GEMINI)
endif()
if(TARGET alpacacore_playerone)
    target_compile_definitions(alpacacore_tests PRIVATE ALPACACORE_ENABLE_PLAYERONE)
endif()
'''

FIX_SWEEP = "CONTRACT_SWEEP_ENTRIES(CS_T1_INVALID)\n"


def _run(router=FIX_ROUTER, header=FIX_HEADER, cmake=FIX_CMAKE, sweep=FIX_SWEEP) -> list[str]:
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        for rel, text in ((ROUTER, router), (REGISTRY, header), (TESTS_CMAKE, cmake), (SWEEP_TEST, sweep)):
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text, encoding="utf-8")
        return check(root)


def self_test() -> int:
    global ALLOWLIST
    problems: list[str] = []

    def expect(name: str, failures: list[str], needle: str | None):
        if needle is None:
            if failures:
                problems.append("%s: expected clean, got %r" % (name, failures))
        elif not any(needle in f for f in failures):
            problems.append("%s: expected a finding containing %r, got %r" % (name, needle, failures))

    saved = dict(ALLOWLIST)
    try:
        ALLOWLIST = {}
        # Parser: comment/string braces do not end the function; the second
        # function's arm is not attributed to the first.
        got = router_pairs(FIX_ROUTER)
        if got != {("bisque", "telescope"), ("gemini", "switch"), ("ioptron", "camera")}:
            problems.append("router_pairs parsed %r" % sorted(got))
        ent = registry_entries(FIX_HEADER)
        if set(ent) != {"bisque_telescope", "gemini_switch", "ioptron_camera"}:
            problems.append("registry_entries parsed %r" % sorted(ent))
        if ent.get("gemini_switch") != {"ALPACACORE_ENABLE_GEMINI"}:
            problems.append("continuation-line entry lost its guard: %r" % ent.get("gemini_switch"))

        expect("clean fixture", _run(), None)

        # Rule 1: a constructed pair missing from the registry.
        expect("missing pair", _run(header=FIX_HEADER.replace("X(gemini_switch)", "")), "UNSWEPT PAIR")
        expect("new router arm", _run(router=FIX_ROUTER.replace(
            "return false;\n}", 'if (vendor == "qhy" && device_type_str == "focuser") { return true; }\n    return false;\n}', 1)),
            "UNSWEPT PAIR: the router constructs qhy/focuser")
        # ... which an allow-list with a reason satisfies.
        ALLOWLIST = {("gemini", "switch"): "reason"}
        expect("allow-listed", _run(header=FIX_HEADER.replace("X(gemini_switch)", "")), None)
        # Rule 2: stale allow-list, both directions.
        expect("stale: now covered", _run(), "STALE ALLOWLIST ENTRY: gemini/switch is now in the registry")
        ALLOWLIST = {("nosuch", "camera"): "reason"}
        expect("stale: no arm", _run(), "STALE ALLOWLIST ENTRY: nosuch/camera is no longer constructed")
        ALLOWLIST = {}
        # Rule 3: orphan and guard mismatch.
        expect("orphan", _run(header=FIX_HEADER.replace("X(bisque_telescope)", "X(bisque_telescope) X(bisque_camera)")),
               "ORPHAN ENTRY: bisque_camera")
        expect("guard mismatch", _run(header=FIX_HEADER.replace(
            "#ifdef ALPACACORE_ENABLE_GEMINI\n#define CS_GEMINI", "#ifdef ALPACACORE_ENABLE_ZWO\n#define CS_GEMINI")),
            "GUARD MISMATCH: gemini_switch")
        expect("override guard is required", _run(header=FIX_HEADER.replace(
            "#ifdef ALPACACORE_ENABLE_PLAYERONE", "#ifdef ALPACACORE_ENABLE_IOPTRON")), "GUARD MISMATCH: ioptron_camera")
        # Rule 4: the guard is not defined for the tests, or in the wrong block.
        expect("guard undefined", _run(cmake=FIX_CMAKE.replace(
            "    target_compile_definitions(alpacacore_tests PRIVATE ALPACACORE_ENABLE_GEMINI)\n", "")),
            "GUARD NOT DEFINED FOR TESTS: ALPACACORE_ENABLE_GEMINI")
        expect("guard in wrong block", _run(cmake=FIX_CMAKE.replace(
            "if(TARGET alpacacore_playerone)", "if(TARGET alpacacore_gemini)")), "GUARD DEFINED OUTSIDE ITS TARGET")
        # Rule 5: the sweep source is not compiled / does not expand the registry.
        expect("not compiled", _run(cmake=FIX_CMAKE.replace("    test_contract_sweep.cpp\n", "")), "SWEEP NOT COMPILED")
        expect("not compiled (conditional)", _run(cmake=FIX_CMAKE.replace(
            "    test_contract_sweep.cpp\n", "").replace(
            "if(TARGET alpacacore_bisque)", "list(APPEND TEST_SOURCES test_contract_sweep.cpp)\nif(TARGET alpacacore_bisque)")),
            "SWEEP NOT COMPILED")
        expect("not expanded", _run(sweep="// nothing\n"), "SWEEP DOES NOT EXPAND THE REGISTRY")
        # Robustness: a moved router function is a finding, not a silent pass.
        expect("router moved", _run(router="int x;\n"), "NO ROUTER ARMS FOUND")
        expect("empty registry", _run(header="// empty\n"), "EMPTY REGISTRY")
    finally:
        ALLOWLIST = saved

    if problems:
        print("check_contract_sweep self-test FAILED:")
        for p in problems:
            print("  " + p)
        return 1
    print("check_contract_sweep self-test OK")
    return 0


if __name__ == "__main__":
    if "--self-test" in sys.argv[1:]:
        sys.exit(self_test())
    sys.exit(main())
