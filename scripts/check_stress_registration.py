#!/usr/bin/env python3
"""Fail on [stress] tag mistakes: missing vendor coverage, and misplaced tags.

Five rule families, not one:

1. Every vendor (driver, device type) pair needs a `[stress]` TEST_CASE or an
   ALLOWLIST entry.
2. A `[stress-guard]`-without-`[stress]` case inside a `*_concurrency_stress.cpp`
   file is rejected: that tag is for harness self-tests, and a registration
   wearing it would read as registered while dropping out of the vendor count.
3. A `[stress]` case anywhere else under `AlpacaCore/tests/` is rejected: such
   a file can compile unconditionally, and one case alone then satisfies the
   TSan job's vendor zero-coverage grep and makes it vacuous. The scope is that
   directory deliberately -- `alpacacore_tests` is the only binary CI runs a
   tag filter against.
4. A registration file must be listed inside an `if(TARGET ...)` block in
   `AlpacaCore/tests/CMakeLists.txt`. Rule 3 assumes every `[stress]` case is
   in a conditionally-compiled file; this is the other half of that assumption
   (issue #396), and without it appending a correctly-named, correctly-tagged
   registration to the unconditional `TEST_SOURCES` list passes every other
   rule while making the TSan zero-test grep vacuous again.
5. A registration file must use `StressCallGuard` the way AGENTS.md documents
   it (issues #379, #334), or be named in `GUARD_ALLOWLIST` with a reason:
   the guard present, `CHECK(...unexpected_count())`, `INFO(...report())`,
   `CHECK(...total_calls())`, and no local `call()` helper in either of the
   two forms this repo has used (a file-scope function/template, or an
   `auto call = [...]` lambda). A stale allow-list entry is itself a failure.
   See check_guard_usage().

Rules 1-3 and 5 read C++ with the comments stripped (issue #386), so an illustrative
case macro in a doc comment -- `concurrency_stress.h` is the documentation hub
for this pattern -- is documentation, not a registration. String literals are
deliberately NOT stripped. The scans also cover every Catch2 registration
macro, not just `TEST_CASE`, and join adjacent tag literals the way the
preprocessor does (issue #393).


AGENTS.md requires every new or substantially-changed driver to register a
`[stress]` TEST_CASE with the ThreadSanitizer concurrency suite
(`AlpacaCore/tests/concurrency_stress.h`, wired into the `sanitizers-tsan` CI
job). Registration is manual today, and nothing failed when it was skipped.

Run from the repo root:  python3 scripts/check_stress_registration.py
Regex regression guard (no repo state needed):  python3 scripts/check_stress_registration.py --self-test

Coverage is tracked per (vendor, Alpaca device type) pair, not per vendor.
A vendor-level check (does `test_<vendor>_concurrency_stress.cpp` exist at
all?) would pass ZWO or ToupTek in full the moment any one of their drivers
is registered -- while the ZWO rotator, focuser and dew-heater switch
and the ToupTek focuser were still unregistered, that would have hidden
them behind a green check. Keying on device type as well catches those.

Known gap: the pair is the finest key the gate has, so a registered driver
masks every other driver of the same vendor and type. The two ZWO ASIAIR
switch drivers (`zwo_asiair_switch_driver.cpp`, `zwo_asiair_plus_switch_driver.cpp`,
libgpiod, no fake seam) have no [stress] case and are hidden behind the ZWO
dew-heater switch registration; they are covered by code review only.
Same shape for (gemini, covercalibrator): only Flat Panel Pro
(GeminiFlatPanelV2Driver, gemini_flatpanel_driver.cpp) is stormed, but that
also marks the Cover Lite class (GeminiFlatPanelDriver, same file, its own
set_connected/calibrator implementation, no task threads) and the Rev2
model path as covered; both are code-review only, not exercised.

The device type for a driver file is read from its own
`get_device_type() const override { return DeviceType::X; }` rather than
guessed from the filename: `gemini_flatpanel_driver.cpp` actually returns
DeviceType::CoverCalibrator, so a filename-based guess would be wrong.

New driver, no stress test yet? Either add the `[stress]` TEST_CASE in a
`*_concurrency_stress.cpp` file gated behind the vendor's `if(TARGET ...)`
block (see `test_touptek_concurrency_stress.cpp` for the shape: one factory +
one operate callback), or add the (vendor, device type) pair to ALLOWLIST below
with a comment. The allow-list is meant to shrink, not grow -- an entry left
in place after coverage is added will itself fail the check (see below), so
there is nothing to remember to clean up by hand.
"""

import os
import re
import subprocess
import sys

VENDORS_PREFIX = "AlpacaCore/src/vendors/"
# The registration-file glob is the naming convention AGENTS.md and this
# script's own failure messages state: `*_concurrency_stress.cpp`, with no
# `test_` prefix (issue #376). The prefix used to be load-bearing in two
# opposite directions -- find_registered_pairs() only scanned files that had
# it, so a registration without it did not count toward vendor coverage, and
# find_stray_stress_cases() excluded registration files by the same glob, so
# such a file was *rejected* as a stray [stress] case. The second is the
# sharper failure: an author following the documented naming got a rejection
# telling them the tag does not belong in the file whose whole purpose is to
# carry it. Every file in the tree happens to carry the prefix, so this widens
# what is accepted and rejects nothing new.
STRESS_TEST_GLOB = "AlpacaCore/tests/*_concurrency_stress.cpp"
STRESS_TEST_SUFFIX = "_concurrency_stress.cpp"
TESTS_CMAKELISTS = "AlpacaCore/tests/CMakeLists.txt"
# Every place a TEST_CASE can compile into alpacacore_tests. Headers are
# included because the tests/ helpers (fake_mount_server.h and friends) are
# #included by several TUs, so a TEST_CASE added to one would compile in and
# re-inflate the vendor threshold while slipping past a *.cpp-only scan.
#
# The .cpp glob is deliberately *.cpp and not test_*.cpp: AGENTS.md states the
# rule as "a [stress] case anywhere else under AlpacaCore/tests/", and a
# narrower glob would leave a file not named test_* outside the check while the
# prose said otherwise. Registration files are excluded by path in
# find_stray_stress_cases(), not by failing to match here. The .hpp/.cc/.inc/
# .ipp entries match nothing today and exist for the same reason: the prose
# says "anywhere else under AlpacaCore/tests/", so an extension the globs miss
# would be a silent hole rather than a documented limit.
TEST_GLOBS = ("AlpacaCore/tests/*.cpp", "AlpacaCore/tests/*.h",
              "AlpacaCore/tests/*.hpp", "AlpacaCore/tests/*.cc",
              "AlpacaCore/tests/*.inc", "AlpacaCore/tests/*.ipp")

# Anchored to the actual override, not just any DeviceType:: mention in the
# file -- a driver that referenced a different DeviceType::X earlier (a
# comment, a switch/comparison, a helper) before its own override would
# otherwise be silently miscategorized by a plain first-match search.
DEVICE_TYPE_OVERRIDE_RE = re.compile(
    r"get_device_type\s*\(\s*\)\s*const\s+override\s*\{\s*return\s+DeviceType::([A-Za-z]+)\s*;")
