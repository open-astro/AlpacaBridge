#!/usr/bin/env python3
"""Fail when the repo's own docs/config disagree with each other or with reality.

None of these facts are cross-checked anywhere else, so each has drifted
silently in the past (see AGENTS.md's own admissions and the 2026-09
harness-readiness evaluation that prompted this script). Every check below
is read-only and file-local -- no network, no build.

Run from the repo root:  python3 scripts/check_docs_drift.py

Checks:
  1. Every ALPACACORE_ENABLE_* CMake option is documented in the
     docs/development.md build-options table.
  2. The zizmor pinned version + sha256 are identical between ci.yml and
     ci_preflight.sh.
  3. The cppcheck --suppress list is identical between ci.yml and
     ci_preflight.sh (AGENTS.md: "Keep the cppcheck --suppress list
     identical between ci.yml and ci_preflight.sh").
  4. VERSION matches the version in the README's changelog badge line.
  5. The drivers whose `get_connected()` blocks are exactly the ones
     `async_connectable.h` names as blocking. Measured from the code, not
     restated: the router rule ("never call get_connected() while
     get_connecting() is true") depends on knowing which drivers have that
     shape, and the list has drifted repeatedly in both directions
     (issues #315, #355, #381, #407). The same check also fails on a COUNT
     of either list ("the five telescopes", "four wrapper-backed switches")
     stated anywhere the globs below reach: a number is a second source of
     truth for a list that is already gated by name, and it is what went
     stale before. CHANGELOG.md is exempt -- its entries describe what was
     true when they were written.
  6. The QHYSDK seam's three parallel lists agree: every pure virtual on the
     interface has a LockedQHYSDK override, every override actually takes the
     mutex, and the forward sweep in test_qhy_fake_sdk.cpp drives all of them.
  7. Every relative path referenced in AGENTS.md's inline code spans
     (`` `AlpacaCore/...` ``, `` `scripts/...` ``, `` `docs/...` ``, etc.)
     that looks like a real repo path actually exists.
  8. The TSan job's filtered runs are identical between ci.yml and
     ci_preflight.sh: the same set of `alpacacore_tests "<tag>"` invocations,
     and the same zero-test grep pattern guarding each one (issue #341). The
     pre-flight script only has value while it runs what CI runs, and this
     pair is written out twice with nothing comparing it -- the same shape as
     checks 2 and 3.
"""

import glob
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read(path):
    return (ROOT / path).read_text(encoding="utf-8", errors="replace")


# --- check 1: CMake options vs docs/development.md --------------------------

CMAKE_OPTION_RE = re.compile(r"option\((ALPACACORE_ENABLE_\w+)\b")
DOCS_OPTION_RE = re.compile(r"`(ALPACACORE_ENABLE_\w+)`")


def check_cmake_options_documented():
    failures = []
    cmake_text = read("AlpacaCore/CMakeLists.txt")
    cmake_options = set(CMAKE_OPTION_RE.findall(cmake_text))

    docs_text = read("docs/development.md")
    docs_options = set(DOCS_OPTION_RE.findall(docs_text))

    missing = sorted(cmake_options - docs_options)
    for opt in missing:
        failures.append(
            "docs/development.md build-options table is missing %s "
            "(defined in AlpacaCore/CMakeLists.txt)" % opt
        )
    return failures


# --- check 2: zizmor pin sync ------------------------------------------------

def check_zizmor_pin_sync():
    failures = []
    ci = read(".github/workflows/ci.yml")
    preflight = read("scripts/ci_preflight.sh")

    # ci.yml has more than one `ver=`/`sha256=` shell pin (cppcheck's
    # from-source build has its own); scope to the "Install zizmor" step so
    # this doesn't accidentally read cppcheck's pin instead.
    ci_block = _scoped_block(ci, "- name: Install zizmor", ("- name:",))
    if ci_block is None:
        failures.append("could not find the 'Install zizmor' step in ci.yml")
        return failures

    ci_ver = re.search(r"^\s*ver=([0-9.]+)\s*$", ci_block, re.MULTILINE)
    ci_sha = re.search(r"^\s*sha256=([0-9a-f]{64})\s*$", ci_block, re.MULTILINE)
    pf_ver = re.search(r'^ZIZMOR_VER="([0-9.]+)"', preflight, re.MULTILINE)
    pf_sha = re.search(r'^ZIZMOR_SHA256="([0-9a-f]{64})"', preflight, re.MULTILINE)

    if not (ci_ver and ci_sha and pf_ver and pf_sha):
        failures.append(
            "could not find zizmor ver/sha256 pins in both ci.yml (zizmor "
            "job) and ci_preflight.sh (ZIZMOR_VER/ZIZMOR_SHA256) -- update "
            "this check's regexes if the format changed"
        )
        return failures

    if ci_ver.group(1) != pf_ver.group(1):
        failures.append(
            "zizmor version mismatch: ci.yml has %s, ci_preflight.sh has %s"
            % (ci_ver.group(1), pf_ver.group(1))
        )
    if ci_sha.group(1) != pf_sha.group(1):
        failures.append(
            "zizmor sha256 mismatch: ci.yml has %s, ci_preflight.sh has %s"
            % (ci_sha.group(1), pf_sha.group(1))
        )
    return failures