# Catch2 has more than one test-registration macro, and every one of them
# takes its tags in the same place (issue #393). Matching only `TEST_CASE`
# left `SCENARIO`, the `TEMPLATE_*` family and every `_METHOD` fixture variant
# invisible: a `[stress]` case written with any of them evaded the stray-tag
# rejection entirely, and a registration written with one did not count toward
# vendor coverage.
#
# The `_METHOD` variants put a fixture class name before the description, and
# `TEMPLATE_*_SIG` puts a signature after the tags; in every case the tags are
# the string-literal argument immediately following the description, so the
# pattern is "an optional non-string leading argument, then two consecutive
# runs of string literals".
CATCH_TEST_MACROS = (
    "TEST_CASE",
    "TEST_CASE_METHOD",
    "SCENARIO",
    "SCENARIO_METHOD",
    "TEMPLATE_TEST_CASE",
    "TEMPLATE_TEST_CASE_SIG",
    "TEMPLATE_TEST_CASE_METHOD",
    "TEMPLATE_TEST_CASE_METHOD_SIG",
    "TEMPLATE_PRODUCT_TEST_CASE",
    "TEMPLATE_PRODUCT_TEST_CASE_SIG",
    "TEMPLATE_PRODUCT_TEST_CASE_METHOD",
    "TEMPLATE_PRODUCT_TEST_CASE_METHOD_SIG",
    "TEMPLATE_LIST_TEST_CASE",
    "TEMPLATE_LIST_TEST_CASE_METHOD",
    "METHOD_AS_TEST_CASE",
    # Catch2 3.6+. find_package(Catch2 QUIET) in AlpacaCore/tests/CMakeLists.txt
    # is unpinned, so a runner with 3.6+ can compile one of these even though
    # nothing in the tree writes one today.
    "TEST_CASE_PERSISTENT_FIXTURE",
    # REGISTER_TEST_CASE(fn, "name", "[tags]") -- the tags sit in the same
    # third-argument position, behind one non-string leading argument.
    "REGISTER_TEST_CASE",
)
# Longest first, so TEST_CASE cannot shadow TEST_CASE_METHOD in the alternation.
_MACRO_ALTERNATION = "|".join(
    re.escape(m) for m in sorted(CATCH_TEST_MACROS, key=len, reverse=True))
_STRING_LITERAL_RUN = r'(?:"(?:[^"\\]|\\.)*"\s*)+'

# The description is matched as one or more adjacent string literals
# (escapes allowed, `"a" "b"` concatenation allowed) so a comma inside it
# cannot cut the match short and silently drop the tags.
#
# The TAGS argument is matched the same way, for the same reason (issue #393):
# `"[vendor][camera]" "[stress]"` is one string to the preprocessor and two
# adjacent literals to this scan, and a pattern accepting only a single
# literal saw `{vendor, camera}` and silently lost `stress` -- unregistering a
# real registration in one direction, and letting a stray `"[async]" "[stress]"`
# case evade the rejection in the other.
#
# Whitespace BETWEEN tags is allowed because Catch2 allows it: "[a][b]" and
# "[a] [b]" are the same two tags to Catch2, but a pattern demanding one
# unbroken run of brackets sees only the first form. That gap silently
# un-registers a case from this gate -- and from the [stress-guard] rejection
# below -- for a purely cosmetic difference in how someone typed the tags.
TEST_CASE_TAGS_RE = re.compile(
    r"\b(?:" + _MACRO_ALTERNATION + r")\s*\(\s*"
    r'(?:[^"(),]*,\s*)?'            # optional fixture class (the _METHOD variants)
    + _STRING_LITERAL_RUN + r",\s*"  # description
    r"(" + _STRING_LITERAL_RUN + r")")  # tags
STRING_LITERAL_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
RAW_STRING_RE = re.compile(r'"([^()\\ ]{0,16})\(.*?\)\1"', re.S)
TAG_RE = re.compile(r"\[([^\]]+)\]")


def tags_in_literal_run(run):
    """The lowercased Catch2 tags in a matched run of adjacent string literals.

    The literal bodies are concatenated first, the way the preprocessor does
    it, so `"[vendor][camera]" "[stress]"` yields all three tags and the quote
    characters between them cannot be mistaken for tag text.
    """
    joined = "".join(STRING_LITERAL_RE.findall(run))
    return {t.lower() for t in TAG_RE.findall(joined)}