# --- check 3: cppcheck --suppress list sync ---------------------------------

SUPPRESS_RE = re.compile(r"--suppress=(\S+)")


def _scoped_block(text, start_marker, end_markers):
    """text from start_marker to the first of end_markers found after it (or EOF).

    Used to scope a regex scan to one CI step/gate instead of the whole file,
    the same way check_zizmor_pin_sync does -- a file-wide scan is only safe
    while the flag being matched (--suppress=, ver=, ...) appears nowhere
    else in the file, which is an assumption worth pinning down rather than
    leaving implicit.
    """
    start = text.find(start_marker)
    if start == -1:
        return None
    end = len(text)
    for marker in end_markers:
        pos = text.find(marker, start + len(start_marker))
        if pos != -1:
            end = min(end, pos)
    return text[start:end]


def check_cppcheck_suppress_sync():
    failures = []
    ci_full = read(".github/workflows/ci.yml")
    preflight_full = read("scripts/ci_preflight.sh")

    # Both files currently have exactly one --suppress= invocation, so a
    # whole-file scan happens to be equivalent to a scoped one today -- but
    # scope explicitly anyway (mirroring check_zizmor_pin_sync) so this stays
    # correct if a second tool with its own --suppress flag is ever added to
    # either file.
    ci = _scoped_block(ci_full, "- name: Analyze changed C/C++ files", ("\n  zizmor:",))
    preflight = _scoped_block(preflight_full, 'section "cppcheck (changed files)"', ('section "',))
    if ci is None or preflight is None:
        failures.append(
            "could not find the cppcheck step in ci.yml or the cppcheck "
            "gate in ci_preflight.sh -- update this check's markers if "
            "either file's structure changed"
        )
        return failures

    ci_suppress = SUPPRESS_RE.findall(ci)
    pf_suppress = SUPPRESS_RE.findall(preflight)

    if not ci_suppress or not pf_suppress:
        failures.append(
            "could not find --suppress= flags in both ci.yml and "
            "ci_preflight.sh -- update this check's regex if the cppcheck "
            "invocation changed"
        )
        return failures

    if set(ci_suppress) != set(pf_suppress):
        failures.append(
            "cppcheck --suppress list differs: ci.yml has %s, "
            "ci_preflight.sh has %s"
            % (sorted(set(ci_suppress)), sorted(set(pf_suppress)))
        )
    return failures


# --- check 4: VERSION vs README badge ---------------------------------------

def check_version_matches_readme():
    failures = []
    version = read("VERSION").strip()
    readme = read("README.md")

    m = re.search(r"^####\s*\[([0-9.]+)\]\s*-\s*[0-9-]+\s*&middot;\s*\[Changelog\]", readme, re.MULTILINE)
    if not m:
        failures.append("could not find the version badge line in README.md")
        return failures

    badge_version = m.group(1)
    if badge_version != version:
        failures.append(
            "VERSION (%s) does not match the README badge version (%s)"
            % (version, badge_version)
        )
    return failures


# --- check 5: the blocking-get_connected() list vs the code -----------------
#
# issue #381. Four comments and prose passages used to hand-maintain COUNTS of
# this split; nothing checked them and they drifted repeatedly, including a
# stale count introduced by the PR that was correcting the others. The counts
# are gone (the rule is stated instead), but the NAMED lists remain
# load-bearing: `async_connectable.h`'s connection-task tail must read
# get_connected() before taking pending_mutex_ precisely because these drivers
# nest the driver mutex outside it, and the router must never read
# get_connected() mid-task because these drivers block on it. A reader who
# trusts a stale list is misled in the direction that matters -- believing a
# driver is safe to read mid-task when it blocks.
#
# So the code is the source of truth here and the comment must follow it: every
# get_connected() override under AlpacaCore/src/vendors is classified by its
# body, and the two blocking classes must be named in that header. A driver
# whose file is not in DRIVER_PROSE_NAMES fails loudly rather than being
# bucketed silently -- add it there AND to the header's list in the same
# change.

GET_CONNECTED_RE = re.compile(r"bool\s+get_connected\s*\(\s*\)\s*const\s+override\s*\{")

# Driver file basename -> the name the async_connectable.h comment uses for it.
# Only the drivers that CAN be classified as blocking need an entry; the
# lock-free majority is not named anywhere, by design.
# The value is (prose name, which list it belongs to). The list matters: the
# header names these in TWO sentences, and one name is a prefix of another
# across them -- "iOptron" (telescope) inside "iOptron iMate PowerBox"
# (switch). A plain `name in header` test therefore answered wrongly in both
# directions: dropping iOptron from the telescope list still "found" it via the
# switch entry (the exact drift this check exists to catch, passing green), and
# a correctly-removed iOptron reported a STALE entry that could not be resolved
# without editing an unrelated sentence. Each name is now looked for only in
# its own list.
DRIVER_PROSE_NAMES = {
    "bisque_telescope_driver.cpp": ("Bisque", "telescopes"),
    "celestron_telescope_driver.cpp": ("Celestron", "telescopes"),
    "ioptron_telescope_driver.cpp": ("iOptron", "telescopes"),
    "onstep_telescope_driver.cpp": ("OnStep", "telescopes"),
    "skywatcher_telescope_driver.cpp": ("Sky-Watcher", "telescopes"),
    "ioptron_switch_driver.cpp": ("iOptron iMate PowerBox", "switches"),
    "touptek_switch_driver.cpp": ("ToupTek StellaVita", "switches"),
    "zwo_asiair_switch_driver.cpp": ("ZWO ASIAIR", "switches"),
    "zwo_asiair_plus_switch_driver.cpp": ("ASIAIR Plus", "switches"),
}

# Where each list lives in async_connectable.h, as (start marker, end marker).
# Both must be found or the check fails loudly: a header rewrite that moves
# them must not silently turn this gate into a no-op.
BLOCKING_LIST_SPANS = {
    "telescopes": ("create that hazard are the", "telescopes."),
    "switches": ("The wrapper-backed switch drivers (", ")"),
}


def _blocking_list_spans(header):
    """{list name: text} for each blocking list, or ({}, [failure])."""
    # Strip `//` comment markers and collapse the wrapping so a name split
    # across two comment lines still matches.
    flat = " ".join(line.strip().lstrip("/").strip() for line in header.splitlines())
    flat = re.sub(r"\s+", " ", flat)
    spans = {}
    failures = []
    for name, (start, end) in BLOCKING_LIST_SPANS.items():
        i = flat.find(start)
        j = flat.find(end, i + len(start)) if i >= 0 else -1
        if i < 0 or j < 0:
            failures.append(
                "BLOCKING-LIST SPAN NOT FOUND: could not locate the '%s' list in "
                "async_connectable.h (looked for %r ... %r). The list was moved or reworded -- "
                "update BLOCKING_LIST_SPANS in %s, or this check silently stops checking."
                % (name, start, end, Path(__file__).name))
            continue
        spans[name] = flat[i:j + len(end)]
    return spans, failures


def _names_in_span(which, span):
    """The driver names a blocking-list sentence actually lists."""
    if which == "telescopes":
        body = span.split("create that hazard are the", 1)[1]
        body = body.rsplit("telescopes.", 1)[0]
    else:
        body = span.split("(", 1)[1].rsplit(")", 1)[0]
    # "A, B, C and D" -> [A, B, C, D]
    parts = []
    for chunk in body.split(","):
        parts.extend(re.split(r"\band\b", chunk))
    return [p.strip() for p in parts if p.strip()]

QHY_INTERFACE_HEADER = "AlpacaCore/include/alpacacore/vendor/qhy/qhy_sdk_wrapper.h"
QHY_LOCKED_HEADER = "AlpacaCore/tests/locked_qhy_sdk.h"
QHY_SWEEP_TEST = "AlpacaCore/tests/test_qhy_fake_sdk.cpp"

# The cv/exception qualifiers between the closing paren and `= 0` / `override`
# are optional but must be TOLERATED: without them a `const` method is invisible
# to both patterns, so a 27th pure virtual that happens to be const would be
# omitted from every set, all three differences would come out empty, and the
# gate would pass on exactly the drift it exists to catch. (A developer who did
# add the sweep entry got the opposite: a STALE SWEEP ENTRY naming the wrong
# cause.) Nothing on this seam is const today, so the hole was only reachable
# by the next method added -- which is the whole population this gate is for.
_QUALIFIERS = r"(?:\s*(?:const|noexcept|final|override))*"
PURE_VIRTUAL_RE = re.compile(
    r"\bvirtual\b[^;{}]*?(\w+)\s*\([^;{}]*\)" + _QUALIFIERS + r"\s*=\s*0\s*;", re.S)
OVERRIDE_RE = re.compile(
    r"(\w+)\s*\([^;{}]*\)(?:\s*(?:const|noexcept|final))*\s*override\s*\{", re.S)
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")


def _strip_comments(text):
    """Comments blanked (newlines kept). These headers carry long doc comments
    whose prose contains parentheses and identifiers, and both patterns above
    scan across whitespace -- without this a sentence in a comment is matched
    as a method signature. No raw string literals exist in either header."""
    text = BLOCK_COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return LINE_COMMENT_RE.sub("", text)