def strip_comments(text):
    """`text` with C and C++ comments blanked out, preserving every offset.

    The tag scans below are plain text scans, and `TEST_GLOBS` covers
    `AlpacaCore/tests/*.h` -- including `concurrency_stress.h`, the
    documentation hub for this whole pattern. Without this, writing the
    illustrative line

        // TEST_CASE("MyVendor camera lifecycle", "[myvendor][camera][stress]")

    into a doc comment there failed CI for a registration that does not exist
    at runtime, and the same in reverse for a commented-out `[stress-guard]`
    example inside a registration file (issue #386). A warning comment was the
    stopgap; this is the filter.

    Comments are replaced with spaces rather than removed so that a match's
    offsets still line up with the original text, and string and character
    literals are tracked so that a `//` or `/*` *inside* one is left alone.
    Raw string literals (`R"delim(...)delim"`) are handled too: `//` is
    ordinary text inside one.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j == -1 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j == -1 else j + 2
            # Keep newlines so line-oriented reading of the result still works.
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c == '"' or c == "'":
            # A raw string literal is introduced by R immediately before the
            # quote (prefixes u8/u/U/L may precede the R).
            if c == '"' and i > 0 and text[i - 1] == "R":
                m = RAW_STRING_RE.match(text, i)
                if m:
                    out.append(m.group(0))
                    i = m.end()
                    continue
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == c or text[j] == "\n":
                    j += 1
                    break
                j += 1
            out.append(text[i:j])
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)

# (vendor, device type) pairs with no [stress] TEST_CASE yet. Seeded from the
# gap found when this check was introduced (2026-09) so the check starts
# green; each line is a driver this repo already knows is uncovered.
#
# Remove an entry the same PR that adds its [stress] coverage -- a
# still-covered entry left behind is itself a failure (see main()), so
# nothing here can silently go stale.
ALLOWLIST = {
    ("qhy", "camera"),
    ("qhy", "filterwheel"),
}


# Registration files that do NOT yet wrap their operate callbacks in
# StressCallGuard (issue #379; the migration itself is #326).
#
# The guard turns a swallowed exception into a counted, named failure.
# Without it, run_lifecycle_stress's own outer `catch (const std::exception&)`
# swallows everything a storm throws and the case passes regardless -- which is
# why this is a gate and not a style note.
#
# Default is MANDATORY: a new registration file must use the guard or be added
# here deliberately. Remove an entry in the same PR that migrates its file; a
# stale entry is itself a failure below, so nothing here can silently go stale.
# Empty since open-astro#326: every registration file now uses the guard. An
# entry here is a deliberate, explained exception -- and the stale-entry rule
# in check_guard_usage() means one left behind after its file is migrated is
# itself a failure, so this cannot quietly refill.
GUARD_ALLOWLIST = set()


# A hand-rolled call() wrapper in either form the registrations used:
#   static void call(const std::function<void()>& fn) {...}   (file scope)
#   template <typename F> void call(F&& fn) {...}             (file scope)
#   auto call = [](auto&& fn) {...};                           (lambda)
# The function form needs a return type (or `static`) before the name; the
# lambda form is `call` bound with `=` to a `[` capture list. A CALL SITE
# `call([&] {...})` matches neither, so only the definition is reported.
# `[^\S\n]` rather than `\s` inside the return-type class: the definition is
# one line, and a class that admits newlines could span into the next one.
LOCAL_CALL_HELPER_RE = re.compile(
    r"^\s*(?:static\s+)?\w[\w:<>,&*]*(?:[^\S\n][\w:<>,&*]*)*\bcall\s*\("
    r"|^\s*(?:static\s+)?(?:const\s+)?auto\s+call\s*=\s*\[",
    re.M,
)


def check_guard_usage():
    """StressCallGuard is used, and its result is actually asserted (issue #379).

    Three failure modes, in increasing order of how convincing they look:

    1. No guard at all. run_lifecycle_stress swallows exceptions in its own
       outer catch, so the storm passes no matter what the driver threw.
    2. A guard constructed and used, but the closing CHECK omitted. Worse than
       (1): the file LOOKS like it follows the documented pattern, and a reader
       scanning for StressCallGuard concludes the calls are checked.
    3. The CHECK present but INFO(report()) missing, so a real finding arrives
       as a bare `0 == 1` naming nothing it swallowed.

    Plus the vacuity hole (issue #334): unexpected_count() == 0 passes when the
    callback never ran at all, so a storm that silently stopped exercising the
    driver still reports a pass. total_calls() > 0 is what closes it.

    A local `call(...)` helper that wraps the same try/catch by hand is
    rejected outright: it is the shape the guard replaced, and it reintroduces
    the counting-without-failing problem one file at a time. Both forms the
    merged registrations used are caught: the file-scope function/template
    (`static void call(...)`, `template <...> void call(...)`) and the lambda
    (`auto call = [](auto&& fn) {...}`), which was four of the five.

    Reads the comment-stripped text, like rules 1-3: a closing CHECK that has
    been commented out must not satisfy the presence rules.
    """
    failures = []
    seen = set()
    for path in tracked_files(STRESS_TEST_GLOB):
        name = os.path.basename(path)
        seen.add(name)
        text = strip_comments(read_text(path))
        allowed = name in GUARD_ALLOWLIST
        uses_guard = "StressCallGuard" in text

        if not uses_guard:
            if not allowed:
                failures.append(
                    "NO STRESS GUARD: %s has no StressCallGuard, so "
                    "run_lifecycle_stress's outer catch swallows whatever the storm throws and the case "
                    "passes regardless. Wrap each call in the operate callback, or add '%s' to "
                    "GUARD_ALLOWLIST in %s with a reason." % (path, name, __file__)
                )
            continue

        if allowed:
            failures.append(
                "STALE GUARD ALLOWLIST ENTRY: %s now uses StressCallGuard -- remove '%s' from "
                "GUARD_ALLOWLIST in %s." % (path, name, __file__)
            )

        if not re.search(r"CHECK\s*\(\s*[\w.]*unexpected_count\s*\(\s*\)", text):
            failures.append(
                "GUARD NOT ASSERTED: %s constructs a StressCallGuard but never CHECKs "
                "unexpected_count(). Counting without failing is worse than not guarding: the file looks "
                "like it follows the documented pattern. End the TEST_CASE with the INFO/CHECK pair from "
                "AGENTS.md." % path
            )
        if not re.search(r"INFO\s*\(\s*[\w.]*report\s*\(\s*\)", text):
            failures.append(
                "GUARD REPORT NOT ATTACHED: %s CHECKs unexpected_count() without a preceding "
                "INFO(guard.report()). CHECK takes no message argument, so a real finding arrives as a "
                "bare `0 == 1` naming nothing it swallowed." % path
            )
        if not re.search(r"CHECK\s*\(\s*[\w.]*total_calls\s*\(\s*\)", text):
            failures.append(
                "GUARD COUNT VACUOUS: %s CHECKs unexpected_count() without also CHECKing total_calls(). "
                "A guard that was never invoked reports zero unexpected throws, exactly like one that saw "
                "a hundred clean calls, so a storm that silently stopped exercising the driver still "
                "passes (issue #334)." % path
            )
        if LOCAL_CALL_HELPER_RE.search(text):
            failures.append(
                "LOCAL call() HELPER: %s defines its own call() wrapper. That is the hand-rolled "
                "try/catch StressCallGuard replaced, and it reintroduces counting-without-failing one "
                "file at a time. Use the guard." % path
            )

    for name in sorted(GUARD_ALLOWLIST - seen):
        failures.append(
            "STALE GUARD ALLOWLIST ENTRY: '%s' matches no registration file anymore -- remove it from "
            "GUARD_ALLOWLIST in %s." % (name, __file__)
        )
    return failures


def read_text(path):
    """`path`'s contents. A seam the self-test patches, like tracked_files()."""
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def tracked_files(pattern):
    out = subprocess.run(
        ["git", "ls-files", pattern],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    ).stdout
    return [p for p in out.splitlines() if p]


def device_types_in_text(text):
    """The distinct device types text's get_device_type() override(s)
    return, lowercased. A file with two driver classes that both return the
    same type (e.g. gemini_flatpanel_driver.cpp) yields one value; text with
    no matching override at all yields none. Split out from
    driver_device_types() (file I/O) so a self-test can exercise this
    directly against synthetic snippets.
    """
    return {v.lower() for v in DEVICE_TYPE_OVERRIDE_RE.findall(text)}


def driver_device_types(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return device_types_in_text(fh.read())


def find_drivers():
    """({(vendor, device_type): [driver file paths]}, [ambiguous findings])"""
    drivers = {}
    ambiguous = []
    for path in tracked_files(VENDORS_PREFIX + "*_driver.cpp"):
        # AlpacaCore/src/vendors/<vendor>/<name>_driver.cpp
        parts = path[len(VENDORS_PREFIX):].split("/")
        if len(parts) != 2:
            continue
        vendor = parts[0]
        types = driver_device_types(path)
        if len(types) > 1:
            ambiguous.append(
                "AMBIGUOUS DEVICE TYPE: %s defines get_device_type() overrides "
                "returning disagreeing values %s -- fix the driver or this "
                "check's DEVICE_TYPE_OVERRIDE_RE, don't guess" % (path, sorted(types))
            )
            continue
        if not types:
            print("WARNING: could not determine device type for %s "
                  "(no get_device_type() override matched) -- treating as uncovered" % path)
            device_type = "unknown"
        else:
            device_type = next(iter(types))
        drivers.setdefault((vendor, device_type), []).append(path)
    return drivers, ambiguous


def stress_tag_sets_in_text(text):
    """[{tag, tag, ...}, ...] for every TEST_CASE in text tagged [stress].

    Split out from find_registered_pairs() (which also needs the known
    vendor/device-type vocabulary) so a self-test can exercise the raw
    TEST_CASE_TAGS_RE/TAG_RE extraction against synthetic snippets.
    """
    text = strip_comments(text)
    tag_sets = []
    for m in TEST_CASE_TAGS_RE.finditer(text):
        tags = tags_in_literal_run(m.group(1))
        if "stress" in tags:
            tag_sets.append(tags)
    return tag_sets


def find_registered_pairs(known_vendors, known_device_types):
    """({(vendor, device_type)} covered by a [stress] TEST_CASE, [guard findings]).

    Matches tags against the vendor/device-type vocabulary the driver scan
    itself found, rather than assuming a fixed tag order -- a TEST_CASE is
    tagged [vendor][device_type][stress] plus sometimes more (e.g.
    [round-4]), and this only needs to find the two tags that are actually a
    known vendor and a known device type.
    """
    registered = set()
    guard_failures = []
    # One pass over each registration file, collecting the [stress] pairs and
    # the [stress-guard]-without-[stress] rejections together, since both read
    # the same text. The stray-[stress] check is NOT folded in here: it scans a
    # different, wider set (TEST_GLOBS) and deliberately skips these files, so
    # it lives in find_stray_stress_cases() and re-globs from scratch.
    for path in tracked_files(STRESS_TEST_GLOB):
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        for tags in stress_tag_sets_in_text(text):
            vendor_tags = [t for t in tags if t in known_vendors]
            dtype_tags = [t for t in tags if t in known_device_types]
            for vendor in vendor_tags:
                for dtype in dtype_tags:
                    registered.add((vendor, dtype))
        for case in guard_tagged_cases_in_text(text):
            guard_failures.append(
                "[stress-guard] IN A REGISTRATION FILE: %s uses [stress-guard] "
                "without [stress], so it does not count toward vendor coverage "
                "here. Use [stress] for a vendor registration; [stress-guard] is "
                "for harness self-tests only: %s" % (path, case)
            )
    return registered, guard_failures


def guard_tagged_cases_in_text(text):
    """The matched TEST_CASE(...) headers tagged [stress-guard] but NOT [stress].

    Each entry is the whole matched prefix (name + tag string), not just the
    description -- that is what the failure message quotes, so a reader can
    see which tags were actually written.

    [stress-guard] exists for harness self-tests that need ThreadSanitizer but
    are not vendor registrations (issue #322); it runs under its own TSan
    invocation, deliberately outside the vendor-coverage count this script
    gates. A registration file that reached for it INSTEAD of [stress] would
    still get TSan, still look registered to a reader, and silently drop out
    of that count -- so the pair would go uncovered without appearing in
    ALLOWLIST. Reject the tag in these files; the harness self-test that owns
    it lives in test_stress_call_guard.cpp, which this glob never scans.
    """
    text = strip_comments(text)
    found = []
    for m in TEST_CASE_TAGS_RE.finditer(text):
        tags = tags_in_literal_run(m.group(1))
        if "stress-guard" in tags and "stress" not in tags:
            found.append(m.group(0))
    return found


def stray_stress_cases_in_text(text):
    """The matched TEST_CASE(...) headers tagged [stress] -- for files OUTSIDE
    the *_concurrency_stress.cpp glob, where that tag does not belong.

    This is the direction that actually went wrong: test_async_connectable.cpp
    carried [stress] from the unconditional TEST_SOURCES block, so with every
    vendor target absent `alpacacore_tests "[stress]"` still matched one case,
    printed "All tests passed (... in 1 test case)", and the CI zero-coverage
    grep accepted it -- sanitizers-tsan went green with no vendor concurrency
    coverage at all. The tag was moved to [stress-guard]; this check is what
    stops the next one being added.

    Core/harness self-tests that need TSan use [stress-guard], which has its
    own invocation and its own zero-test grep.
    """
    text = strip_comments(text)
    found = []
    for m in TEST_CASE_TAGS_RE.finditer(text):
        tags = tags_in_literal_run(m.group(1))
        if "stress" in tags:
            found.append(m.group(0))
    return found


def find_stray_stress_cases():
    """[findings] for [stress]-tagged TEST_CASEs outside the registration glob."""
    registrations = set(tracked_files(STRESS_TEST_GLOB))
    failures = []
    candidates = []
    for glob in TEST_GLOBS:
        candidates.extend(tracked_files(glob))
    for path in sorted(set(candidates)):
        if path in registrations:
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        for case in stray_stress_cases_in_text(text):
            failures.append(
                "[stress] OUTSIDE A REGISTRATION FILE: %s tags a TEST_CASE "
                "[stress], but that tag is reserved for vendor driver "
                "registrations in %s -- this file compiles unconditionally, "
                "so the case alone satisfies CI's vendor zero-coverage grep and "
                "makes it vacuous. Use [stress-guard] for a core/harness "
                "self-test that needs TSan: %s"
                % (path, STRESS_TEST_GLOB, case)
            )
    return failures


# `if(TARGET alpacacore_<vendor>)` gates a file on that VENDOR's target, which
# is what AGENTS.md tells authors to write. Three shapes look like gating and
# are not, and a bare `"TARGET" in line` accepts all three: `if(NOT TARGET x)`
# compiles the file exactly when the vendor is ABSENT, which is the opposite of
# gating; any identifier merely CONTAINING the word counts, and
# `if(CATCH2_MAIN_TARGET STREQUAL "")` is a live example in the very CMakeLists
# this parses; and a non-vendor target such as `if(TARGET Catch2::Catch2WithMain)`
# (line 11 of that file) is true in every build that compiles these tests at
# all, so a file parked there would pass the rule while still compiling in the
# vendor-less TSan build -- the exact hole this rule closes. Require TARGET as
# its own word naming an `alpacacore_` target, and reject a negated condition
# outright.
#
# Known limit: only the `if(` line itself is scanned, so a condition wrapped
# across lines reads as ungated. Nothing in the tree writes one.
_TARGET_GATE_RE = re.compile(r"\bTARGET\s+alpacacore_\S")
_NOT_TARGET_RE = re.compile(r"\bNOT\s+TARGET\b")


def _is_target_gate(condition):
    """True when `condition` gates on a target EXISTING."""
    if _NOT_TARGET_RE.search(condition):
        return False
    return bool(_TARGET_GATE_RE.search(condition))


def ungated_registration_files_in_cmake(cmake_text, registration_files):
    """[findings] for registration files not gated behind an `if(TARGET ...)`.

    The `sanitizers-tsan` job runs `alpacacore_tests "[stress]"` and rejects a
    run that executed zero test cases. That grep is the only thing standing
    between "no vendor concurrency coverage at all" and a green job, and it
    works only while EVERY `[stress]` case lives in a file that compiles
    conditionally, behind `if(TARGET alpacacore_<vendor>)` in
    `AlpacaCore/tests/CMakeLists.txt`. Then a vendor-less build has no
    `[stress]` cases, the run reports zero, and the grep fails the job.

    find_stray_stress_cases() closes this from one side: a `[stress]` case in a
    file outside the registration glob. This is the other side (issue #396).
    Append a registration file to the unconditional `TEST_SOURCES` block and
    every other check here passes -- right name, right tag, pair counted as
    covered -- while the vendor-less TSan build now has one `[stress]` case,
    the zero-test grep is satisfied by it, and the job goes green with no
    vendor concurrency coverage. That is the exact failure mode the gate
    exists to prevent, reached from the other direction.

    A text scan is adequate and matches the rest of this script: track the
    `if(`/`endif()` nesting line by line and record, for each line, whether any
    enclosing condition mentions `TARGET`. `elseif`/`else` are treated as still
    inside the same block. A file named in CMakeLists but not tracked by git is
    not this check's business, and a tracked file named nowhere in CMakeLists
    is reported too: it compiles into nothing, so its registration is dead.
    """
    seen = {}
    depth_is_target = []
    for raw in cmake_text.splitlines():
        line = raw.split("#", 1)[0]
        stripped = line.strip()
        lowered = stripped.lower()
        if lowered.startswith("if(") or lowered.startswith("if ("):
            depth_is_target.append(_is_target_gate(stripped))
        elif lowered.startswith("elseif(") or lowered.startswith("elseif ("):
            if depth_is_target:
                # An elseif arm of a `if(TARGET ...)` block is NOT itself
                # gated on that target, so it only counts when it names one.
                depth_is_target[-1] = _is_target_gate(stripped)
        elif lowered.startswith("else(") or lowered.startswith("else ("):
            if depth_is_target:
                depth_is_target[-1] = False
        elif lowered.startswith("endif"):
            if depth_is_target:
                depth_is_target.pop()
        gated = any(depth_is_target)
        # Any line naming the file counts as a listing, and `seen` is keyed on
        # the basename. Both are fine for the shape this CMakeLists has (one flat
        # directory, sources named only by set()/list(APPEND)), but a line like
        # set_source_files_properties(<file> PROPERTIES ...) outside the
        # if(TARGET ...) block would read as an ungated listing and fail a
        # correctly-gated file. Nothing in the tree does that; widen this to
        # match only source-listing commands if it ever appears.
        for name in re.findall(r"[A-Za-z0-9_./-]+" + re.escape(STRESS_TEST_SUFFIX), line):
            base = name.rsplit("/", 1)[-1]
            # EVERY mention must be gated, not just one of them. CMake
            # de-duplicates a source named twice, so a file listed once inside
            # if(TARGET ...) and once in the unconditional block still compiles
            # with every vendor target absent -- the build stays green and
            # silent, and the ungated mention is the one that decides. An `or`
            # here let the gated mention mask the ungated one and re-opened the
            # exact hole this rule closes.
            seen[base] = seen.get(base, True) and gated

    failures = []
    for path in sorted(registration_files):
        base = path.rsplit("/", 1)[-1]
        if base not in seen:
            failures.append(
                "REGISTRATION FILE NOT IN %s: %s is tracked and matches %s but "
                "is named nowhere in the tests CMakeLists, so it compiles into "
                "nothing and its [stress] coverage is dead. Add it inside the "
                "vendor's if(TARGET alpacacore_<vendor>) block."
                % (TESTS_CMAKELISTS, path, STRESS_TEST_GLOB))
        elif not seen[base]:
            failures.append(
                "REGISTRATION FILE NOT VENDOR-GATED: %s is added to TEST_SOURCES "
                "in %s outside any if(TARGET ...) block, so it compiles even "
                "with every vendor target absent. One [stress] case in a "
                "vendor-less build satisfies the sanitizers-tsan zero-test grep "
                "and makes it vacuous. Move it inside the vendor's "
                "if(TARGET alpacacore_<vendor>) block."
                % (path, TESTS_CMAKELISTS))
    return failures


def find_ungated_registration_files():
    """[findings] for registration files that compile unconditionally."""
    registrations = tracked_files(STRESS_TEST_GLOB)
    if not registrations:
        return []
    return ungated_registration_files_in_cmake(read_text(TESTS_CMAKELISTS),
                                               registrations)


def main():
    drivers, failures = find_drivers()
    failures = list(failures)  # find_drivers' own ambiguity findings, if any
    known_vendors = {v for v, _ in drivers}
    known_device_types = {d for _, d in drivers}
    registered, guard_failures = find_registered_pairs(known_vendors, known_device_types)
    failures.extend(guard_failures)
    failures.extend(find_stray_stress_cases())
    failures.extend(find_ungated_registration_files())
    failures.extend(check_guard_usage())

    for (vendor, dtype), paths in sorted(drivers.items()):
        covered = (vendor, dtype) in registered
        allowed = (vendor, dtype) in ALLOWLIST
        if not covered and not allowed:
            failures.append(
                "MISSING: %s/%s has no [stress] TEST_CASE and is not in "
                "ALLOWLIST (%s): %s"
                % (vendor, dtype, __file__, ", ".join(paths))
            )
        if covered and allowed:
            failures.append(
                "STALE ALLOWLIST ENTRY: %s/%s now has [stress] coverage -- "
                "remove ('%s', '%s') from ALLOWLIST in %s"
                % (vendor, dtype, vendor, dtype, __file__)
            )

    driver_pairs = set(drivers)
    for vendor, dtype in sorted(ALLOWLIST - driver_pairs):
        failures.append(
            "STALE ALLOWLIST ENTRY: %s/%s has no matching driver anymore -- "
            "remove ('%s', '%s') from ALLOWLIST in %s"
            % (vendor, dtype, vendor, dtype, __file__)
        )

    if failures:
        print("Stress-test registration check failed:\n")
        for f in failures:
            print("  " + f)
        print("\n%d finding(s)." % len(failures))
        return 1

    print("Stress-test registration OK -- %d driver/device-type pairs checked, "
          "%d allow-listed as not-yet-covered." % (len(drivers), len(ALLOWLIST)))
    return 0


def self_test():
    """Regression guard for this script's own regexes, run with --self-test.

    Exercises device_types_in_text() and stress_tag_sets_in_text() against
    synthetic snippets so a future edit to either regex gets caught here
    instead of only showing up as a silently wrong (vendor, device_type)
    pair. Not run as part of the normal check (no repo state needed).

    It also drives main() end to end over temp files with tracked_files()
    patched, which covers the two tag predicates AND their wiring: deleting
    either `failures.extend(...)` line in main() fails a check here rather
    than passing silently. Keep that property when adding cases -- a predicate
    tested only through its own function leaves the wiring unpinned.
    """
    checks = []

    def check(name, condition):
        checks.append((name, condition))

    # A DeviceType:: mention earlier in the file (a comment, here) must not
    # be picked up ahead of the actual override.
    decoy = """
        // Historically this was DeviceType::Camera before a refactor.
        class Foo : public FocuserDriver {
        public:
            DeviceType get_device_type() const override { return DeviceType::Focuser; }
        };
    """
    check("decoy DeviceType:: mention is ignored", device_types_in_text(decoy) == {"focuser"})

    # Two classes in one file agreeing is fine (the real gemini_flatpanel_driver.cpp shape).
    agree = """
        class A : public CoverCalibratorDriver {
            DeviceType get_device_type() const override { return DeviceType::CoverCalibrator; }
        };
        class B : public CoverCalibratorDriver {
            DeviceType get_device_type() const override { return DeviceType::CoverCalibrator; }
        };
    """
    check("two classes agreeing yields one value", device_types_in_text(agree) == {"covercalibrator"})

    # Two classes disagreeing must be flagged as ambiguous (len > 1), not
    # resolved by picking whichever comes first.
    disagree = """
        class A : public FocuserDriver {
            DeviceType get_device_type() const override { return DeviceType::Focuser; }
        };
        class B : public SwitchDriver {
            DeviceType get_device_type() const override { return DeviceType::Switch; }
        };
    """
    check("disagreeing overrides are detected as ambiguous", len(device_types_in_text(disagree)) > 1)

    # No override at all.
    check("no override yields no types", device_types_in_text("// nothing here") == set())

    # A comma inside the TEST_CASE description must not cut the tag match
    # short (the exact bug fixed for #269's non-blocking review note).
    comma_desc = 'TEST_CASE("Foo, bar - baz", "[vendor][focuser][stress]") {}'
    check("a comma in the description doesn't drop the tags",
          stress_tag_sets_in_text(comma_desc) == [{"vendor", "focuser", "stress"}])

    # String-literal concatenation ("a" "b") in the description.
    concat_desc = 'TEST_CASE("Foo" " - bar", "[vendor][focuser][stress]") {}'
    check("concatenated string literals in the description still match",
          stress_tag_sets_in_text(concat_desc) == [{"vendor", "focuser", "stress"}])

    # A TEST_CASE with no [stress] tag must not be picked up.
    non_stress = 'TEST_CASE("Foo", "[vendor][focuser][unit]") {}'
    check("a non-[stress] TEST_CASE is excluded", stress_tag_sets_in_text(non_stress) == [])

    # [stress-guard] must not stand in for [stress] in a registration file:
    # it gets its own TSan invocation but is deliberately outside the vendor
    # coverage count, so a registration wearing it would silently go
    # uncovered while still looking registered.
    guard_only = 'TEST_CASE("Foo", "[vendor][focuser][stress-guard]") {}'
    check("a [stress-guard]-only TEST_CASE is rejected in a registration file",
          len(guard_tagged_cases_in_text(guard_only)) == 1)
    check("a [stress-guard]-only TEST_CASE is not counted as [stress] coverage",
          stress_tag_sets_in_text(guard_only) == [])

    # Carrying both is fine -- the case still counts as vendor coverage, and
    # the extra tag only adds it to the second TSan invocation.
    both_tags = 'TEST_CASE("Foo", "[vendor][focuser][stress][stress-guard]") {}'
    check("a TEST_CASE tagged both [stress] and [stress-guard] is allowed",
          guard_tagged_cases_in_text(both_tags) == [])
    check("a TEST_CASE tagged both still counts as [stress] coverage",
          stress_tag_sets_in_text(both_tags) == [{"vendor", "focuser", "stress", "stress-guard"}])

    # Catch2 treats "[a][b]" and "[a] [b]" as the same two tags, so a pattern
    # demanding one unbroken bracket run would silently un-register a case
    # over a cosmetic difference -- and would let a spaced [stress-guard] slip
    # past the rejection above.
    spaced = 'TEST_CASE("Foo", "[vendor] [focuser] [stress]") {}'
    check("whitespace between tags still registers as [stress] coverage",
          stress_tag_sets_in_text(spaced) == [{"vendor", "focuser", "stress"}])
    spaced_guard = 'TEST_CASE("Foo", "[vendor] [focuser]  [stress-guard]") {}'
    check("whitespace between tags does not let [stress-guard] evade the check",
          len(guard_tagged_cases_in_text(spaced_guard)) == 1)

    # The other direction, which is the one that actually went wrong: a
    # [stress] tag in a file outside the registration glob re-inflates the
    # vendor threshold and makes CI's zero-coverage grep vacuous again.
    stray = 'TEST_CASE("Foo", "[async_connectable][stress]") {}'
    check("a [stress] TEST_CASE outside the glob is rejected",
          len(stray_stress_cases_in_text(stray)) == 1)
    guarded = 'TEST_CASE("Foo", "[async_connectable][stress-guard]") {}'
    check("a [stress-guard] TEST_CASE outside the glob is fine",
          stray_stress_cases_in_text(guarded) == [])
    unrelated = 'TEST_CASE("Foo", "[async_connectable][unit]") {}'
    check("an untagged-for-stress TEST_CASE outside the glob is fine",
          stray_stress_cases_in_text(unrelated) == [])

    # Concatenated TAG literals: "[a][b]" "[stress]" is one string to the
    # preprocessor, and a pattern accepting only a single literal silently
    # dropped the second (issue #393).
    concat_tags = 'TEST_CASE("Foo", "[vendor][focuser]" "[stress]") {}'
    check("concatenated tag literals are joined before the tags are read",
          stress_tag_sets_in_text(concat_tags) == [{"vendor", "focuser", "stress"}])
    concat_stray = 'TEST_CASE("Foo", "[async]" "[stress]") {}'
    check("a stray case with concatenated tag literals is still rejected",
          len(stray_stress_cases_in_text(concat_stray)) == 1)

    # Catch2's other registration macros take their tags in the same place, so
    # a [stress] case written with any of them must be seen (issue #393).
    check("SCENARIO registers as [stress] coverage",
          stress_tag_sets_in_text('SCENARIO("Foo", "[vendor][focuser][stress]") {}')
          == [{"vendor", "focuser", "stress"}])
    check("TEST_CASE_METHOD's fixture argument does not hide the tags",
          stress_tag_sets_in_text(
              'TEST_CASE_METHOD(MyFixture, "Foo", "[vendor][focuser][stress]") {}')
          == [{"vendor", "focuser", "stress"}])
    check("METHOD_AS_TEST_CASE's qualified method name does not hide the tags",
          stress_tag_sets_in_text(
              'METHOD_AS_TEST_CASE(MyFixture::run, "Foo", "[vendor][focuser][stress]")')
          == [{"vendor", "focuser", "stress"}])
    check("TEST_CASE_PERSISTENT_FIXTURE registers as [stress] coverage",
          stress_tag_sets_in_text(
              'TEST_CASE_PERSISTENT_FIXTURE(MyFixture, "Foo", "[vendor][focuser][stress]") {}')
          == [{"vendor", "focuser", "stress"}])
    check("REGISTER_TEST_CASE registers as [stress] coverage",
          stress_tag_sets_in_text(
              'REGISTER_TEST_CASE(myFn, "Foo", "[vendor][focuser][stress]");')
          == [{"vendor", "focuser", "stress"}])
    check("TEMPLATE_TEST_CASE registers as [stress] coverage",
          stress_tag_sets_in_text(
              'TEMPLATE_TEST_CASE("Foo", "[vendor][focuser][stress]", int, long) {}')
          == [{"vendor", "focuser", "stress"}])
    check("a SCENARIO tagged [stress] outside the glob is rejected",
          len(stray_stress_cases_in_text('SCENARIO("Foo", "[async][stress]") {}')) == 1)
    check("a [stress-guard]-only TEST_CASE_METHOD is rejected in a registration file",
          len(guard_tagged_cases_in_text(
              'TEST_CASE_METHOD(Fix, "Foo", "[vendor][focuser][stress-guard]") {}')) == 1)

    # Comment awareness (issue #386): an illustrative TEST_CASE in a doc
    # comment is documentation, not a registration, and must not fail CI.
    commented = '// TEST_CASE("Example", "[myvendor][camera][stress]") { ... }'
    check("a TEST_CASE inside a // comment is not a stray [stress] case",
          stray_stress_cases_in_text(commented) == [])
    check("a TEST_CASE inside a // comment is not [stress] coverage either",
          stress_tag_sets_in_text(commented) == [])
    block_commented = '/*\nTEST_CASE("Example", "[myvendor][camera][stress]") {}\n*/'
    check("a TEST_CASE inside a block comment is not a stray [stress] case",
          stray_stress_cases_in_text(block_commented) == [])
    commented_guard = '// TEST_CASE("Example", "[vendor][focuser][stress-guard]") {}'
    check("a commented-out [stress-guard] example is not rejected",
          guard_tagged_cases_in_text(commented_guard) == [])
    # ...and a real case sitting next to a commented-out one is still seen.
    mixed_comment = (commented + "\n"
                     'TEST_CASE("Real", "[vendor][focuser][stress]") {}')
    check("a real case beside a commented-out one is still registered",
          stress_tag_sets_in_text(mixed_comment) == [{"vendor", "focuser", "stress"}])
    # A // inside a string literal does not start a comment.
    url_in_string = ('TEST_CASE("See https://example.com/x", '
                     '"[vendor][focuser][stress]") {}')
    check("a // inside a string literal does not blank the rest of the line",
          stress_tag_sets_in_text(url_in_string) == [{"vendor", "focuser", "stress"}])
    # NOT pinned here, deliberately: the raw-string and char-literal branches of
    # strip_comments() have no observable effect on any input tried. The literal
    # scanner stops at a newline as well as at the closing quote, so it re-syncs
    # at every line break and a mangled literal cannot reach a TEST_CASE on a
    # later line. Probed by deleting each branch in turn and re-running this
    # suite plus same-line cases (a quote char literal, a raw string containing
    # both a quote and a //, and a C++14 digit separator): identical tag sets
    # every time. A check written against them would pass with the branch
    # deleted -- the fake coverage this gate exists to remove -- so the honest
    # record is this comment. Tracked for a real pin if the scanner ever stops
    # breaking on newlines.

    # The CMake gating rule (issue #396), at the predicate level.
    gated = ("if(TARGET alpacacore_zwo)\n"
             "    list(APPEND TEST_SOURCES test_zwo_concurrency_stress.cpp)\n"
             "endif()\n")
    regs = ["AlpacaCore/tests/test_zwo_concurrency_stress.cpp"]
    check("a registration file inside if(TARGET ...) passes",
          ungated_registration_files_in_cmake(gated, regs) == [])
    ungated = "list(APPEND TEST_SOURCES test_zwo_concurrency_stress.cpp)\n"
    check("a registration file outside any if(TARGET ...) is rejected",
          len(ungated_registration_files_in_cmake(ungated, regs)) == 1)
    absent = "list(APPEND TEST_SOURCES test_zwo_camera.cpp)\n"
    check("a registration file named nowhere in CMakeLists is rejected",
          len(ungated_registration_files_in_cmake(absent, regs)) == 1)
    non_target_if = ("if(BUILD_TESTING)\n"
                     "    list(APPEND TEST_SOURCES test_zwo_concurrency_stress.cpp)\n"
                     "endif()\n")
    check("an if() that names no TARGET does not count as gating",
          len(ungated_registration_files_in_cmake(non_target_if, regs)) == 1)
    nested = ("if(TARGET alpacacore_zwo)\n"
              "    if(SOMETHING_ELSE)\n"
              "        list(APPEND TEST_SOURCES test_zwo_concurrency_stress.cpp)\n"
              "    endif()\n"
              "endif()\n")
    check("a registration file nested deeper inside a TARGET block passes",
          ungated_registration_files_in_cmake(nested, regs) == [])
    else_arm = ("if(TARGET alpacacore_zwo)\n"
                "else()\n"
                "    list(APPEND TEST_SOURCES test_zwo_concurrency_stress.cpp)\n"
                "endif()\n")
    check("the else() arm of an if(TARGET ...) block does not count as gating",
          len(ungated_registration_files_in_cmake(else_arm, regs)) == 1)
    else_arm_spaced = ("if(TARGET alpacacore_zwo)\n"
                       "else ()\n"
                       "    list(APPEND TEST_SOURCES test_zwo_concurrency_stress.cpp)\n"
                       "endif()\n")
    check("the spaced `else ()` arm does not count as gating either",
          len(ungated_registration_files_in_cmake(else_arm_spaced, regs)) == 1)
    commented_cmake = ("# list(APPEND TEST_SOURCES test_zwo_concurrency_stress.cpp)\n"
                       + gated)
    check("a commented CMake line does not stand in for a real one",
          ungated_registration_files_in_cmake(commented_cmake, regs) == [])
    both_listed = ("set(TEST_SOURCES\n"
                   "    test_zwo_concurrency_stress.cpp\n"
                   ")\n"
                   + gated)
    check("a file listed BOTH ungated and gated is still rejected",
          len(ungated_registration_files_in_cmake(both_listed, regs)) == 1)
    both_listed_reversed = (gated
                            + "set(TEST_SOURCES\n"
                              "    test_zwo_concurrency_stress.cpp\n"
                              ")\n")
    check("the gated mention cannot mask a later ungated one either",
          len(ungated_registration_files_in_cmake(both_listed_reversed, regs)) == 1)
    twice_gated = gated + gated
    check("a file listed twice, both times gated, still passes",
          ungated_registration_files_in_cmake(twice_gated, regs) == [])
    not_target = ("if(NOT TARGET alpacacore_zwo)\n"
                  "    list(APPEND TEST_SOURCES test_zwo_concurrency_stress.cpp)\n"
                  "endif()\n")
    check("if(NOT TARGET ...) is not gating -- it compiles when the vendor is absent",
          len(ungated_registration_files_in_cmake(not_target, regs)) == 1)
    target_substring = ('if(CATCH2_MAIN_TARGET STREQUAL "")\n'
                        "    list(APPEND TEST_SOURCES test_zwo_concurrency_stress.cpp)\n"
                        "endif()\n")
    check("an identifier merely containing TARGET does not count as gating",
          len(ungated_registration_files_in_cmake(target_substring, regs)) == 1)
    non_vendor_target = ("if(TARGET Catch2::Catch2WithMain)\n"
                         "    list(APPEND TEST_SOURCES test_zwo_concurrency_stress.cpp)\n"
                         "endif()\n")
    check("a non-vendor if(TARGET ...) does not count as gating",
          len(ungated_registration_files_in_cmake(non_vendor_target, regs)) == 1)

    # The predicate above is well covered, but main()'s USE of it was not:
    # deleting `failures.extend(guard_failures)` left every check green. Drive
    # main() end to end over synthetic files so the wiring is pinned too.
    import os
    import tempfile

    real_tracked_files = globals()["tracked_files"]
    real_read_text = globals()["read_text"]
    with tempfile.TemporaryDirectory() as tmp:
        # Deliberately WITHOUT the `test_` prefix: this is the issue #376
        # behaviour -- a file following the documented
        # `*_concurrency_stress.cpp` naming counts toward vendor coverage
        # instead of being rejected as a stray [stress] case. Reverting
        # STRESS_TEST_GLOB to the old `test_*` form must fail these checks,
        # which it cannot do if the fixture's own file carries the prefix.
        stress = os.path.join(tmp, "fakevendor_concurrency_stress.cpp")
        core = os.path.join(tmp, "test_fakecore.cpp")

        def fake_tracked_files(pattern):
            # No drivers at all: with nothing to be uncovered, a non-zero exit
            # can ONLY come from the tag wiring under test. Returning []
            # says that outright rather than relying on a temp path happening
            # to be shallow enough that find_drivers() skips it.
            if pattern.endswith("_driver.cpp"):
                return []
            # The broad .cpp glob must return the non-registration file too --
            # otherwise find_stray_stress_cases() skips everything it is handed
            # (all of it is in `registrations`) and returns [] no matter what,
            # leaving its main() wiring untestable. That was a real hole:
            # deleting `failures.extend(find_stray_stress_cases())` left every
            # check here green until this fixture grew the second file.
            #
            # Keyed on the pattern itself rather than TEST_GLOBS[0]: reordering
            # that tuple would otherwise silently hand the .cpp glob [] and
            # re-open exactly the hole described above.
            if pattern == "AlpacaCore/tests/*.cpp":
                return [stress, core]
            if pattern in TEST_GLOBS:
                return []
            # The literal glob, not STRESS_TEST_GLOB: keying on the constant
            # makes this fixture match whatever the constant happens to be, so
            # a revert of the issue #376 widening would keep every check green
            # (it did -- all 43 passed against the old `test_*` glob).
            if pattern == "AlpacaCore/tests/*_concurrency_stress.cpp":
                return [stress]
            # Every glob the script asks for is named above. Fail loudly on a
            # new one rather than falling through: a silent default would hand
            # it the registration file and the checks here would keep passing
            # while covering less than they claim.
            raise AssertionError(
                "self-test fixture has no case for glob %r -- add one" % pattern)

        # The CMake gating rule reads the real tests CMakeLists, which knows
        # nothing about this fixture's temp file. Hand main() a synthetic one
        # instead, gated by default so the other rules are what the fixtures
        # below are actually testing; `cmake_source` overrides it for the two
        # cases that test the gating rule itself.
        gated_cmake = (
            "set(TEST_SOURCES test_core.cpp)\n"
            "if(TARGET alpacacore_fakevendor)\n"
            "    list(APPEND TEST_SOURCES fakevendor_concurrency_stress.cpp)\n"
            "endif()\n")

        def run_main_with(stress_source, core_source="", cmake_source=None):
            with open(stress, "w", encoding="utf-8") as fh:
                fh.write(stress_source + "\n")
            with open(core, "w", encoding="utf-8") as fh:
                fh.write(core_source + "\n")
            cmake_text = gated_cmake if cmake_source is None else cmake_source
            globals()["tracked_files"] = fake_tracked_files

            # Only the CMakeLists read is synthetic. It used to return
            # cmake_text for EVERY path, which silently handed the guard rules
            # (issue #379) the CMake text instead of the fixture's own source,
            # so they could never see the StressCallGuard the fixture writes.
            # Any rule that reads a TEST file needs the real bytes.
            def fake_read_text(path):
                if path in (stress, core):
                    return real_read_text(path)
                return cmake_text

            globals()["read_text"] = fake_read_text
            saved_allowlist = set(ALLOWLIST)
            ALLOWLIST.clear()
            # Cleared for the same reason as ALLOWLIST: the fixture's tree
            # holds one synthetic registration file, so every real entry would
            # otherwise report as stale and drown the finding under test.
            saved_guard_allowlist = set(GUARD_ALLOWLIST)
            GUARD_ALLOWLIST.clear()
            try:
                return main()
            finally:
                globals()["tracked_files"] = real_tracked_files
                globals()["read_text"] = real_read_text
                ALLOWLIST.clear()
                ALLOWLIST.update(saved_allowlist)
                GUARD_ALLOWLIST.clear()
                GUARD_ALLOWLIST.update(saved_guard_allowlist)

        # Guard-compliant (issue #379): the guard rules are mandatory by
        # default, and this fixture's file is deliberately NOT in
        # GUARD_ALLOWLIST -- so the baseline "passes" case has to carry the
        # full documented idiom. That also means every other case below
        # inherits it, and a regression in the guard rules shows up here
        # rather than only against the real tree.
        GUARDED_BODY = (
            "  alpacacore::test::StressCallGuard guard;\n"
            "  INFO(guard.report());\n"
            "  CHECK(guard.unexpected_count() == 0);\n"
            "  CHECK(guard.total_calls() > 0);\n"
        )
        clean = 'TEST_CASE("Ok", "[fakevendor][camera][stress]") {\n' + GUARDED_BODY + "}"
        check("main() passes when a registration file uses [stress]",
              run_main_with(clean) == 0)
        offending = 'TEST_CASE("Bad", "[fakevendor][camera][stress-guard]") {\n' + GUARDED_BODY + "}"
        check("main() FAILS when a registration file uses [stress-guard] alone",
              run_main_with(offending) == 1)
        # Pins that the rejection is PER CASE, not per file. Every other
        # fixture here holds a single TEST_CASE, so a per-file implementation
        # (union the file's tags, then test) would pass all of them
        # identically while letting this one through -- the file as a whole
        # carries [stress], but the second case alone does not, and that case
        # is the one that silently drops out of the vendor coverage count.
        # The first case carries the full idiom so the guard rules (rule 5)
        # are satisfied and the ONLY thing left to fail on is the tag: with
        # two empty bodies this fixture failed on NO STRESS GUARD instead, and
        # deleting both tag rules left it green (review finding on PR #465).
        mixed = ('TEST_CASE("Ok", "[fakevendor][camera][stress]") {\n' + GUARDED_BODY + "}\n"
                 'TEST_CASE("Bad", "[fakevendor][camera][stress-guard]") {}')
        check("main() FAILS on a [stress-guard]-only case beside a [stress] one",
              run_main_with(mixed) == 1)

        # The sibling wiring: a [stress] case in a NON-registration file is the
        # regression that made CI's vendor zero-coverage grep vacuous.
        stray = 'TEST_CASE("Stray", "[fakecore][stress]") {}'
        check("main() FAILS when a non-registration file uses [stress]",
              run_main_with(clean, stray) == 1)
        guarded = 'TEST_CASE("Fine", "[fakecore][stress-guard]") {}'
        check("main() passes when a non-registration file uses [stress-guard]",
              run_main_with(clean, guarded) == 0)

        # The guard rules and their wiring into main() (issues #379, #334).
        # Each drops ONE line from the documented idiom, so a rule that stops
        # firing is caught here rather than only when a real registration
        # quietly loses its assertion.
        no_guard = 'TEST_CASE("Ok", "[fakevendor][camera][stress]") {}'
        check("main() FAILS when a registration file has no StressCallGuard",
              run_main_with(no_guard) == 1)

        unasserted = ('TEST_CASE("Ok", "[fakevendor][camera][stress]") {\n'
                      "  alpacacore::test::StressCallGuard guard;\n"
                      "  INFO(guard.report());\n"
                      "  CHECK(guard.total_calls() > 0);\n}")
        check("main() FAILS when a guard's unexpected_count() is never CHECKed",
              run_main_with(unasserted) == 1)

        no_info = ('TEST_CASE("Ok", "[fakevendor][camera][stress]") {\n'
                   "  alpacacore::test::StressCallGuard guard;\n"
                   "  CHECK(guard.unexpected_count() == 0);\n"
                   "  CHECK(guard.total_calls() > 0);\n}")
        check("main() FAILS when INFO(guard.report()) is missing",
              run_main_with(no_info) == 1)

        vacuous = ('TEST_CASE("Ok", "[fakevendor][camera][stress]") {\n'
                   "  alpacacore::test::StressCallGuard guard;\n"
                   "  INFO(guard.report());\n"
                   "  CHECK(guard.unexpected_count() == 0);\n}")
        check("main() FAILS when total_calls() is not CHECKed (vacuous zero)",
              run_main_with(vacuous) == 1)

        local_call = ('TEST_CASE("Ok", "[fakevendor][camera][stress]") {\n'
                      + GUARDED_BODY +
                      "}\n"
                      "static void call(const std::function<void()>& fn) { try { fn(); } catch (...) {} }")
        check("main() FAILS when a registration file defines its own call() helper",
              run_main_with(local_call) == 1)
        # The lambda form, which four of the five merged helpers used; the
        # first regex needed `call(` directly and never matched `call = [`.
        local_call_lambda = ('TEST_CASE("Ok", "[fakevendor][camera][stress]") {\n'
                             "  auto call = [](auto&& fn) { try { fn(); } catch (...) {} };\n"
                             + GUARDED_BODY +
                             "}\n")
        check("main() FAILS when a registration file defines a call() lambda",
              run_main_with(local_call_lambda) == 1)
        # A call SITE is not a definition: the guard's own invocation style
        # and any helper named call() from a header must not trip the rule.
        call_site = ('TEST_CASE("Ok", "[fakevendor][camera][stress]") {\n'
                     "  guard([&] { call(1); });\n"
                     + GUARDED_BODY +
                     "}\n")
        check("main() passes when call() is only invoked, not defined",
              run_main_with(call_site) == 0)
        # Rule 5 reads comment-stripped text, like rules 1-3: a closing CHECK
        # that has been commented out must not count as present.
        commented_out = ('TEST_CASE("Ok", "[fakevendor][camera][stress]") {\n'
                         "  alpacacore::test::StressCallGuard guard;\n"
                         "  INFO(guard.report());\n"
                         "  CHECK(guard.unexpected_count() == 0);\n"
                         "  // CHECK(guard.total_calls() > 0);\n}")
        check("main() FAILS when the total_calls() CHECK is commented out",
              run_main_with(commented_out) == 1)

        # The CMake gating rule's own wiring into main() (issue #396).
        ungated_cmake = ("set(TEST_SOURCES test_core.cpp\n"
                         "    fakevendor_concurrency_stress.cpp)\n")
        check("main() FAILS when a registration file compiles unconditionally",
              run_main_with(clean, "", ungated_cmake) == 1)
        check("main() FAILS when a registration file is in no CMakeLists at all",
              run_main_with(clean, "", "set(TEST_SOURCES test_core.cpp)\n") == 1)

    failed = [name for name, ok in checks if not ok]
    for name, ok in checks:
        print("[%s] %s" % ("PASS" if ok else "FAIL", name))
    if failed:
        print("\n%d/%d self-test(s) failed." % (len(failed), len(checks)))
        return 1
    print("\nAll %d self-test(s) passed." % len(checks))
    return 0


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    sys.exit(main())