def _matching_brace(text, open_index):
    """Index just past the `}` closing the `{` at open_index."""
    depth = 0
    for i in range(open_index, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
    return len(text)


def classify_get_connected_bodies():
    """({basename: kind}, [findings]) for every vendor get_connected() override.

    kind is "driver-mutex" (takes a lock_guard/unique_lock, so it blocks behind
    the connect sequence that holds the same mutex), "wrapper" (reaches the
    vendor wrapper's is_open(), which takes the wrapper mutex that open() holds
    throughout), or "lock-free".
    """
    kinds = {}
    failures = []
    vendors = ROOT / "AlpacaCore" / "src" / "vendors"
    for path in sorted(vendors.rglob("*")):
        if path.suffix not in (".cpp", ".h", ".hpp"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in GET_CONNECTED_RE.finditer(text):
            open_index = match.end() - 1
            body = text[open_index:_matching_brace(text, open_index)]
            if re.search(r"\b(lock_guard|unique_lock|scoped_lock)\b", body):
                kind = "driver-mutex"
            elif re.search(r"(\.|->)is_open\s*\(\s*\)", body):
                kind = "wrapper"
            elif re.search(r"\bload\s*\(|\breturn\s+connected_\s*;", body):
                kind = "lock-free"
            else:
                failures.append(
                    "UNCLASSIFIABLE get_connected(): %s -- it neither takes a lock, nor reaches a "
                    "wrapper's is_open(), nor reads an atomic. Classify it by hand: if it can block, "
                    "add it to DRIVER_PROSE_NAMES in %s and to the list in async_connectable.h; if it "
                    "cannot, make that visible in the body (an atomic load or a plain return)."
                    % (path.relative_to(ROOT), Path(__file__).name))
                continue
            # A file with two driver classes (gemini_flatpanel_driver.cpp) is
            # only interesting if they disagree, which would mean the file
            # cannot be named as one thing in the prose list.
            if kinds.get(path.name, kind) != kind:
                failures.append(
                    "MIXED get_connected() kinds in %s: %s and %s. The prose list names files, not "
                    "classes, so split the drivers or name them individually."
                    % (path.relative_to(ROOT), kinds[path.name], kind))
            kinds[path.name] = kind
    return kinds, failures


def check_blocking_get_connected_list():
    kinds, failures = classify_get_connected_bodies()
    if not kinds:
        return ["No get_connected() overrides found under AlpacaCore/src/vendors -- the scan is broken."]

    header = read("AlpacaCore/include/alpacacore/async_connectable.h")
    spans, span_failures = _blocking_list_spans(header)
    failures.extend(span_failures)
    if span_failures:
        return failures

    listed_names = {which: set(_names_in_span(which, span)) for which, span in spans.items()}

    def named(entry):
        # Whole parsed names, not a substring of the sentence: substring
        # matching is only safe here because no name is a prefix of another
        # WITHIN one list today, and that is not a property worth depending on
        # (it already failed across the two lists -- "iOptron" inside "iOptron
        # iMate PowerBox").
        prose, which = entry
        return prose in listed_names[which]

    # Parse the names OUT of each list, so a name that should not be there is
    # caught even when no driver file maps to it. Probing only for the names in
    # DRIVER_PROSE_NAMES could never see that: the dict holds only the drivers
    # that ARE blocking, so a lock-free driver named in the header (SynScan
    # after #130 -- the drift this whole check exists for) matched nothing and
    # passed.
    expected = {which: set() for which in BLOCKING_LIST_SPANS}
    for basename, kind in kinds.items():
        entry = DRIVER_PROSE_NAMES.get(basename)
        if entry is not None and kind != "lock-free":
            expected[entry[1]].add(entry[0])
    for which, span in spans.items():
        for listed in _names_in_span(which, span):
            if listed not in expected[which]:
                failures.append(
                    "STALE BLOCKING-LIST ENTRY: async_connectable.h's %s list names '%s', but no "
                    "driver with that name has a blocking get_connected(). Either it was made "
                    "lock-free and the name must come out, or the name does not match "
                    "DRIVER_PROSE_NAMES." % (which, listed))

    # The counts this PR removed must not creep back. A number in front of
    # "telescopes"/"wrapper-backed switch(es)" is exactly the thing nothing
    # checks and that went stale repeatedly -- the lists themselves are gated
    # above, so a count adds nothing but a second source of truth.
    count_re = re.compile(
        r"\b(two|three|four|five|six|seven|eight|nine|ten|\d+)\s+"
        r"(?:named\s+|more\s+)?(?:telescopes?|wrapper-backed\s+switch(?:es)?)\b",
        re.IGNORECASE)
    # Anchored on the noun deliberately. A rule that also caught a bare "the
    # five" (noun implied) was tried and dropped: "the two locked phases", "the
    # two ZWO drivers" and similar ordinary prose light it up, so it fails on
    # correct text. A count with the noun left implied has to be caught by
    # review -- which is how the one at AGENTS.md:777 was.
    # Globbed, not a hand-written file list: the first version of this loop
    # named four files and missed synscan_telescope_driver.cpp, which carried
    # the counts -- a gate against drift that itself drifts is worth very
    # little. CHANGELOG.md is deliberately out of scope: its historical entries
    # describe what was true when they were written.
    # ROOT.glob, not glob.glob: the latter is cwd-relative, so running this
    # from anywhere but the repo root would return nothing and turn the rule
    # into a silent no-op -- the same failure shape as the hand-written file
    # list it replaced. Headers and tests are in scope too; a count is just as
    # stale in test_async_connectable.cpp as in a driver.
    counted_paths = sorted(
        str(p.relative_to(ROOT))
        for pattern in ("AGENTS.md", "README.md", "docs/**/*.md",
                        "AlpacaCore/include/**/*.h", "AlpacaCore/src/**/*.h",
                        "AlpacaCore/src/**/*.cpp", "AlpacaCore/tests/**/*.h",
                        "AlpacaCore/tests/**/*.cpp",
                        "AlpacaHTTP/include/**/*.h", "AlpacaHTTP/src/**/*.cpp",
                        "AlpacaHTTP/tests/**/*.cpp")
        for p in ROOT.glob(pattern))
    for path in counted_paths:
        for match in count_re.finditer(read(path)):
            failures.append(
                "COUNTED BLOCKING DRIVERS: %s says %r. These lists are gated by name; a count is a "
                "second source of truth that nothing checks and that has gone stale before "
                "(issue #381). Refer to the named list instead." % (path, match.group(0)))

    for basename, kind in sorted(kinds.items()):
        if kind == "lock-free":
            # The lock-free majority is deliberately unnamed. What matters is
            # that it is not named as blocking: a driver made lock-free (as
            # SynScan was by #130) must come OUT of the list.
            entry = DRIVER_PROSE_NAMES.get(basename)
            if entry and named(entry):
                failures.append(
                    "STALE BLOCKING-LIST ENTRY: %s's get_connected() is lock-free, but "
                    "async_connectable.h still names '%s' among the drivers that block. Remove it there "
                    "and from DRIVER_PROSE_NAMES." % (basename, entry[0]))
            continue
        entry = DRIVER_PROSE_NAMES.get(basename)
        if entry is None:
            failures.append(
                "UNNAMED BLOCKING DRIVER: %s's get_connected() is %s, so it blocks behind its connect "
                "sequence, but no prose name is registered for it. Add it to DRIVER_PROSE_NAMES in %s "
                "and to the list in async_connectable.h -- the router rule and the pending_mutex_ "
                "ordering both depend on that list being complete." % (basename, kind, Path(__file__).name))
            continue
        if not named(entry):
            failures.append(
                "MISSING FROM THE BLOCKING LIST: %s's get_connected() is %s, but async_connectable.h's "
                "%s list does not name '%s'." % (basename, kind, entry[1], entry[0]))
    return failures


# --- check 6: the QHYSDK seam's three parallel lists ------------------------
#
# issue #394. The forward sweep in test_qhy_fake_sdk.cpp drives every QHYSDK
# method through LockedQHYSDK and asserts each landed on its own counterpart
# exactly once, which is a genuine (mutation-verified) guard against a
# TRANSPOSED forward. It is not a guard against an ABSENT one: the method list
# is hand-written in the test and closed with `methods.size() == N`, a literal
# compared to a literal. Add a pure virtual to QHYSDK and the compiler forces a
# LockedQHYSDK override -- the class would otherwise be abstract -- but nothing
# forces a test entry, so the sweep passes having exercised N of N+1 forwards.
#
# And the compiler only guarantees the forward EXISTS. Nothing guarantees it
# takes the mutex, which is the only reason the decorator exists: its job is to
# keep ThreadSanitizer findings pointing at driver code rather than at the
# deliberately unhardened fake, and one unlocked forward makes the fake racy
# under a storm and produces a TSan report naming the fake -- the exact
# confusion the decorator was built to prevent, arriving silently.


def _class_body(text, class_name):
    """The text between `class <name> ... {` and its matching brace, or None."""
    match = re.search(r"\bclass\s+%s\b[^{;]*\{" % re.escape(class_name), text)
    if not match:
        return None
    return text[match.end():_matching_brace(text, match.end() - 1) - 1]


def check_qhy_seam_lists():
    failures = []
    interface_body = _class_body(_strip_comments(read(QHY_INTERFACE_HEADER)), "QHYSDK")
    locked_body = _class_body(_strip_comments(read(QHY_LOCKED_HEADER)), "LockedQHYSDK")
    if interface_body is None or locked_body is None:
        return ["Could not locate class QHYSDK and/or class LockedQHYSDK -- this check's parser is broken."]

    interface_methods = set(PURE_VIRTUAL_RE.findall(interface_body))
    if not interface_methods:
        return ["No pure virtuals found on QHYSDK -- this check's parser is broken."]

    # Cross-check the count against the literal the sweep test already asserts
    # (CHECK(methods.size() == N)). Everything else here is set differences, so
    # a method the regex fails to see drops out of ALL of them and the gate goes
    # quietly green -- which is exactly what a const method did until round 2.
    # Comparing against a number maintained elsewhere turns the next such miss
    # into a loud failure, and pins that literal at the same time.
    sweep_text = read(QHY_SWEEP_TEST)
    size_match = re.search(r"\bmethods\.size\(\)\s*==\s*(\d+)", sweep_text)
    if size_match is None:
        failures.append(
            "Could not find the `CHECK(methods.size() == N)` literal in %s -- this check uses it to "
            "detect a method its regexes silently missed, so losing it would make that failure "
            "invisible again." % QHY_SWEEP_TEST)
    elif int(size_match.group(1)) != len(interface_methods):
        failures.append(
            "QHY SEAM COUNT MISMATCH: %s asserts methods.size() == %s, but %d pure virtuals were "
            "parsed off QHYSDK. Either the sweep literal is stale, or a method's declaration has a "
            "shape the parser in %s does not match (a const or noexcept qualifier did exactly that "
            "once) -- in which case the set comparisons below would pass while missing it."
            % (QHY_SWEEP_TEST, size_match.group(1), len(interface_methods), Path(__file__).name))

    # Each override, with its body, so the lock can be checked too.
    locked_methods = {}
    for match in OVERRIDE_RE.finditer(locked_body):
        open_index = match.end() - 1
        locked_methods[match.group(1)] = locked_body[open_index:_matching_brace(locked_body, open_index)]

    for name in sorted(interface_methods - set(locked_methods)):
        failures.append(
            "QHYSDK::%s() has no LockedQHYSDK override. (If this fires, the parser in %s is wrong: an "
            "unimplemented pure virtual would make LockedQHYSDK abstract and fail the build.)"
            % (name, Path(__file__).name))
    for name in sorted(set(locked_methods) - interface_methods):
        failures.append(
            "LockedQHYSDK::%s() overrides nothing on QHYSDK -- stale forward, or the interface lost a "
            "method." % name)

    # cancel_exposure is the one documented exception (open-astro#339): the real
    # QHYSDKWrapper::cancel_exposure() deliberately skips the per-handle call
    # mutex so it can interrupt a GetQHYCCDSingleFrame already blocked on the
    # same handle, and routing it through the shared mutex here inverted that
    # invariant. It still takes A mutex -- its own -- so it is not an unlocked
    # forward and the fake stays non-racy; it just does not queue behind the
    # call it exists to interrupt.
    CANCEL_EXEMPT = "cancel_exposure"
    for name in sorted(set(locked_methods) & interface_methods):
        body = locked_methods[name]
        if name == CANCEL_EXEMPT:
            # Mechanical, not a substring: the body must take the lock_guard on
            # cancel_mutex_ AND must not go through locked(). A body that does
            # both (shared mutex plus a mention of cancel_mutex_) is the
            # inverted shape #339 fixed, and a substring test passed it.
            # `lock_guard<std::mutex>` or CTAD `lock_guard` both count; any
            # mention of the shared `mutex_` (locked() or a hand-rolled guard)
            # is the inverted shape, so it is rejected by name, not by idiom.
            takes_own = re.search(r"lock_guard\s*(?:<\s*std::mutex\s*>)?\s*\w+\s*\(\s*cancel_mutex_\s*\)", body)
            touches_shared = "locked(" in body or re.search(r"(?<![\w])mutex_\b", body)
            if not takes_own or touches_shared:
                failures.append(
                    "LockedQHYSDK::cancel_exposure() must take cancel_mutex_ -- its own lock, separate "
                    "from the shared one, per open-astro#339. Through the shared mutex it queues behind "
                    "the in-flight call it exists to interrupt; with no mutex at all it becomes the one "
                    "racy forward in the decorator.")
            continue
        if "locked(" not in body:
            failures.append(
                "UNLOCKED FORWARD: LockedQHYSDK::%s() does not go through locked(). The decorator exists "
                "only to take the mutex -- an unlocked forward makes the fake racy under a [stress] storm "
                "and produces a ThreadSanitizer report naming the FAKE, which is the confusion the "
                "decorator was built to prevent." % name)

    # The hand-written sweep list in the test.
    sweep = read(QHY_SWEEP_TEST)
    list_match = re.search(r"const std::vector<std::string> methods\{(.*?)\};", sweep, re.S)
    if not list_match:
        failures.append(
            "Could not find the `const std::vector<std::string> methods{...}` sweep list in %s."
            % QHY_SWEEP_TEST)
        return failures
    swept = set(re.findall(r'"([^"]+)"', list_match.group(1)))
    for name in sorted(interface_methods - swept):
        failures.append(
            "NOT SWEPT: QHYSDK::%s() is not in the forward sweep's method list in %s. The sweep is what "
            "checks the forward reaches its own counterpart; a method missing from the list is verified "
            "by inspection only." % (name, QHY_SWEEP_TEST))
    for name in sorted(swept - interface_methods):
        failures.append(
            "STALE SWEEP ENTRY: %s is in the sweep list in %s but is not a QHYSDK method."
            % (name, QHY_SWEEP_TEST))
    return failures


# --- check 7: AGENTS.md path references exist -------------------------------

# Backtick-quoted spans that look like a repo-relative path: start with one of
# these top-level dirs/files (spaces allowed only for a verbatim tracked path), and are not a bare CLI flag
# or a URL.
PATH_PREFIXES = (
    "AlpacaCore/", "AlpacaHTTP/", "scripts/", "docs/", ".github/",
    ".claude/", "debian/",
)
# Spans are matched only after fenced code blocks are removed (their triple
# backticks would otherwise pair across lines and invert every later match,
# which silently dropped 66 of 83 path references in the first cut of this
# fix). With fences gone, pairing is consistent even for a span that wraps
# onto the next line; such a span is skipped, since a path never wraps.
# Fences: three or more backticks or tildes, optionally indented (list items).
FENCED_BLOCK_RE = re.compile(r"^[ \t]*(`{3,}|~{3,}).*?^[ \t]*\1[ \t]*$", re.S | re.M)
# A double-backtick span shows a literal backtick (`` ` ``) and would break
# single-backtick parity; it is never a path reference, so drop it first.
DOUBLE_BACKTICK_SPAN_RE = re.compile(r"``.+?``")
CODE_SPAN_RE = re.compile(r"`([^`]+)`")
# Tripwire for the span matcher, not a rule about document size: if
# AGENTS.md is legitimately trimmed below this, lower the floor.
MIN_AGENTS_MD_PATH_REFS = 50
# Trailing punctuation/anchors that can ride along inside a backtick span.
TRIM_SUFFIX_RE = re.compile(r"[),.;:]+$")


def _run_git(args, check=True):
    return subprocess.run(
        ["git"] + args, cwd=ROOT, capture_output=True, text=True, check=check
    )


def _is_gitignored(path):
    """True if git would ignore this path (e.g. a generated file/dir).

    Used instead of a plain filesystem exists() check: a generated file like
    debian/changelog can be present in one developer's tree from a past
    local build (making exists() pass by accident there) while being absent
    from every clean checkout, including CI's. A path git ignores is
    expected to be absent and isn't a documentation error.
    """
    return _run_git(["check-ignore", "-q", path], check=False).returncode == 0


def check_agents_md_paths_exist():
    failures = []
    text = FENCED_BLOCK_RE.sub("", read("AGENTS.md"))
    text = DOUBLE_BACKTICK_SPAN_RE.sub("", text)
    # With fences gone every backtick must pair up; one stray backtick would
    # invert every span after it, and the count floor below only catches a
    # large inversion. Fail loudly on parity instead.
    if text.count("`") % 2 != 0:
        return ["AGENTS.md has an unbalanced backtick outside fenced blocks; "
                "the path-reference check cannot pair code spans reliably"]
    seen = set()

    # core.quotePath=false: a tracked path with non-ASCII bytes must not
    # come back quoted, or it would never match a span.
    tracked = set(_run_git(["-c", "core.quotePath=false", "ls-files"]).stdout.splitlines())
    tracked_dirs = set()
    for f in tracked:
        parts = f.split("/")
        for i in range(1, len(parts)):
            tracked_dirs.add("/".join(parts[:i]) + "/")

    checked = 0
    for m in CODE_SPAN_RE.finditer(text):
        span = m.group(1)
        if "\n" in span or not span.startswith(PATH_PREFIXES):
            continue
        checked += 1
        path = TRIM_SUFFIX_RE.sub("", span)
        # Markdown anchors / fragments (`docs/x.md#section`), glob patterns,
        # and template placeholders (`AlpacaCore/src/vendors/<vendor>/...`)
        # aren't real filesystem paths.
        if "#" in path or "*" in path or "<" in path or ">" in path:
            continue
        if path in seen:
            continue
        seen.add(path)

        if path in tracked or path in tracked_dirs:
            continue
        # A span with whitespace is validated only when it names a tracked
        # file or directory verbatim (e.g. `AlpacaCore/conformu/Astroasis/
        # Oasis Focuser/`, handled above). Otherwise it is SKIPPED, not
        # failed: it may be prose (`AlpacaCore/tests/ and AlpacaHTTP/`) and
        # cannot be told apart from a drifted spaced path. So a spaced path
        # that later drifts stays green here; that is the accepted trade.
        if any(ch.isspace() for ch in path):
            continue
        # Not a tracked file or the directory of one: a generated/ignored
        # path (debian/changelog, a `.../build/` output dir) is expected to
        # be absent from a clean checkout, so it isn't a documentation error.
        if _is_gitignored(path):
            continue
        failures.append("AGENTS.md references a path that does not exist: %s" % path)
    if checked < MIN_AGENTS_MD_PATH_REFS:
        failures.append(
            "only %d backticked path references found in AGENTS.md (floor %d): "
            "either the code-span matcher regressed, or AGENTS.md was trimmed "
            "and MIN_AGENTS_MD_PATH_REFS should be lowered"
            % (checked, MIN_AGENTS_MD_PATH_REFS))
    return failures


# --- check 8: TSan filtered-run sync (ci.yml vs ci_preflight.sh) ------------

# Both files spell out the same filtered TSan runs -- today `[stress]` for the
# vendor registrations and `[stress-guard]` for the harness self-tests -- and
# each run carries its own zero-test grep, because a tag filter matching no
# tests exits 0 and Catch2 reports that as success. Nothing compared the two
# files, so editing one filtered run (retagging it, tightening the grep,
# adding a third invocation) could silently leave the other behind and the
# pre-flight would stop being a faithful mirror of CI. Issue #341.
#
# Matches both spellings of the invocation: bare in ci.yml
# (`alpacacore_tests "[stress]"`) and quoted-path in ci_preflight.sh
# (`"${TSAN_BUILD_DIR}/tests/alpacacore_tests" "[stress]"`). The trailing
# quoted tag is what distinguishes a real run from the `test -x` / `[ -x ... ]`
# existence probes on the same binary in both files.
TSAN_RUN_RE = re.compile(r'alpacacore_tests"?\s+"(\[[^"]+\])"')
TSAN_GREP_RE = re.compile(r"grep\s+-qE\s+'([^']+)'")

# A floor, not a count: it exists only so a regex that stops matching fails
# loudly instead of comparing two empty sets and passing. Deliberately NOT set
# to today's two runs -- the number of filtered runs is the check's subject,
# not a second place to state it (see check 5 on counts as a second source of
# truth).
MIN_TSAN_FILTERED_RUNS = 1


def check_tsan_filtered_runs_sync():
    failures = []
    ci_full = read(".github/workflows/ci.yml")
    preflight_full = read("scripts/ci_preflight.sh")

    ci = _scoped_block(ci_full, "  sanitizers-tsan:", ("\n  format:",))
    preflight = _scoped_block(
        preflight_full,
        'section "ThreadSanitizer (concurrency stress, all vendors)"',
        ("\n# --- ",),
    )
    if ci is None or preflight is None:
        failures.append(
            "could not find the sanitizers-tsan job in ci.yml or the "
            "ThreadSanitizer gate in ci_preflight.sh -- update this check's "
            "markers if either file's structure changed"
        )
        return failures

    ci_tags = sorted(TSAN_RUN_RE.findall(ci))
    pf_tags = sorted(TSAN_RUN_RE.findall(preflight))
    ci_greps = TSAN_GREP_RE.findall(ci)
    pf_greps = TSAN_GREP_RE.findall(preflight)

    for label, tags in (("ci.yml", ci_tags), ("ci_preflight.sh", pf_tags)):
        if len(tags) < MIN_TSAN_FILTERED_RUNS:
            failures.append(
                "found %d filtered TSan run(s) in %s (floor %d) -- either the "
                "invocation matcher regressed or the TSan gate was removed"
                % (len(tags), label, MIN_TSAN_FILTERED_RUNS)
            )
    if failures:
        return failures

    if ci_tags != pf_tags:
        failures.append(
            "TSan filtered runs differ: ci.yml runs %s, ci_preflight.sh runs "
            "%s -- a filtered run must be added, retagged or removed in both"
            % (ci_tags, pf_tags)
        )

    # One zero-test guard per filtered run, in each file. A run that loses its
    # grep goes green on zero tests, which is the whole reason the grep is
    # there; a count mismatch catches that without depending on the two
    # appearing in any particular order.
    for label, tags, greps in (
        ("ci.yml", ci_tags, ci_greps),
        ("ci_preflight.sh", pf_tags, pf_greps),
    ):
        if len(greps) != len(tags):
            failures.append(
                "%s has %d filtered TSan run(s) but %d zero-test grep(s) -- "
                "every filtered run needs its own guard, or the run reports "
                "success having executed nothing"
                % (label, len(tags), len(greps))
            )

    if set(ci_greps) != set(pf_greps):
        failures.append(
            "TSan zero-test grep pattern differs: ci.yml has %s, "
            "ci_preflight.sh has %s"
            % (sorted(set(ci_greps)), sorted(set(pf_greps)))
        )
    return failures


CHECKS = [
    ("CMake options documented in docs/development.md", check_cmake_options_documented),
    ("zizmor pin sync (ci.yml vs ci_preflight.sh)", check_zizmor_pin_sync),
    ("cppcheck --suppress sync (ci.yml vs ci_preflight.sh)", check_cppcheck_suppress_sync),
    ("VERSION matches README badge", check_version_matches_readme),
    ("Blocking get_connected() list matches the code", check_blocking_get_connected_list),
    ("AGENTS.md path references exist", check_agents_md_paths_exist),
    ("QHY SDK seam lists agree (interface / LockedQHYSDK / sweep)", check_qhy_seam_lists),
    ("TSan filtered runs sync (ci.yml vs ci_preflight.sh)", check_tsan_filtered_runs_sync),
]


def main():
    all_failures = []
    for name, fn in CHECKS:
        failures = fn()
        if failures:
            print("[FAIL] %s" % name)
            for f in failures:
                print("  - %s" % f)
        else:
            print("[PASS] %s" % name)
        all_failures.extend(failures)

    if all_failures:
        print("\n%d finding(s)." % len(all_failures))
        return 1

    print("\nDocs drift check OK.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
